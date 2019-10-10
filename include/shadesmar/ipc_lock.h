//
// Created by squadrick on 30/09/19.
//

#ifndef shadesmar_IPC_LOCK_H
#define shadesmar_IPC_LOCK_H

#include <sys/stat.h>

#include <cstdint>
#include <thread>

#include <boost/interprocess/sync/interprocess_upgradable_mutex.hpp>

#include <shadesmar/macros.h>

using namespace boost::interprocess;

#define MAX_SH_PROCS 64

template <uint32_t size,
          std::memory_order mem_order = std::memory_order_relaxed>
class IPC_Set {
  static_assert((size & (size - 1)) == 0, "size must be power of two");

 public:
  IPC_Set() { std::memset(__array, 0, size); }

  void insert(uint32_t elem) {
    for (uint32_t idx = hash(elem);; ++idx) {
      idx &= size - 1;
      auto probedElem = __array[idx].load(mem_order);

      if (probedElem != elem) {
        // The entry is either free or contains another key
        if (probedElem != 0) {
          continue;  // contains another key
        }
        // Entry is free, time for CAS
        // probedKey or __array[idx] is expected to be zero
        uint32_t exp = 0;
        if (__array[idx].compare_exchange_strong(exp, elem, mem_order)) {
          // successfully insert the element
          break;
        } else {
          // some other proc got to it before us, continue searching
          // to know which elem was written to __array[idx], look at exp
          continue;
        }
      }
      return;
    }
  }

  bool remove(uint32_t elem) {
    uint32_t idx = hash(elem) & size - 1, init_idx = idx;
    do {
      idx &= size - 1;
      auto probedElem = __array[idx].load(mem_order);

      if (probedElem == elem) {
        if (__array[idx].compare_exchange_strong(elem, 0, mem_order)) {
          // successfully found and deleted elem
          return true;
        } else {
          // possible that some other proc deleted elem
          // TODO: Maybe do a break instead
          idx++;
          continue;
        }
      }
      idx++;
    } while (idx != init_idx);

    // we exit after doing a full pass through the array
    // but we failed to delete an element, maybe already deleted
    return false;
  }

  std::atomic_uint32_t __array[size]{};

 private:
  inline static uint32_t hash(uint32_t h) {
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
  }
};

bool proc_exists(__pid_t proc) {
  std::string pid_path = "/proc/" + std::to_string(proc);
  struct stat sts {};
  return !(stat(pid_path.c_str(), &sts) == -1 && errno == ENOENT);
}

class IPC_Lock {
 public:
  IPC_Lock() = default;
  void lock() {
    // TODO: use timed_lock instead of try_lock
    while (!mutex_.try_lock()) {
      // failed to get mutex_ within timeout,
      // so mutex_ is either held properly
      // or some process which holds it has died
      if (ex_proc != -1) {
        // ex_proc is not default value, some other proc
        // has access already
        if (proc_exists(ex_proc)) {
          // fear not, everything is alright, wait for
          // next timeout
        } else {
          // proc is fucked, we don't unlock
          // we just assume that this proc has the lock
          // relinquish control
          // and we return to the caller
          break;
        }
      } else {
        // ex_proc = -1, so the writers are blocking us
        prune_sharable_procs();
      }

      std::this_thread::sleep_for(std::chrono::microseconds(2000));
    }
    ex_proc = getpid();
  }

  void unlock() {
    ex_proc = -1;
    mutex_.unlock();
  }

  void lock_sharable() {
    // TODO: use timed_lock_sharable instead of try_lock_sharable
    while (!mutex_.try_lock_sharable()) {
      // only reason for failure is that exclusive lock is held
      if (ex_proc != -1) {
        if (proc_exists(ex_proc)) {
          // no problem, go back to waiting
        } else {
          // ex_proc is dead
          ex_proc = -1;
          mutex_.unlock();
        }
      } else {
        // should never happen
      }
      // TODO: Maybe prune_sharable_procs()?

      std::this_thread::sleep_for(std::chrono::microseconds(2000));
    }
    sh_procs.insert(getpid());
  }

  void unlock_sharable() {
    if (sh_procs.remove(getpid())) {
      mutex_.unlock_sharable();
    }
  }

 private:
  void prune_sharable_procs() {
    for (auto &i : sh_procs.__array) {
      uint32_t sh_proc = i.load();

      if (sh_proc == 0) continue;
      if (!proc_exists(sh_proc)) {
        if (sh_procs.remove(sh_proc)) {
          // removal of element was a success
          // this ensures no over-pruning or duplicate deletion
          // TODO: Verify no contention
          mutex_.unlock_sharable();
        }
      }
    }
  }

  interprocess_upgradable_mutex mutex_;  // 16 bytes
  std::atomic<__pid_t> ex_proc{0};       // 4 bytes
  IPC_Set<MAX_SH_PROCS> sh_procs;        // 320 bytes
  // Total size = 340 bytes
};

#endif  // shadesmar_IPC_LOCK_H