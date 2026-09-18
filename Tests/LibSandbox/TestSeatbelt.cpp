/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>

#if defined(AK_OS_MACOS)

#    include <AK/ByteString.h>
#    include <AK/Function.h>
#    include <AK/Vector.h>
#    include <LibCore/System.h>
#    include <LibSandbox/Sandbox.h>
#    include <LibTest/TestCase.h>
#    include <errno.h>
#    include <fcntl.h>
#    include <limits.h>
#    include <pthread.h>
#    include <signal.h>
#    include <stdlib.h>
#    include <sys/mman.h>
#    include <sys/socket.h>
#    include <sys/stat.h>
#    include <sys/wait.h>
#    include <unistd.h>

enum class Outcome {
    Allowed,
    Denied,
};

// Runs the operation in a child process after applying the profile. The operation returns whether it succeeded. A
// child killed with SIGKILL by the syscall filter counts as denied.
static Outcome run_sandboxed(Sandbox::SeatbeltProfile const& profile, Function<bool()> const& operation)
{
    auto child = fork();
    VERIFY(child >= 0);
    if (child == 0) {
        if (Sandbox::apply_macos_sandbox(profile).is_error())
            _exit(2);
        _exit(operation() ? 0 : 1);
    }

    int status = 0;
    VERIFY(waitpid(child, &status, 0) == child);
    if (WIFSIGNALED(status)) {
        VERIFY(WTERMSIG(status) == SIGKILL);
        return Outcome::Denied;
    }
    VERIFY(WIFEXITED(status));
    VERIFY(WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == 1);
    return WEXITSTATUS(status) == 0 ? Outcome::Allowed : Outcome::Denied;
}

static Outcome run_sandboxed(Function<bool()> const& operation)
{
    return run_sandboxed({}, operation);
}

// A scratch directory with one directory that the tests grant to the sandbox and one that they do not.
struct Fixture {
    Fixture()
    {
        char path_template[] = "/tmp/TestSeatbelt.XXXXXX";
        VERIFY(mkdtemp(path_template));
        char resolved[PATH_MAX];
        VERIFY(realpath(path_template, resolved));
        root = resolved;
        granted = ByteString::formatted("{}/granted", root);
        outside = ByteString::formatted("{}/outside", root);
        VERIFY(mkdir(granted.characters(), 0700) == 0);
        VERIFY(mkdir(outside.characters(), 0700) == 0);
        granted_file = create_file(granted, "file", "granted"sv);
        outside_file = create_file(outside, "file", "outside"sv);
    }

    ~Fixture()
    {
        auto command = ByteString::formatted("rm -rf '{}'", root);
        (void)system(command.characters());
    }

    static ByteString create_file(ByteString const& directory, char const* name, StringView contents)
    {
        auto path = ByteString::formatted("{}/{}", directory, name);
        auto fd = open(path.characters(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
        VERIFY(fd >= 0);
        VERIFY(write(fd, contents.characters_without_null_termination(), contents.length()) == static_cast<ssize_t>(contents.length()));
        VERIFY(close(fd) == 0);
        return path;
    }

    Vector<Sandbox::SeatbeltPath> granted_paths(Sandbox::SeatbeltPath::Access access = Sandbox::SeatbeltPath::Access::ReadOnly) const
    {
        Vector<Sandbox::SeatbeltPath> paths;
        MUST(Sandbox::add_seatbelt_path_if_exists(paths, granted, access));
        return paths;
    }

    ByteString root;
    ByteString granted;
    ByteString outside;
    ByteString granted_file;
    ByteString outside_file;
};

static bool can_open_for_reading(ByteString const& path)
{
    auto fd = open(path.characters(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    close(fd);
    return true;
}

TEST_CASE(sandboxed_process_can_do_ordinary_work)
{
    EXPECT_EQ(run_sandboxed([] {
        pthread_t thread;
        if (pthread_create(&thread, nullptr, [](void*) -> void* { return nullptr; }, nullptr) != 0)
            return false;
        pthread_join(thread, nullptr);

        auto* memory = static_cast<u8*>(malloc(16 * MiB));
        if (!memory)
            return false;
        memset(memory, 1, 16 * MiB);
        free(memory);

        int sockets[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
            return false;
        return write(sockets[0], "x", 1) == 1;
    }),
        Outcome::Allowed);
}

TEST_CASE(sandboxed_process_can_create_anonymous_shared_memory)
{
    EXPECT_EQ(run_sandboxed([] {
        auto buffer = Core::System::anon_create(64 * KiB, O_CLOEXEC);
        if (buffer.is_error())
            return false;
        auto* mapping = mmap(nullptr, 64 * KiB, PROT_READ | PROT_WRITE, MAP_SHARED, buffer.value(), 0);
        return mapping != MAP_FAILED;
    }),
        Outcome::Allowed);
}

TEST_CASE(sandboxed_process_reads_only_granted_paths)
{
    Fixture fixture;
    auto paths = fixture.granted_paths();

    EXPECT_EQ(run_sandboxed({ .paths = paths }, [&] { return can_open_for_reading(fixture.granted_file); }), Outcome::Allowed);
    EXPECT_EQ(run_sandboxed({ .paths = paths }, [&] { return can_open_for_reading(fixture.outside_file); }), Outcome::Denied);
}

#endif
