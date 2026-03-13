/*
 * Copyright (c) 2025, Ryszard Goc <ryszardgoc@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/Concepts.h>
#include <AK/Error.h>
#include <AK/Format.h>
#include <AK/Windows.h>
#include <LibSync/Export.h>
#include <LibSync/Mutex.h>

namespace Sync {

template<>
Mutex::MutexBase()
{
    static_assert(sizeof(m_storage) == sizeof(SRWLOCK));
    PSRWLOCK pSRW = new (m_storage) SRWLOCK;
    InitializeSRWLock(pSRW);
}
template<>
Mutex::~MutexBase() = default;

template<>
void Mutex::lock()
{
    AcquireSRWLockExclusive(reinterpret_cast<PSRWLOCK>(m_storage));
}

template<>
bool Mutex::try_lock()
{
    return TryAcquireSRWLockExclusive(reinterpret_cast<PSRWLOCK>(m_storage));
}

template<>
void Mutex::unlock()
{
    ReleaseSRWLockExclusive(reinterpret_cast<PSRWLOCK>(m_storage));
}

template<>
RecursiveMutex::MutexBase()
{
    static_assert(sizeof(m_storage) == sizeof(CRITICAL_SECTION));
    LPCRITICAL_SECTION pCS = new (m_storage) CRITICAL_SECTION;
    // TODO: Optimize this for our use case
    InitializeCriticalSectionAndSpinCount(pCS, 4000);
}

template<>
RecursiveMutex::~MutexBase()
{
    DeleteCriticalSection(reinterpret_cast<LPCRITICAL_SECTION>(m_storage));
}

template<>
void RecursiveMutex::lock()
{
    EnterCriticalSection(reinterpret_cast<LPCRITICAL_SECTION>(m_storage));
}

template<>
bool RecursiveMutex::try_lock()
{
    return TryEnterCriticalSection(reinterpret_cast<LPCRITICAL_SECTION>(m_storage));
}

template<>
void RecursiveMutex::unlock()
{
    LeaveCriticalSection(reinterpret_cast<LPCRITICAL_SECTION>(m_storage));
}

namespace {

void init_ipc_mutex(void* storage)
{
    PHANDLE pHandle = new (storage) HANDLE;
    SECURITY_ATTRIBUTES sa = { .nLength = sizeof(SECURITY_ATTRIBUTES), .lpSecurityDescriptor = nullptr, .bInheritHandle = TRUE };
    // TODO: If we want to send these over IPC then some other methods have to be exposed to construct a MutexBase from
    // the duplicated HANDLE. Otherwise it can use handle inheritance. We might want to only make it inheritable as an option.
    HANDLE handle = CreateMutexW(&sa, FALSE, nullptr);
    if (!handle) {
        warnln("Failed to create mutex object with: {}", Error::from_windows_error());
        VERIFY_NOT_REACHED();
    }
    *pHandle = handle;
}

void destroy_ipc_mutex(void* storage)
{
    CloseHandle(*reinterpret_cast<PHANDLE>(storage));
}

void lock_ipc_mutex(void* storage)
{
    DWORD result = WaitForSingleObject(*reinterpret_cast<PHANDLE>(storage), INFINITE);
    if (result != WAIT_OBJECT_0) {
        warnln("Failed to acquire mutex: {}", Error::from_windows_error(result));
        VERIFY_NOT_REACHED();
    }
}

bool try_lock_ipc_mutex(void* storage)
{
    DWORD result = WaitForSingleObject(*reinterpret_cast<PHANDLE>(storage), 0);
    if (result == WAIT_OBJECT_0) {
        return true;
    }
    if (result == WAIT_TIMEOUT) {
        return false;
    }
    if (result == WAIT_ABANDONED)
        VERIFY_NOT_REACHED();
    warnln("Failed trying to acquire mutex: {}", Error::from_windows_error(result));
    VERIFY_NOT_REACHED();
}

void unlock_ipc_mutex(void* storage)
{
    BOOL result = ReleaseMutex(*reinterpret_cast<PHANDLE>(storage));
    if (!result) {
        warnln("Failed to release mutex: {}", Error::from_windows_error());
        VERIFY_NOT_REACHED();
    }
}

}

template<>
IPCMutex::MutexBase()
{
    static_assert(sizeof(m_storage) == sizeof(HANDLE));
    init_ipc_mutex(m_storage);
}
template<>
IPCMutex::~MutexBase() { destroy_ipc_mutex(m_storage); }
template<>
void IPCMutex::lock() { lock_ipc_mutex(m_storage); }
template<>
bool IPCMutex::try_lock() { return try_lock_ipc_mutex(m_storage); }
template<>
void IPCMutex::unlock() { unlock_ipc_mutex(m_storage); }

template<>
IPCRecursiveMutex::MutexBase()
{
    static_assert(sizeof(m_storage) == sizeof(HANDLE));
    init_ipc_mutex(m_storage);
}
template<>
IPCRecursiveMutex::~MutexBase() { destroy_ipc_mutex(m_storage); }
template<>
void IPCRecursiveMutex::lock() { lock_ipc_mutex(m_storage); }
template<>
bool IPCRecursiveMutex::try_lock() { return try_lock_ipc_mutex(m_storage); }
template<>
void IPCRecursiveMutex::unlock() { unlock_ipc_mutex(m_storage); }

template class SYNC_API MutexBase<PolicyNonRecursive, PolicyIntraprocess>;
template class SYNC_API MutexBase<PolicyRecursive, PolicyIntraprocess>;
template class SYNC_API MutexBase<PolicyNonRecursive, PolicyInterprocess>;
template class SYNC_API MutexBase<PolicyRecursive, PolicyInterprocess>;

}
