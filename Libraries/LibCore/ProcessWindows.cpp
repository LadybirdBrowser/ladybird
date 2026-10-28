/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2023-2024, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2024, stasoid <stasoid@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Assertions.h>
#include <AK/ByteBuffer.h>
#include <AK/ScopeGuard.h>
#include <AK/String.h>
#include <AK/Utf16View.h>
#include <AK/Vector.h>
#include <AK/Windows.h>
#include <LibCore/Process.h>
#include <LibCore/System.h>

namespace Core {

Process::Process(Process&& other)
    : m_handle(exchange(other.m_handle, nullptr))
{
}

Process& Process::operator=(Process&& other)
{
    m_handle = exchange(other.m_handle, nullptr);
    return *this;
}

Process::~Process()
{
    if (m_handle)
        CloseHandle(m_handle);
}

static ErrorOr<HANDLE> duplicate_inheritable_handle(HANDLE handle)
{
    HANDLE duplicate = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &duplicate, 0, TRUE, DUPLICATE_SAME_ACCESS))
        return Error::from_windows_error();
    return duplicate;
}

ErrorOr<Process> Process::spawn(ProcessSpawnOptions const& options)
{
    // FIXME: Pass a custom environment to CreateProcess.
    VERIFY(!options.environment.has_value());

    // The child inherits only the handles listed for it, as posix_spawn's file actions give a child only the
    // descriptors they name, so a handle passed to one child never reaches another spawned meanwhile.
    Vector<HANDLE> inherited_handles;
    Vector<HANDLE> handles_to_make_uninheritable;
    Vector<HANDLE> duplicates_to_close;
    ScopeGuard clean_up_handles = [&] {
        for (auto* handle : handles_to_make_uninheritable)
            SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0);
        for (auto* handle : duplicates_to_close)
            CloseHandle(handle);
    };

    Array<HANDLE, 3> standard_handles { GetStdHandle(STD_INPUT_HANDLE), GetStdHandle(STD_OUTPUT_HANDLE), GetStdHandle(STD_ERROR_HANDLE) };
    // The standard descriptor numbers are this process's own standard handles, which all share one value where it has
    // none, so an action takes the first matching stream that no earlier action took.
    Array<int, 3> standard_fds { STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO };
    Array<bool, 3> standard_handle_is_redirected {};
    for (auto const& file_action : options.file_actions) {
        TRY(file_action.visit(
            [&](FileAction::DupFd const& action) -> ErrorOr<void> {
                auto handle = to_handle(action.write_fd);
                for (size_t index : { 1uz, 2uz, 0uz }) {
                    if (action.fd != standard_fds[index] || standard_handle_is_redirected[index])
                        continue;
                    standard_handles[index] = handle;
                    standard_handle_is_redirected[index] = true;
                    return {};
                }
                // A handle cannot be renumbered, but an inherited handle keeps its value in the child.
                if (action.fd != action.write_fd)
                    return Error::from_string_literal("A handle can only reach a child process under its own value");
                if (inherited_handles.contains_slow(handle))
                    return {};
                DWORD flags = 0;
                if (!GetHandleInformation(handle, &flags))
                    return Error::from_windows_error();
                if (!(flags & HANDLE_FLAG_INHERIT)) {
                    if (!SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
                        return Error::from_windows_error();
                    handles_to_make_uninheritable.append(handle);
                }
                TRY(inherited_handles.try_append(handle));
                return {};
            },
            [](auto const&) -> ErrorOr<void> {
                return Error::from_string_literal("This file action is not supported on Windows");
            }));
    }

    // The child finds its standard handles in its startup info, so each is passed as a duplicate that is inheritable
    // for this spawn alone.
    bool has_standard_handles = false;
    for (auto& handle : standard_handles) {
        if (!handle || handle == INVALID_HANDLE_VALUE)
            continue;
        handle = TRY(duplicate_inheritable_handle(handle));
        TRY(duplicates_to_close.try_append(handle));
        TRY(inherited_handles.try_append(handle));
        has_standard_handles = true;
    }

    StringBuilder builder;
    if (!options.search_for_executable_in_path && !options.executable.find_any_of("\\/:"sv).has_value())
        builder.appendff("\"./{}\" ", options.executable);
    else
        builder.appendff("\"{}\" ", options.executable);

    for (auto arg : options.arguments)
        builder.appendff("\"{}\" ", arg);

    builder.append('\0');
    ByteBuffer command_line = TRY(builder.to_byte_buffer());

    STARTUPINFOEXA startup_info = {};
    startup_info.StartupInfo.cb = sizeof(startup_info);
    if (has_standard_handles) {
        startup_info.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
        startup_info.StartupInfo.hStdInput = standard_handles[0];
        startup_info.StartupInfo.hStdOutput = standard_handles[1];
        startup_info.StartupInfo.hStdError = standard_handles[2];
    }

    ByteBuffer attribute_list_storage;
    if (!inherited_handles.is_empty()) {
        SIZE_T attribute_list_size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_list_size);
        attribute_list_storage = TRY(ByteBuffer::create_zeroed(attribute_list_size));
        startup_info.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_list_storage.data());
        if (!InitializeProcThreadAttributeList(startup_info.lpAttributeList, 1, 0, &attribute_list_size))
            return Error::from_windows_error();
    }
    ScopeGuard delete_attribute_list = [&] {
        if (startup_info.lpAttributeList)
            DeleteProcThreadAttributeList(startup_info.lpAttributeList);
    };
    if (startup_info.lpAttributeList) {
        if (!UpdateProcThreadAttribute(startup_info.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, static_cast<void*>(inherited_handles.data()), inherited_handles.size() * sizeof(HANDLE), nullptr, nullptr))
            return Error::from_windows_error();
    }

    PROCESS_INFORMATION process_info = {};

    BOOL result = CreateProcessA(
        NULL,
        (char*)command_line.data(),
        NULL, // process security attributes
        NULL, // primary thread security attributes
        !inherited_handles.is_empty(),
        EXTENDED_STARTUPINFO_PRESENT | (options.new_process_group ? CREATE_NEW_PROCESS_GROUP : 0),
        NULL, // use parent's environment
        NULL, // working directory
        &startup_info.StartupInfo,
        &process_info);

    if (!result)
        return Error::from_windows_error();

    CloseHandle(process_info.hThread);

    return Process(process_info.hProcess);
}

