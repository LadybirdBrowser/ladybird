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

// FIXME: Implement other file actions

}

struct ProcessSpawnOptions {
    StringView name {};
    ByteString executable {};
    bool search_for_executable_in_path { false };
    Vector<ByteString> const& arguments {};
    Optional<ByteString> working_directory {};

    using FileActionType = Variant<FileAction::OpenFile, FileAction::CloseFile>;
    Vector<FileActionType> file_actions {};
};

class Process {
    AK_MAKE_NONCOPYABLE(Process);

public:
    enum class KeepAsChild {
        Yes,
        No
    };

    Process(Process&& other)
        : m_pid(exchange(other.m_pid, 0))
        , m_should_disown(exchange(other.m_should_disown, false))
    {
    }

    Process& operator=(Process&& other)
    {
        m_pid = exchange(other.m_pid, 0);
        m_should_disown = exchange(other.m_should_disown, false);
        return *this;
    }

    ~Process()
    {
        (void)disown();
    }

    static ErrorOr<Process> spawn(ProcessSpawnOptions const& options);
    static Process current();

    // FIXME: Make the following 2 functions return Process instance or delete them.
    static ErrorOr<pid_t> spawn(StringView path, ReadonlySpan<ByteString> arguments, ByteString working_directory = {}, KeepAsChild keep_as_child = KeepAsChild::No);
    static ErrorOr<pid_t> spawn(StringView path, ReadonlySpan<StringView> arguments, ByteString working_directory = {}, KeepAsChild keep_as_child = KeepAsChild::No);

    // FIXME: Remove this. char const* should not exist on this level of abstraction.
    static ErrorOr<pid_t> spawn(StringView path, ReadonlySpan<char const*> arguments = {}, ByteString working_directory = {}, KeepAsChild keep_as_child = KeepAsChild::No);

    static ErrorOr<String> get_name();
    enum class SetThreadName {
        No,
        Yes,
    };
    static ErrorOr<void> set_name(StringView, SetThreadName = SetThreadName::No);

    static void wait_for_debugger_and_break();
    static ErrorOr<bool> is_being_debugged();

    pid_t pid() const { return m_pid; }

    ErrorOr<void> disown();

    // FIXME: Make it return an exit code.
    ErrorOr<bool> wait_for_termination();

private:
    Process(pid_t pid)
        : m_pid(pid)
        , m_should_disown(true)
    {
    }

    pid_t m_pid;
    bool m_should_disown;
};

}
