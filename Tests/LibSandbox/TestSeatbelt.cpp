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
#    include <IOKit/kext/KextManager.h>
#    include <LibCore/MachPort.h>
#    include <LibCore/System.h>
#    include <LibSandbox/Sandbox.h>
#    include <LibTest/TestCase.h>
#    include <errno.h>
#    include <fcntl.h>
#    include <libproc.h>
#    include <limits.h>
#    include <mach-o/dyld.h>
#    include <mach-o/loader.h>
#    include <mach/mach.h>
#    include <net/route.h>
#    include <pthread.h>
#    include <servers/bootstrap.h>
#    include <signal.h>
#    include <stdlib.h>
#    include <sys/mman.h>
#    include <sys/socket.h>
#    include <sys/stat.h>
#    include <sys/syscall.h>
#    include <sys/sysctl.h>
#    include <sys/ttycom.h>
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

// Registers a bootstrap service in the test process, standing in for the Browser endpoint or for an unrelated service.
static ByteString register_bootstrap_service(StringView purpose)
{
    auto name = ByteString::formatted("org.ladybird.TestSeatbelt.{}.{}", purpose, getpid());
    auto port = MUST(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));
    auto send_right = MUST(port.insert_right(Core::MachPort::MessageRight::MakeSend));
    MUST(send_right.register_with_bootstrap_server(name));
    (void)port.release();
    (void)send_right.release();
    return name;
}

static bool can_look_up_bootstrap_service(ByteString const& name)
{
    mach_port_t port = MACH_PORT_NULL;
    return bootstrap_look_up(bootstrap_port, name.characters(), &port) == KERN_SUCCESS;
}

TEST_CASE(sandboxed_process_looks_up_only_its_own_mach_server)
{
    static auto browser_endpoint = register_bootstrap_service("browser"sv);
    static auto unrelated_service = register_bootstrap_service("unrelated"sv);

    EXPECT_EQ(run_sandboxed({ .mach_server_name = browser_endpoint }, [&] { return can_look_up_bootstrap_service(browser_endpoint); }), Outcome::Allowed);
    EXPECT_EQ(run_sandboxed({ .mach_server_name = browser_endpoint }, [&] { return can_look_up_bootstrap_service(unrelated_service); }), Outcome::Denied);
    EXPECT_EQ(run_sandboxed({ .mach_server_name = browser_endpoint }, [&] { return can_look_up_bootstrap_service("com.apple.pasteboard.1"); }), Outcome::Denied);
}

TEST_CASE(sandboxed_process_cannot_obtain_task_ports_for_other_processes)
{
    auto parent = getpid();
    EXPECT_EQ(run_sandboxed([&] {
        mach_port_t task = MACH_PORT_NULL;
        return task_name_for_pid(mach_task_self(), parent, &task) == KERN_SUCCESS;
    }),
        Outcome::Denied);
    EXPECT_EQ(run_sandboxed([&] {
        mach_port_t task = MACH_PORT_NULL;
        return task_for_pid(mach_task_self(), parent, &task) == KERN_SUCCESS;
    }),
        Outcome::Denied);
}

TEST_CASE(sandboxed_process_inspects_only_itself)
{
    auto parent = getpid();

    EXPECT_EQ(run_sandboxed([] {
        proc_bsdinfo info {};
        return proc_pidinfo(getpid(), PROC_PIDTBSDINFO, 0, &info, sizeof(info)) == sizeof(info);
    }),
        Outcome::Allowed);
    EXPECT_EQ(run_sandboxed([&] {
        proc_bsdinfo info {};
        return proc_pidinfo(parent, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) == sizeof(info);
    }),
        Outcome::Denied);
    EXPECT_EQ(run_sandboxed([] {
        pid_t pids[1024];
        return proc_listallpids(pids, sizeof(pids)) > 0;
    }),
        Outcome::Denied);
}

static bool can_read_sysctl(StringView name)
{
    char value[4096];
    size_t size = sizeof(value);
    return sysctlbyname(ByteString(name).characters(), value, &size, nullptr, 0) == 0;
}

TEST_CASE(sandboxed_process_reads_only_allowed_sysctls)
{
    EXPECT_EQ(run_sandboxed([] { return can_read_sysctl("hw.ncpu"sv) && can_read_sysctl("hw.optional.arm64"sv); }), Outcome::Allowed);

    // Another process's arguments and environment.
    auto parent = getpid();
    EXPECT_EQ(run_sandboxed([&] {
        int mib[] = { CTL_KERN, KERN_PROCARGS2, parent };
        char arguments[4096];
        size_t size = sizeof(arguments);
        return sysctl(mib, 3, arguments, &size, nullptr, 0) == 0;
    }),
        Outcome::Denied);

    // The system-wide TCP connection table.
    EXPECT_EQ(run_sandboxed([] {
        size_t size = 0;
        return sysctlbyname("net.inet.tcp.pcblist64", nullptr, &size, nullptr, 0) == 0;
    }),
        Outcome::Denied);
}

