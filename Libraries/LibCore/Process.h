/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Forward.h>
#include <AK/Span.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibCore/Export.h>
#include <LibCore/File.h>

namespace Core {

namespace FileAction {

struct OpenFile {
    ByteString path;
    File::OpenMode mode = File::OpenMode::NotOpen;
    int fd = -1;
    mode_t permissions = 0600;
};

struct CloseFile {
    int fd { -1 };
};

struct DupFd {
    int write_fd { -1 };
    int fd { -1 };
};

}

struct ProcessSpawnOptions {
    StringView name {};
    ByteString executable {};
    bool search_for_executable_in_path { false };
    // On supported platforms, ask the kernel to terminate this process when its parent dies.
    bool die_with_parent { false };
    // Start this process in a process group of its own, so that the signals a terminal sends to its foreground job,
    // such as SIGINT for Ctrl+C, do not reach it. SIGTTOU starts out blocked, so that writing to a terminal with tostop
    // set does not stop it. On Windows, the new process group only keeps Ctrl+C from it.
    bool new_process_group { false };
    ReadonlySpan<ByteString> arguments {};
    // The child's whole environment, as NAME=value strings. Without it, the child inherits the environment of this
    // process.
    Optional<Vector<ByteString>> environment {};

    using FileActionType = Variant<FileAction::OpenFile, FileAction::CloseFile, FileAction::DupFd>;
    Vector<FileActionType> file_actions {};
};

class CORE_API Process {
    AK_MAKE_NONCOPYABLE(Process);

public:
    Process(Process&& other);
    Process& operator=(Process&& other);
    ~Process();

    static ErrorOr<Process> spawn(ProcessSpawnOptions const& options);

    static ErrorOr<Process> spawn(StringView path, ReadonlySpan<ByteString> arguments);
    static ErrorOr<Process> spawn(StringView path, ReadonlySpan<StringView> arguments);

    static ErrorOr<String> get_name();

    [[noreturn]] static void terminate_immediately(int status);

    enum class TerminationMode {
        Graceful,
        Forceful,
    };
    static ErrorOr<void> terminate_process(pid_t, TerminationMode);

    static void wait_for_debugger_and_break();
    static ErrorOr<bool> is_being_debugged();

    pid_t pid() const;

    ErrorOr<int> wait_for_termination() const;

private:
#ifndef AK_OS_WINDOWS
    Process(pid_t pid = -1)
        : m_pid(pid)
    {
    }

    pid_t m_pid;
#else
    Process(void* handle = 0)
        : m_handle(handle)
    {
    }

    void* m_handle;
#endif
};

}
