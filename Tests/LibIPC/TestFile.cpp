/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/System.h>
#include <LibIPC/File.h>
#include <LibTest/TestCase.h>
#include <fcntl.h>

static bool is_open(int fd)
{
    return fcntl(fd, F_GETFD) != -1;
}

TEST_CASE(move_assignment_closes_the_descriptor_it_replaces)
{
    auto replaced_fd = TRY_OR_FAIL(Core::System::open("/dev/null"sv, O_RDONLY | O_CLOEXEC));
    auto moved_fd = TRY_OR_FAIL(Core::System::open("/dev/null"sv, O_RDONLY | O_CLOEXEC));

    auto file = IPC::File::adopt_fd(replaced_fd);
    file = IPC::File::adopt_fd(moved_fd);
    EXPECT(!is_open(replaced_fd));
    EXPECT(is_open(moved_fd));
    EXPECT_EQ(file.fd(), moved_fd);
}
