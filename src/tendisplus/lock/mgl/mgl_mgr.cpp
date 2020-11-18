#include <utility>
#include "tendisplus/utils/invariant.h"
#include "tendisplus/lock/mgl/mgl_mgr.h"
#include "tendisplus/lock/mgl/mgl.h"
#include "tendisplus/lock/mgl/lock_defines.h"

namespace tendisplus {
namespace mgl {

const char* lockModeRepr(LockMode mode) {
  switch (mode) {
    case LockMode::LOCK_X:
      return "X";
    case LockMode::LOCK_IX:
      return "IX";
    case LockMode::LOCK_S:
      return "S";
    case LockMode::LOCK_IS:
      return "IS";
    default:
      return "?";
  }
}

bool isConflict(uint16_t modes, LockMode mode) {
  static uint16_t x = 1 << enum2Int(LockMode::LOCK_X);
  static uint16_t s = 1 << enum2Int(LockMode::LOCK_S);
  static uint16_t ix = 1 << enum2Int(LockMode::LOCK_IX);
  static uint16_t is = 1 << enum2Int(LockMode::LOCK_IS);
  static uint16_t conflictTable[enum2Int(LockMode::LOCK_MODE_NUM)] = {
    0,                                       // NONE
    static_cast<uint16_t>(x),                // IS
    static_cast<uint16_t>(s | x),            // IX
    static_cast<uint16_t>(ix | x),           // S
    static_cast<uint16_t>(is | ix | s | x),  // x
  };
  uint16_t modeInt = enum2Int(mode);
  return (conflictTable[modeInt] & modes) != 0;
}

LockSchedCtx::LockSchedCtx()
  : _runningModes(0),
    _pendingModes(0),
    _runningRefCnt(enum2Int(LockMode::LOCK_MODE_NUM), 0),
    _pendingRefCnt(enum2Int(LockMode::LOCK_MODE_NUM), 0) {}

// NOTE(deyukong): if compitable locks come endlessly,
// and we always schedule compitable locks first.
// Then the _pendingList will have no chance to schedule.
void LockSchedCtx::lock(MGLock* core) {
  auto mode = core->getMode();
  if (isConflict(_runningModes, mode) || _pendingList.size() >= 1) {
    auto it = _pendingList.insert(_pendingList.end(), core);
    incrPendingRef(mode);
    core->setLockResult(LockRes::LOCKRES_WAIT, it);
  } else {
    auto it = _runningList.insert(_runningList.end(), core);
    incrRunningRef(mode);
    core->setLockResult(LockRes::LOCKRES_OK, it);
  }
#ifdef TENDIS_DEBUG
  // one resource can only been locked one time for one thread
  INVARIANT_D(_threadIds.find(core->getThreadId()) == _threadIds.end());
  _threadIds.insert(core->getThreadId());
#endif
}

void LockSchedCtx::schedPendingLocks() {
  std::list<MGLock*>::iterator it = _pendingList.begin();
  while (it != _pendingList.end()) {
    MGLock* tmpLock = *it;
    if (isConflict(_runningModes, tmpLock->getMode())) {
      it++;
      // NOTE(vinchen): Here, it should be break instead of continue.
      // Because of first come first lock/unlock, it can't release the
      // lock after the conflict pending lock. Otherwise, it would lead
      // to this lock starve.
      break;
    }
    incrRunningRef(tmpLock->getMode());
    decPendingRef(tmpLock->getMode());
    auto runningIt = _runningList.insert(_runningList.end(), tmpLock);
    it = _pendingList.erase(it);
    tmpLock->setLockResult(LockRes::LOCKRES_OK, runningIt);
    tmpLock->notify();
  }
}

bool LockSchedCtx::unlock(MGLock* core) {
  auto mode = core->getMode();
  if (core->getStatus() == LockRes::LOCKRES_OK) {
    _runningList.erase(core->getLockIter());
    decRunningRef(mode);
    core->releaseLockResult();
    if (_runningModes != 0) {
#ifdef TENDIS_DEBUG
      _threadIds.erase(core->getThreadId());
#endif
      return false;
    }
    INVARIANT_D(_runningList.size() == 0);
    schedPendingLocks();
  } else if (core->getStatus() == LockRes::LOCKRES_WAIT) {
    _pendingList.erase(core->getLockIter());
    decPendingRef(mode);
    core->releaseLockResult();
    // no need to schedPendingLocks here
    INVARIANT_D((_pendingModes == 0 && _pendingList.size() == 0) ||
                (_pendingModes != 0 && _pendingList.size() != 0));
  } else {
    INVARIANT_D(0);
  }
#ifdef TENDIS_DEBUG
  _threadIds.erase(core->getThreadId());
#endif
  return _pendingList.empty() && _runningList.empty();
}

void LockSchedCtx::incrPendingRef(LockMode mode) {
  auto modeInt = enum2Int(mode);
  ++_pendingRefCnt[modeInt];
  if (_pendingRefCnt[modeInt] == 1) {
    INVARIANT_D((_pendingModes & (1 << modeInt)) == 0);
    _pendingModes |= static_cast<uint16_t>((1 << modeInt));
  }
}

void LockSchedCtx::decPendingRef(LockMode mode) {
  auto modeInt = enum2Int(mode);
  INVARIANT_D(_pendingRefCnt[modeInt] != 0);
  --_pendingRefCnt[modeInt];
  if (_pendingRefCnt[modeInt] == 0) {
    INVARIANT_D((_pendingModes & (1 << modeInt)) != 0);
    _pendingModes &= static_cast<uint16_t>(~(1 << modeInt));
  }
}

void LockSchedCtx::incrRunningRef(LockMode mode) {
  auto modeInt = enum2Int(mode);
  ++_runningRefCnt[modeInt];
  if (_runningRefCnt[modeInt] == 1) {
    INVARIANT_D((_runningModes & (1 << modeInt)) == 0);
    _runningModes |= static_cast<uint16_t>((1 << modeInt));
  }
}

void LockSchedCtx::decRunningRef(LockMode mode) {
  auto modeInt = enum2Int(mode);
  INVARIANT_D(_runningRefCnt[modeInt] != 0);
  --_runningRefCnt[modeInt];
  if (_runningRefCnt[modeInt] == 0) {
    INVARIANT_D((_runningModes & (1 << modeInt)) != 0);
    _runningModes &= static_cast<uint16_t>(~(1 << modeInt));
  }
}

std::string LockSchedCtx::toString() {
  std::stringstream ss;
  for (auto i : _runningList) {
    ss << "running: {" << i->toString() << "}\r\n";
  }

  for (auto i : _pendingList) {
    ss << "pending: {" << i->toString() << "}\r\n";
  }

  return ss.str();
}

MGLockMgr& MGLockMgr::getInstance() {
  static MGLockMgr mgr;
  return mgr;
}

void MGLockMgr::lock(MGLock* core) {
  uint64_t hash = core->getHash();
  LockShard& shard = _shards[hash % SHARD_NUM];
  std::lock_guard<std::mutex> lk(shard.mutex);
  auto iter = shard.map.find(core->getTarget());
  if (iter == shard.map.end()) {
    LockSchedCtx tmp;
    auto insertResult = shard.map.emplace(core->getTarget(), std::move(tmp));
    iter = insertResult.first;
  }
  iter->second.lock(core);
  return;
}

void MGLockMgr::unlock(MGLock* core) {
  uint64_t hash = core->getHash();
  LockShard& shard = _shards[hash % SHARD_NUM];
  std::lock_guard<std::mutex> lk(shard.mutex);

  INVARIANT_D(core->getStatus() == LockRes::LOCKRES_WAIT ||
              core->getStatus() == LockRes::LOCKRES_OK);

  auto iter = shard.map.find(core->getTarget());
  INVARIANT(iter != shard.map.end());
  bool empty = iter->second.unlock(core);
  if (empty) {
    shard.map.erase(iter);
  }
  return;
}

std::string MGLockMgr::toString() {
  std::stringstream ss;
  for (uint32_t i = 0; i < SHARD_NUM; i++) {
    LockShard& shard = _shards[i];

    std::lock_guard<std::mutex> lk(shard.mutex);
    for (auto& iter : shard.map) {
      ss << iter.second.toString();
    }
  }
  return ss.str();
}

}  // namespace mgl
}  // namespace tendisplus