TEST_CASE(sandboxed_process_cannot_open_route_sockets)
{
    EXPECT_EQ(run_sandboxed([] {
        auto fd = socket(PF_ROUTE, SOCK_RAW, 0);
        return fd >= 0;
    }),
        Outcome::Denied);
}

TEST_CASE(sandboxed_process_cannot_query_loaded_kernel_extensions)
{
    EXPECT_EQ(run_sandboxed([] {
        auto info = KextManagerCopyLoadedKextInfo(nullptr, nullptr);
        if (!info)
            return false;
        auto count = CFDictionaryGetCount(info);
        CFRelease(info);
        return count > 0;
    }),
        Outcome::Denied);
}

// NECP descriptors answer questions about network interfaces, such as their private addresses.
static int open_necp_descriptor()
{
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wdeprecated-declarations"
    return syscall(SYS_necp_open, 0);
#    pragma clang diagnostic pop
}

TEST_CASE(sandboxed_process_without_network_cannot_query_interface_addresses)
{
    EXPECT_EQ(run_sandboxed([] { return open_necp_descriptor() >= 0; }),
        Outcome::Denied);
}

TEST_CASE(sandboxed_process_cannot_open_existing_shared_memory)
{
    // One object with an arbitrary name, and one that matches the names used by Core::System::anon_create().
    auto unrelated_name = ByteString::formatted("/TestSeatbelt-{}", getpid());
    auto anonymous_name = ByteString::formatted("/shm-{:016x}-{:08x}", static_cast<u64>(getpid()), 0u);
    for (auto const& name : { unrelated_name, anonymous_name }) {
        auto fd = shm_open(name.characters(), O_RDWR | O_CREAT | O_EXCL, 0600);
        VERIFY(fd >= 0);
        VERIFY(ftruncate(fd, 4096) == 0);
        close(fd);
    }

    for (auto const& name : { unrelated_name, anonymous_name }) {
        EXPECT_EQ(run_sandboxed([&] { return shm_open(name.characters(), O_RDWR) >= 0; }), Outcome::Denied);
        EXPECT_EQ(run_sandboxed([&] { return shm_open(name.characters(), O_RDWR | O_CREAT, 0600) >= 0; }), Outcome::Denied);
        shm_unlink(name.characters());
    }
}

TEST_CASE(sandboxed_process_cannot_change_shared_file_state_through_fcntl)
{
    Fixture fixture;
    auto paths = fixture.granted_paths();
    auto run_fcntl = [&](int command, auto argument) {
        return run_sandboxed({ .paths = paths }, [&] {
            auto fd = open(fixture.granted_file.characters(), O_RDONLY | O_CLOEXEC);
            if (fd < 0)
                return false;
            return fcntl(fd, command, argument) != -1;
        });
    };

    EXPECT_EQ(run_fcntl(F_GETFL, 0), Outcome::Allowed);
    EXPECT_EQ(run_fcntl(F_SETFD, FD_CLOEXEC), Outcome::Allowed);

    // Changes caching for every descriptor of the file.
    EXPECT_EQ(run_fcntl(F_GLOBAL_NOCACHE, 1), Outcome::Denied);
    // Changes the file's data protection class.
    EXPECT_EQ(run_fcntl(F_SETPROTECTIONCLASS, 3), Outcome::Denied);
    // Reveals the path of a descriptor that was handed over without one.
    char path[PATH_MAX];
    EXPECT_EQ(run_fcntl(F_GETPATH, path), Outcome::Denied);

    // The GPU service needs F_GETPATH, and it never receives files from the Browser.
    EXPECT_EQ(run_sandboxed({ .paths = paths, .system_services = Sandbox::SystemService::GPU }, [&] {
        auto fd = open(fixture.granted_file.characters(), O_RDONLY | O_CLOEXEC);
        char path[PATH_MAX];
        return fd >= 0 && fcntl(fd, F_GETPATH, path) != -1;
    }),
        Outcome::Allowed);
}

TEST_CASE(sandboxed_process_cannot_preallocate_disk_space_through_inherited_files)
{
    Fixture fixture;
    auto fd = open(fixture.outside_file.characters(), O_RDWR | O_CLOEXEC);
    VERIFY(fd >= 0);

    EXPECT_EQ(run_sandboxed([&] {
        fstore_t store { .fst_flags = F_ALLOCATEALL | F_ALLOCATEPERSIST, .fst_posmode = F_PEOFPOSMODE, .fst_offset = 0, .fst_length = 1 * MiB, .fst_bytesalloc = 0 };
        return fcntl(fd, F_PREALLOCATE, &store) != -1;
    }),
        Outcome::Denied);
    close(fd);
}

