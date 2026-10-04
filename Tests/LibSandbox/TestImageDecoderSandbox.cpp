/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Function.h>
#include <ImageDecoder/Sandbox.h>
#include <LibTest/TestCase.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

// These tests apply the real sandbox of the image decoder in a child process. The decoder parses untrusted images,
// and only ever talks over the channel the Browser handed it, so it must not be able to make sockets or reach files.

static int run_in_image_decoder_sandbox(Function<void()> const& body)
{
    auto child = fork();
    VERIFY(child >= 0);
    if (child == 0) {
        if (ImageDecoder::apply_sandbox({}).is_error())
            _exit(2);
        body();
        _exit(0);
    }

    int status = 0;
    VERIFY(waitpid(child, &status, 0) == child);
    return status;
}

TEST_CASE(image_decoder_cannot_pair_sockets)
{
    auto status = run_in_image_decoder_sandbox([] {
        int fds[2];
        (void)socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    });

    EXPECT(WIFEXITED(status));
    if (WIFEXITED(status))
        EXPECT_EQ(WEXITSTATUS(status), 128 + SIGSYS);
}

TEST_CASE(image_decoder_cannot_create_sockets)
{
    auto status = run_in_image_decoder_sandbox([] {
        VERIFY(socket(AF_UNIX, SOCK_STREAM, 0) == -1);
        VERIFY(errno == EAFNOSUPPORT);
    });

    EXPECT(WIFEXITED(status));
    if (WIFEXITED(status))
        EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST_CASE(image_decoder_cannot_open_files)
{
    auto status = run_in_image_decoder_sandbox([] {
        VERIFY(open("/etc/passwd", O_RDONLY | O_CLOEXEC) == -1);
        VERIFY(errno == EACCES);
    });

    EXPECT(WIFEXITED(status));
    if (WIFEXITED(status))
        EXPECT_EQ(WEXITSTATUS(status), 0);
}
