#pragma once

#include <psp2/kernel/threadmgr.h>

class Mutex {
public:
    Mutex() { sceKernelCreateLwMutex(&work, "vpkgj_lock", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, nullptr); }
    ~Mutex() { sceKernelDeleteLwMutex(&work); }
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;
    void lock() { sceKernelLockLwMutex(&work, 1, nullptr); }
    void unlock() { sceKernelUnlockLwMutex(&work, 1); }
    bool try_lock() { return sceKernelTryLockLwMutex(&work, 1) == 0; }

private:
    alignas(8) SceKernelLwMutexWork work;
};