#    ifndef F_TRANSFEREXTENTS
#        define F_TRANSFEREXTENTS 110
#    endif

TEST_CASE(sandboxed_process_cannot_move_disk_reservations_out_of_readonly_files)
{
    Fixture fixture;
    auto source = open(fixture.granted_file.characters(), O_RDWR | O_CLOEXEC);
    VERIFY(source >= 0);
    fstore_t store { .fst_flags = F_ALLOCATEALL, .fst_posmode = F_PEOFPOSMODE, .fst_offset = 0, .fst_length = 1 * MiB, .fst_bytesalloc = 0 };
    VERIFY(fcntl(source, F_PREALLOCATE, &store) == 0);
    close(source);
    auto target_path = Fixture::create_file(fixture.outside, "target", ""sv);

    Vector<Sandbox::SeatbeltPath> paths;
    MUST(Sandbox::add_seatbelt_path_if_exists(paths, fixture.granted, Sandbox::SeatbeltPath::Access::ReadOnly));
    MUST(Sandbox::add_seatbelt_path_if_exists(paths, fixture.outside, Sandbox::SeatbeltPath::Access::ReadWrite));
    EXPECT_EQ(run_sandboxed({ .paths = paths }, [&] {
        auto source = open(fixture.granted_file.characters(), O_RDONLY | O_CLOEXEC);
        auto target = open(target_path.characters(), O_RDWR | O_CLOEXEC);
        if (source < 0 || target < 0)
            return false;
        return fcntl(source, F_TRANSFEREXTENTS, target) != -1;
    }),
        Outcome::Denied);
}

TEST_CASE(sandboxed_process_cannot_redirect_sigio_to_another_process)
{
    auto parent = getpid();
    EXPECT_EQ(run_sandboxed([&] {
        int sockets[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
            return false;
        return fcntl(sockets[0], F_SETOWN, parent) != -1;
    }),
        Outcome::Denied);
}

// Finds the embedded code signature of the running executable, so a test can try to attach it to the file's vnode.
static Optional<fsignatures_t> code_signature_of_executable()
{
    auto const* header = reinterpret_cast<mach_header_64 const*>(_dyld_get_image_header(0));
    auto const* command = reinterpret_cast<load_command const*>(header + 1);
    for (u32 i = 0; i < header->ncmds; ++i) {
        if (command->cmd == LC_CODE_SIGNATURE) {
            auto const* signature = reinterpret_cast<linkedit_data_command const*>(command);
            fsignatures_t request {};
            request.fs_file_start = 0;
            request.fs_blob_start = reinterpret_cast<void*>(static_cast<uintptr_t>(signature->dataoff));
            request.fs_blob_size = signature->datasize;
            return request;
        }
        command = reinterpret_cast<load_command const*>(reinterpret_cast<u8 const*>(command) + command->cmdsize);
    }
    return {};
}

TEST_CASE(sandboxed_process_cannot_attach_code_signatures_to_readonly_files)
{
    auto executable = MUST(Core::System::current_executable_path());
    auto signature = code_signature_of_executable();
    VERIFY(signature.has_value());

    Vector<Sandbox::SeatbeltPath> paths;
    MUST(Sandbox::add_seatbelt_path_if_exists(paths, executable, Sandbox::SeatbeltPath::Access::ReadOnly));
    EXPECT_EQ(run_sandboxed({ .paths = paths }, [&] {
        auto fd = open(executable.characters(), O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return false;
        auto request = *signature;
        return fcntl(fd, F_ADDFILESIGS, &request) != -1;
    }),
        Outcome::Denied);
}

// fcntl() forwards unknown commands to the descriptor's ioctl handler, which the libc wrapper would not pass through.
static int raw_fcntl(int fd, unsigned long command)
{
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wdeprecated-declarations"
    return syscall(SYS_fcntl, fd, static_cast<int>(command), 0);
#    pragma clang diagnostic pop
}

TEST_CASE(sandboxed_process_cannot_control_a_terminal_through_fcntl)
{
    auto controller = posix_openpt(O_RDWR | O_NOCTTY);
    VERIFY(controller >= 0);
    VERIFY(grantpt(controller) == 0 && unlockpt(controller) == 0);
    auto terminal = open(ptsname(controller), O_RDWR | O_NOCTTY);
    VERIFY(terminal >= 0);

    EXPECT_EQ(run_sandboxed([&] { return raw_fcntl(terminal, TIOCSTOP) != -1; }), Outcome::Denied);

    close(terminal);
    close(controller);
}

#endif
