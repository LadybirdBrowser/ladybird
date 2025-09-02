/*
 * Copyright (c) 2024, stasoid <stasoid@yahoo.com>
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
}

using PFN_NtCreateWaitCompletionPacket = decltype(&NtCreateWaitCompletionPacket);
using PFN_NtCancelWaitCompletionPacket = decltype(&NtCancelWaitCompletionPacket);
using PFN_NtAssociateWaitCompletionPacket = decltype(&NtAssociateWaitCompletionPacket);

inline struct SystemApi {
    PFN_NtAssociateWaitCompletionPacket NtAssociateWaitCompletionPacket = NULL;
    PFN_NtCancelWaitCompletionPacket NtCancelWaitCompletionPacket = NULL;
    PFN_NtCreateWaitCompletionPacket NtCreateWaitCompletionPacket = NULL;

    SystemApi()
    {
        HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
        VERIFY(hNtdll);
        AK_IGNORE_DIAGNOSTIC("-Wcast-function-type-mismatch",
            NtAssociateWaitCompletionPacket = (PFN_NtAssociateWaitCompletionPacket)GetProcAddress(hNtdll, "NtAssociateWaitCompletionPacket");
            NtCancelWaitCompletionPacket = (PFN_NtCancelWaitCompletionPacket)GetProcAddress(hNtdll, "NtCancelWaitCompletionPacket");
            NtCreateWaitCompletionPacket = (PFN_NtCreateWaitCompletionPacket)GetProcAddress(hNtdll, "NtCreateWaitCompletionPacket");)
        VERIFY(NtAssociateWaitCompletionPacket);
        VERIFY(NtCancelWaitCompletionPacket);
        VERIFY(NtCreateWaitCompletionPacket);
    }
} g_system;

#    undef timeval
#    undef IN
#    pragma comment(lib, "ws2_32.lib")
#    include <io.h>
#endif
