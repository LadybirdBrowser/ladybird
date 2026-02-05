/*
 * Copyright (c) 2020, Sergey Bugaev <bugaevc@serenityos.org>
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/StdLibExtras.h>
#include <LibCore/Forward.h>

namespace IPC {

class File {
    AK_MAKE_NONCOPYABLE(File);

public:
    File() = default;

    static File adopt_file(NonnullOwnPtr<Core::File> file);
    static File adopt_fd(int fd);
    static ErrorOr<File> clone_fd(int fd);

    File(File&& other);
    File& operator=(File&& other);

    ~File();

    int fd() const { return m_fd; }

    // This is 'const' since generated IPC messages expose all parameters by const reference.
    [[nodiscard]] int take_fd() const { return exchange(m_fd, -1); }

    ErrorOr<void> clear_close_on_exec();

private:
    explicit File(int fd);

    mutable int m_fd { -1 };
};

}