ErrorOr<Process> Process::spawn(StringView path, ReadonlySpan<ByteString> arguments)
{
    return spawn({
        .executable = path,
        .arguments = arguments,
    });
}

ErrorOr<Process> Process::spawn(StringView path, ReadonlySpan<StringView> arguments)
{
    Vector<ByteString> backing_strings;
    backing_strings.ensure_capacity(arguments.size());
    for (auto argument : arguments)
        backing_strings.append(argument);

    return spawn({
        .executable = path,
        .arguments = backing_strings,
    });
}

void Process::terminate_immediately(int status)
{
    // TerminateProcess() is stronger than the CRT's _exit(), since it skips
    // DLL detach notifications and the other ExitProcess() teardown paths.
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(status));
    VERIFY_NOT_REACHED();
}

ErrorOr<void> Process::terminate_process(pid_t pid, TerminationMode mode)
{
    if (mode == TerminationMode::Graceful) {
        // There is no SIGTERM equivalent on Windows; the closest honest approximation is
        // asking every top-level window of the process to close. Console or windowless
        // processes will not observe this.
        if (!EnumWindows([](HWND hwnd, LPARAM l_param) -> BOOL {
                DWORD window_pid = 0;
                GetWindowThreadProcessId(hwnd, &window_pid);
                if (window_pid == static_cast<DWORD>(l_param))
                    PostMessage(hwnd, WM_CLOSE, 0, 0);
                return TRUE;
            },
                pid))
            return Error::from_windows_error();
        return {};
    }

    HANDLE handle = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (!handle)
        return Error::from_windows_error();
    ScopeGuard close_handle = [&] { CloseHandle(handle); };
    if (!TerminateProcess(handle, 1))
        return Error::from_windows_error();
    return {};
}

// Get the full path of the executable file of the current process
ErrorOr<String> Process::get_name()
{
    Vector<wchar_t, MAX_PATH> path;
    path.resize(MAX_PATH);

    DWORD length = GetModuleFileNameW(NULL, path.data(), MAX_PATH);

    if (length == path.size() && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        path.resize(UNICODE_STRING_MAX_CHARS);
        length = GetModuleFileNameW(NULL, path.data(), UNICODE_STRING_MAX_CHARS);
    }

    if (!length)
        return Error::from_windows_error();

    return MUST(Utf16View { reinterpret_cast<char16_t const*>(path.data()), length }.to_utf8());
}

ErrorOr<bool> Process::is_being_debugged()
{
    return IsDebuggerPresent();
}

// Forces the process to sleep until a debugger is attached, then breaks.
void Process::wait_for_debugger_and_break()
{
    bool print_message = true;
    for (;;) {
        if (IsDebuggerPresent()) {
            DebugBreak();
            return;
        }
        if (print_message) {
            dbgln("Process {} with pid {} is sleeping, waiting for debugger.", Process::get_name(), GetCurrentProcessId());
            print_message = false;
        }
        Sleep(100);
    }
}

pid_t Process::pid() const
{
    return GetProcessId(m_handle);
}

ErrorOr<int> Process::wait_for_termination() const
{
    auto result = WaitForSingleObject(m_handle, INFINITE);
    if (result == WAIT_FAILED)
        return Error::from_windows_error();

    DWORD exit_code = 0;
    if (!GetExitCodeProcess(m_handle, &exit_code))
        return Error::from_windows_error();

    return exit_code;
}

}
