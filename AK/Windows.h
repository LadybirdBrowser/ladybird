/*
 * Copyright (c) 2024, stasoid <stasoid@yahoo.com>
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 * Copyright (c) 2025, Ryszard Goc <ryszardgoc@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// This header should be included only in cpp files.
// It should be included after all other files and should be separated from them by a non-#include line to prevent clang-format from changing header order.

#pragma once

#include <AK/Assertions.h>
#include <AK/Diagnostics.h>
#include <AK/Platform.h>

#ifdef AK_OS_WINDOWS // needed for Swift
#    define timeval dummy_timeval
#    include <ntstatus.h>
#    include <winsock2.h>
#    include <winternl.h>

extern "C" {
// NOTE: These are documented here: https://learn.microsoft.com/en-us/windows/win32/devnotes/-win32-misclowlevelclientsupport
// If the function signature changes, we should catch it with GetProcAddress failing in most cases.
// None of these are marked deprecated and they seem to be used internally in the kernel.

NTAPI NTSTATUS NtAssociateWaitCompletionPacket(
    _In_ HANDLE WaitCompletionPacketHandle,
    _In_ HANDLE IoCompletionHandle,
    _In_ HANDLE TargetObjectHandle,
    _In_opt_ PVOID KeyContext,
    _In_opt_ PVOID ApcContext,
    _In_ NTSTATUS IoStatus,
    _In_ ULONG_PTR IoStatusInformation,
    _Out_opt_ PBOOLEAN AlreadySignaled);

NTAPI NTSTATUS NtCancelWaitCompletionPacket(
    _In_ HANDLE WaitCompletionPacketHandle,
    _In_ BOOLEAN RemoveSignaledPacket);

NTAPI NTSTATUS NtCreateWaitCompletionPacket(
    _Out_ PHANDLE WaitCompletionPacketHandle,
    _In_ ACCESS_MASK DesiredAccess,
    _In_opt_ POBJECT_ATTRIBUTES ObjectAttributes);

// https://learn.microsoft.com/en-us/windows/win32/seccng/processprng
BOOL WINAPI ProcessPrng(PBYTE pbData, SIZE_T cbData);
}

using PFN_NtCreateWaitCompletionPacket = decltype(&NtCreateWaitCompletionPacket);
using PFN_NtCancelWaitCompletionPacket = decltype(&NtCancelWaitCompletionPacket);
using PFN_NtAssociateWaitCompletionPacket = decltype(&NtAssociateWaitCompletionPacket);
using PFN_ProcessPrng = decltype(&ProcessPrng);

inline struct SystemApi {
    PFN_NtAssociateWaitCompletionPacket NtAssociateWaitCompletionPacket = NULL;
    PFN_NtCancelWaitCompletionPacket NtCancelWaitCompletionPacket = NULL;
    PFN_NtCreateWaitCompletionPacket NtCreateWaitCompletionPacket = NULL;
    PFN_ProcessPrng ProcessPrng = NULL;

    SystemApi()
    {
        HMODULE hBcryptprimitives = LoadLibraryW(L"bcryptprimitives.dll");
        HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
        VERIFY(hBcryptprimitives);
        VERIFY(hNtdll);
        AK_IGNORE_DIAGNOSTIC("-Wcast-function-type-mismatch",
            NtAssociateWaitCompletionPacket = (PFN_NtAssociateWaitCompletionPacket)GetProcAddress(hNtdll, "NtAssociateWaitCompletionPacket");
            NtCancelWaitCompletionPacket = (PFN_NtCancelWaitCompletionPacket)GetProcAddress(hNtdll, "NtCancelWaitCompletionPacket");
            NtCreateWaitCompletionPacket = (PFN_NtCreateWaitCompletionPacket)GetProcAddress(hNtdll, "NtCreateWaitCompletionPacket");
            ProcessPrng = (PFN_ProcessPrng)GetProcAddress(hBcryptprimitives, "ProcessPrng");)
        VERIFY(NtAssociateWaitCompletionPacket);
        VERIFY(NtCancelWaitCompletionPacket);
        VERIFY(NtCreateWaitCompletionPacket);
        VERIFY(ProcessPrng);
    }
} g_system;

#    undef timeval
#    undef IN
#    pragma comment(lib, "ws2_32.lib")
#    include <io.h>
#    include <stdlib.h>

inline void initiate_wsa()
{
    WSADATA wsa;
    WORD version = MAKEWORD(2, 2);
    int rc = WSAStartup(version, &wsa);
    VERIFY(rc == 0 && wsa.wVersion == version);
}

inline void terminate_wsa()
{
    int rc = WSACleanup();
    VERIFY(rc == 0);
}

static void invalid_parameter_handler(wchar_t const*, wchar_t const*, wchar_t const*, unsigned int, uintptr_t)
{
}

inline void override_crt_invalid_parameter_handler()
{
    // Make _get_osfhandle return -1 instead of crashing on invalid fd in release (debug still __debugbreak's)
    _set_invalid_parameter_handler(invalid_parameter_handler);
}

inline void windows_init()
{
    initiate_wsa();
    override_crt_invalid_parameter_handler();
}

inline void windows_shutdown()
{
    terminate_wsa();
}
#endif
