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
#include <AK/String.h>
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
    Vector<ByteString> const& arguments {};
    Optional<ByteString> working_directory {};

    using FileActionType = Variant<FileAction::OpenFile, FileAction::CloseFile, FileAction::DupFd>;
    Vector<FileActionType> file_actions {};
};

class Process {
    AK_MAKE_NONCOPYABLE(Process);

public:
    enum class KeepAsChild {
        Yes,
        No
    };

    Process(Process&& other);
    Process& operator=(Process&& other);
    ~Process();

    static ErrorOr<Process> spawn(ProcessSpawnOptions const& options);
    static Process current();

    static ErrorOr<Process> spawn(StringView path, ReadonlySpan<ByteString> arguments, ByteString working_directory = {}, KeepAsChild keep_as_child = KeepAsChild::No);
    static ErrorOr<Process> spawn(StringView path, ReadonlySpan<StringView> arguments, ByteString working_directory = {}, KeepAsChild keep_as_child = KeepAsChild::No);

    static ErrorOr<String> get_name();
    enum class SetThreadName {
        No,
        Yes,
    };
    static ErrorOr<void> set_name(StringView, SetThreadName = SetThreadName::No);

    static void wait_for_debugger_and_break();
    static ErrorOr<bool> is_being_debugged();

    pid_t pid() const;

#ifndef AK_OS_WINDOWS
    ErrorOr<void> disown();
#endif

    ErrorOr<int> wait_for_termination();

private:
#ifndef AK_OS_WINDOWS
    Process(pid_t pid = -1)
        : m_pid(pid)
        , m_should_disown(true)
    {
    }

    pid_t m_pid;
    bool m_should_disown;
#else
    Process(void* handle = 0)
        : m_handle(handle)
    {
    }

    void* m_handle;
#endif
};

}
