/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2023-2024, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2024, stasoid <stasoid@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <AK/Utf16View.h>
#include <LibCore/Process.h>
#include <windows.h>

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

Process Process::current()
{
    return GetCurrentProcess();
}

ErrorOr<Process> Process::spawn(ProcessSpawnOptions const& options)
{
    // file actions are not supported
    VERIFY(options.file_actions.is_empty());

    StringBuilder builder;
    if (!options.search_for_executable_in_path && !options.executable.find_any_of("\\/:"sv).has_value())
        builder.appendff("\"./{}\" ", options.executable);
    else
        builder.appendff("\"{}\" ", options.executable);

    for (auto arg : options.arguments)
        builder.appendff("\"{}\" ", arg);

    builder.append('\0');
    ByteBuffer command_line = TRY(builder.to_byte_buffer());

    auto curdir = options.working_directory.has_value() ? options.working_directory->characters() : 0;

    STARTUPINFO startup_info = {};
    PROCESS_INFORMATION process_info = {};

    BOOL result = CreateProcess(
        NULL,
        (char*)command_line.data(),
        NULL, // process security attributes
        NULL, // primary thread security attributes
        TRUE, // handles are inherited
        0,    // creation flags
        NULL, // use parent's environment
        curdir,
        &startup_info,
        &process_info);

    if (!result)
        return Error::from_windows_error();

    CloseHandle(process_info.hThread);

    return Process(process_info.hProcess);
}

ErrorOr<Process> Process::spawn(StringView path, ReadonlySpan<ByteString> arguments, ByteString working_directory, KeepAsChild)
{
    return spawn({
        .executable = path,
        .arguments = Vector<ByteString> { arguments },
        .working_directory = working_directory.is_empty() ? Optional<ByteString> {} : working_directory,
    });
}

ErrorOr<Process> Process::spawn(StringView path, ReadonlySpan<StringView> arguments, ByteString working_directory, KeepAsChild)
{
    Vector<ByteString> backing_strings;
    backing_strings.ensure_capacity(arguments.size());
    for (auto argument : arguments)
        backing_strings.append(argument);

    return spawn({
        .executable = path,
        .arguments = backing_strings,
        .working_directory = working_directory.is_empty() ? Optional<ByteString> {} : working_directory,
    });
}

// Get the full path of the executable file of the current process
ErrorOr<String> Process::get_name()
{
    wchar_t path[MAX_PATH] = {};

    DWORD length = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (!length)
        return Error::from_windows_error();

    return String::from_utf16(Utf16View { { (u16*)path, length } });
}

ErrorOr<void> Process::set_name(StringView, SetThreadName)
{
    // Process::set_name() probably cannot be implemented on Windows.
    return {};
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

ErrorOr<int> Process::wait_for_termination()
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
