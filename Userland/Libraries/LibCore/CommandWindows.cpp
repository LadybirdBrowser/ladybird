/*
 * Copyright (c) 2020, Itamar S. <itamar8910@gmail.com>
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, stasoid <stasoid@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibCore/Command.h>
#include <io.h>
#include <windows.h>

namespace Core {

struct Handle {
    HANDLE handle { NULL };
    ~Handle() { handle ? CloseHandle(handle) : 0; }
};

static Error windows_error()
{
    return Error::from_windows_error(GetLastError());
}

ErrorOr<OwnPtr<Command>> Command::create(StringView command, char const* const raw_arguments[])
{
    SECURITY_ATTRIBUTES sa {};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    Handle stdin_read, stdin_write;
    if (!CreatePipe(&stdin_read.handle, &stdin_write.handle, &sa, 0))
        return windows_error();
    if (!SetHandleInformation(stdin_write.handle, HANDLE_FLAG_INHERIT, 0))
        return windows_error();

    Handle stdout_read, stdout_write;
    if (!CreatePipe(&stdout_read.handle, &stdout_write.handle, &sa, 0))
        return windows_error();
    if (!SetHandleInformation(stdout_read.handle, HANDLE_FLAG_INHERIT, 0))
        return windows_error();

    Handle stderr_read, stderr_write;
    if (!CreatePipe(&stderr_read.handle, &stderr_write.handle, &sa, 0))
        return windows_error();
    if (!SetHandleInformation(stderr_read.handle, HANDLE_FLAG_INHERIT, 0))
        return windows_error();

    size_t argument_count = 0;
    while (raw_arguments[argument_count])
        argument_count++;
    Vector<char const*> arguments { { raw_arguments, argument_count } };
    StringBuilder builder;
    builder.append(command);
    builder.append(' ');
    builder.join(' ', arguments);
    builder.append('\0');
    ByteBuffer command_line = TRY(builder.to_byte_buffer());

    STARTUPINFO startup_info {};
    startup_info.cb = sizeof(STARTUPINFO);
    startup_info.dwFlags = STARTF_USESTDHANDLES;
    startup_info.hStdInput = stdin_read.handle;
    startup_info.hStdOutput = stdout_write.handle;
    startup_info.hStdError = stderr_write.handle;

    PROCESS_INFORMATION process_info {};

    BOOL result = CreateProcess(NULL,
        reinterpret_cast<char*>(command_line.data()),
        NULL, // process security attributes
        NULL, // primary thread security attributes
        TRUE, // handles are inherited
        0,    // creation flags
        NULL, // use parent's environment
        NULL, // use parent's current directory
        &startup_info,
        &process_info);

    if (!result)
        return windows_error();

    CloseHandle(process_info.hThread);

    auto stdin_file = TRY(Core::File::adopt_fd(_open_osfhandle((intptr_t)stdin_write.handle, 0), Core::File::OpenMode::Write));
    stdin_write.handle = NULL;
    auto stdout_file = TRY(Core::File::adopt_fd(_open_osfhandle((intptr_t)stdout_read.handle, 0), Core::File::OpenMode::Read));
    stdout_read.handle = NULL;
    auto stderr_file = TRY(Core::File::adopt_fd(_open_osfhandle((intptr_t)stderr_read.handle, 0), Core::File::OpenMode::Read));
    stderr_read.handle = NULL;

    return make<Command>(
        process_info.dwProcessId,
        process_info.hProcess,
        move(stdin_file),
        move(stdout_file),
        move(stderr_file));
}

Command::Command(pid_t pid, void* process_handle, NonnullOwnPtr<Core::File> stdin_file, NonnullOwnPtr<Core::File> stdout_file, NonnullOwnPtr<Core::File> stderr_file)
    : m_pid(pid)
    , m_process_handle(process_handle)
    , m_stdin(move(stdin_file))
    , m_stdout(move(stdout_file))
    , m_stderr(move(stderr_file))
{
}

ErrorOr<void> Command::write(StringView input)
{
    ScopeGuard close_stdin { [&] { m_stdin->close(); } };
    TRY(m_stdin->write_until_depleted(input.bytes()));
    return {};
}

ErrorOr<void> Command::write_lines(Span<ByteString> lines)
{
    ScopeGuard close_stdin { [&] { m_stdin->close(); } };
    for (auto line : lines)
        TRY(m_stdin->write_until_depleted(ByteString::formatted("{}\n", line)));
    return {};
}

ErrorOr<Command::ProcessOutputs> Command::read_all()
{
    return ProcessOutputs { TRY(m_stdout->read_until_eof()), TRY(m_stderr->read_until_eof()) };
}

ErrorOr<Command::ProcessResult> Command::status(int options)
{
    if (m_pid == -1)
        return ProcessResult::Unknown;

    m_stdin->close();

    auto result = WaitForSingleObject(m_process_handle, INFINITE);
    if (result == WAIT_FAILED)
        return windows_error();

    ScopeGuard close_process_handle {
        [&] {
            CloseHandle(m_process_handle);
            m_process_handle = nullptr;
            m_pid = -1;
        }
    };

    DWORD exit_code = 0;
    if (!GetExitCodeProcess(m_process_handle, &exit_code))
        return windows_error();

    if (exit_code == 0)
        return ProcessResult::DoneWithZeroExitCode;

    return ProcessResult::Failed;
}

}
