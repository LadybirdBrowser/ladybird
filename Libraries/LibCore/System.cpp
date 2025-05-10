/*
 * Copyright (c) 2021-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2021-2024, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, Matthias Zimmerman <matthias291999@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/FixedArray.h>
#include <AK/ScopeGuard.h>
#include <AK/ScopedValueRollback.h>
#include <AK/StdLibExtras.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibCore/Environment.h>
#include <LibCore/System.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#if defined(AK_OS_LINUX) && !defined(MFD_CLOEXEC)
#    include <linux/memfd.h>
#    include <sys/syscall.h>

static int memfd_create(char const* name, unsigned int flags)
{
    return syscall(SYS_memfd_create, name, flags);
}
#endif

#if defined(AK_OS_MACOS) || defined(AK_OS_IOS)
#    include <mach-o/dyld.h>
#    include <sys/mman.h>
#else
extern char** environ;
#endif

#if defined(AK_OS_BSD_GENERIC) && !defined(AK_OS_SOLARIS)
#    include <sys/sysctl.h>
#endif

#if defined(AK_OS_GNU_HURD)
extern "C" {
#    include <hurd.h>
}
#    include <LibCore/File.h>
#endif

#if defined(AK_OS_HAIKU)
#    include <image.h>
#endif

namespace Core::System {

#if !defined(AK_OS_MACOS) && !defined(AK_OS_IOS) && !defined(AK_OS_HAIKU)
ErrorOr<int> accept4(int sockfd, sockaddr* address, socklen_t* address_length, int flags)
{
    auto fd = ::accept4(sockfd, address, address_length, flags);
    if (fd < 0)
        return Error::from_syscall("accept4"sv, errno);
    return fd;
}
#endif

ErrorOr<void> sigaction(int signal, struct sigaction const* action, struct sigaction* old_action)
{
    if (::sigaction(signal, action, old_action) < 0)
        return Error::from_syscall("sigaction"sv, errno);
    return {};
}

#if defined(AK_OS_SOLARIS)
ErrorOr<SIG_TYP> signal(int signal, SIG_TYP handler)
#elif defined(AK_OS_BSD_GENERIC)
ErrorOr<sig_t> signal(int signal, sig_t handler)
#else
ErrorOr<sighandler_t> signal(int signal, sighandler_t handler)
#endif
{
    auto old_handler = ::signal(signal, handler);
    if (old_handler == SIG_ERR)
        return Error::from_syscall("signal"sv, errno);
    return old_handler;
}

ErrorOr<struct stat> fstat(int fd)
{
    struct stat st = {};
    if (::fstat(fd, &st) < 0)
        return Error::from_syscall("fstat"sv, errno);
    return st;
}

ErrorOr<struct stat> fstatat(int fd, StringView path, int flags)
{
    if (!path.characters_without_null_termination())
        return Error::from_syscall("fstatat"sv, EFAULT);

    struct stat st = {};
    ByteString path_string = path;

    if (::fstatat(fd, path_string.characters(), &st, flags) < 0)
        return Error::from_syscall("fstat"sv, errno);
    return st;
}

ErrorOr<int> fcntl(int fd, int command, ...)
{
    va_list ap;
    va_start(ap, command);
    uintptr_t extra_arg = va_arg(ap, uintptr_t);
    int rc = ::fcntl(fd, command, extra_arg);
    va_end(ap);
    if (rc < 0)
        return Error::from_syscall("fcntl"sv, errno);
    return rc;
}

ErrorOr<void*> mmap(void* address, size_t size, int protection, int flags, int fd, off_t offset, [[maybe_unused]] size_t alignment, [[maybe_unused]] StringView name)
{
    // NOTE: Regular POSIX mmap() doesn't support custom alignment requests.
    VERIFY(!alignment);
    auto* ptr = ::mmap(address, size, protection, flags, fd, offset);
    if (ptr == MAP_FAILED)
        return Error::from_syscall("mmap"sv, errno);
    return ptr;
}

ErrorOr<void> munmap(void* address, size_t size)
{
    if (::munmap(address, size) < 0)
        return Error::from_syscall("munmap"sv, errno);
    return {};
}

ErrorOr<int> anon_create([[maybe_unused]] size_t size, [[maybe_unused]] int options)
{
    int fd = -1;
#if defined(AK_OS_LINUX) || defined(AK_OS_FREEBSD)
    // FIXME: Support more options on Linux.
    auto linux_options = ((options & O_CLOEXEC) > 0) ? MFD_CLOEXEC : 0;
    fd = memfd_create("", linux_options);
    if (fd < 0)
        return Error::from_errno(errno);
    if (::ftruncate(fd, size) < 0) {
        auto saved_errno = errno;
        TRY(close(fd));
        return Error::from_errno(saved_errno);
    }
#elif defined(SHM_ANON)
    fd = shm_open(SHM_ANON, O_RDWR | O_CREAT | options, 0600);
    if (fd < 0)
        return Error::from_errno(errno);
    if (::ftruncate(fd, size) < 0) {
        auto saved_errno = errno;
        TRY(close(fd));
        return Error::from_errno(saved_errno);
    }
#elif defined(AK_OS_BSD_GENERIC) || defined(AK_OS_EMSCRIPTEN) || defined(AK_OS_HAIKU)
    static size_t shared_memory_id = 0;

    auto name = ByteString::formatted("/shm-{}-{}", getpid(), shared_memory_id++);
    fd = shm_open(name.characters(), O_RDWR | O_CREAT | options, 0600);

    if (shm_unlink(name.characters()) == -1) {
        auto saved_errno = errno;
        TRY(close(fd));
        return Error::from_errno(saved_errno);
    }

    if (fd < 0)
        return Error::from_errno(errno);

    if (::ftruncate(fd, size) < 0) {
        auto saved_errno = errno;
        TRY(close(fd));
        return Error::from_errno(saved_errno);
    }

    void* addr = ::mmap(NULL, size, PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        auto saved_errno = errno;
        TRY(close(fd));
        return Error::from_errno(saved_errno);
    }
#endif
    if (fd < 0)
        return Error::from_errno(errno);
    return fd;
}

ErrorOr<int> open(StringView path, int options, mode_t mode)
{
    return openat(AT_FDCWD, path, options, mode);
}

ErrorOr<int> openat(int fd, StringView path, int options, mode_t mode)
{
    if (!path.characters_without_null_termination())
        return Error::from_syscall("open"sv, EFAULT);

    // NOTE: We have to ensure that the path is null-terminated.
    ByteString path_string = path;
    int rc = ::openat(fd, path_string.characters(), options, mode);
    if (rc < 0)
        return Error::from_syscall("open"sv, errno);
    return rc;
}

ErrorOr<void> close(int fd)
{
    if (::close(fd) < 0)
        return Error::from_syscall("close"sv, errno);
    return {};
}

ErrorOr<void> ftruncate(int fd, off_t length)
{
    if (::ftruncate(fd, length) < 0)
        return Error::from_syscall("ftruncate"sv, errno);
    return {};
}

ErrorOr<struct stat> stat(StringView path)
{
    if (!path.characters_without_null_termination())
        return Error::from_syscall("stat"sv, EFAULT);

    struct stat st = {};

    ByteString path_string = path;
    if (::stat(path_string.characters(), &st) < 0)
        return Error::from_syscall("stat"sv, errno);
    return st;
}

ErrorOr<struct stat> lstat(StringView path)
{
    if (!path.characters_without_null_termination())
        return Error::from_syscall("lstat"sv, EFAULT);

    struct stat st = {};

    ByteString path_string = path;
    if (::lstat(path_string.characters(), &st) < 0)
        return Error::from_syscall("lstat"sv, errno);
    return st;
}

ErrorOr<ssize_t> read(int fd, Bytes buffer)
{
    ssize_t rc = ::read(fd, buffer.data(), buffer.size());
    if (rc < 0)
        return Error::from_syscall("read"sv, errno);
    return rc;
}

ErrorOr<ssize_t> write(int fd, ReadonlyBytes buffer)
{
    ssize_t rc = ::write(fd, buffer.data(), buffer.size());
    if (rc < 0)
        return Error::from_syscall("write"sv, errno);
    return rc;
}

ErrorOr<void> kill(pid_t pid, int signal)
{
    if (::kill(pid, signal) < 0)
        return Error::from_syscall("kill"sv, errno);
    return {};
}

ErrorOr<int> dup(int source_fd)
{
    int fd = ::dup(source_fd);
    if (fd < 0)
        return Error::from_syscall("dup"sv, errno);
    return fd;
}

ErrorOr<int> dup2(int source_fd, int destination_fd)
{
    int fd = ::dup2(source_fd, destination_fd);
    if (fd < 0)
        return Error::from_syscall("dup2"sv, errno);
    return fd;
}

ErrorOr<ByteString> getcwd()
{
    auto* cwd = ::getcwd(nullptr, 0);
    if (!cwd)
        return Error::from_syscall("getcwd"sv, errno);

    ByteString string_cwd(cwd);
    free(cwd);
    return string_cwd;
}

ErrorOr<void> ioctl(int fd, unsigned request, ...)
{
    va_list ap;
    va_start(ap, request);
#ifdef AK_OS_HAIKU
    void* arg = va_arg(ap, void*);
#else
    FlatPtr arg = va_arg(ap, FlatPtr);
#endif
    va_end(ap);
    if (::ioctl(fd, request, arg) < 0)
        return Error::from_syscall("ioctl"sv, errno);
    return {};
}

ErrorOr<struct termios> tcgetattr(int fd)
{
    struct termios ios = {};
    if (::tcgetattr(fd, &ios) < 0)
        return Error::from_syscall("tcgetattr"sv, errno);
    return ios;
}

ErrorOr<void> tcsetattr(int fd, int optional_actions, struct termios const& ios)
{
    if (::tcsetattr(fd, optional_actions, &ios) < 0)
        return Error::from_syscall("tcsetattr"sv, errno);
    return {};
}

ErrorOr<void> chmod(StringView pathname, mode_t mode)
{
    if (!pathname.characters_without_null_termination())
        return Error::from_syscall("chmod"sv, EFAULT);

    ByteString path = pathname;
    if (::chmod(path.characters(), mode) < 0)
        return Error::from_syscall("chmod"sv, errno);
    return {};
}

ErrorOr<void> fchmod(int fd, mode_t mode)
{
    if (::fchmod(fd, mode) < 0)
        return Error::from_syscall("fchmod"sv, errno);
    return {};
}

ErrorOr<void> fchown(int fd, uid_t uid, gid_t gid)
{
    if (::fchown(fd, uid, gid) < 0)
        return Error::from_syscall("fchown"sv, errno);
    return {};
}

ErrorOr<void> chown(StringView pathname, uid_t uid, gid_t gid)
{
    if (!pathname.characters_without_null_termination())
        return Error::from_syscall("chown"sv, EFAULT);

    ByteString path = pathname;
    if (::lchown(path.characters(), uid, gid) < 0)
        return Error::from_syscall("lchown"sv, errno);
    return {};
}

static ALWAYS_INLINE ErrorOr<pid_t> posix_spawn_wrapper(StringView path, posix_spawn_file_actions_t const* file_actions, posix_spawnattr_t const* attr, char* const arguments[], char* const envp[], StringView function_name, decltype(::posix_spawn) spawn_function)
{
    pid_t child_pid;
    if ((errno = spawn_function(&child_pid, path.to_byte_string().characters(), file_actions, attr, arguments, envp)))
        return Error::from_syscall(function_name, errno);
    return child_pid;
}

ErrorOr<pid_t> posix_spawn(StringView path, posix_spawn_file_actions_t const* file_actions, posix_spawnattr_t const* attr, char* const arguments[], char* const envp[])
{
    return posix_spawn_wrapper(path, file_actions, attr, arguments, envp, "posix_spawn"sv, ::posix_spawn);
}

ErrorOr<pid_t> posix_spawnp(StringView path, posix_spawn_file_actions_t* const file_actions, posix_spawnattr_t* const attr, char* const arguments[], char* const envp[])
{
    return posix_spawn_wrapper(path, file_actions, attr, arguments, envp, "posix_spawnp"sv, ::posix_spawnp);
}

ErrorOr<off_t> lseek(int fd, off_t offset, int whence)
{
    off_t rc = ::lseek(fd, offset, whence);
    if (rc < 0)
        return Error::from_syscall("lseek"sv, errno);
    return rc;
}

ErrorOr<WaitPidResult> waitpid(pid_t waitee, int options)
{
    int wstatus;
    pid_t pid = ::waitpid(waitee, &wstatus, options);
    if (pid < 0)
        return Error::from_syscall("waitpid"sv, errno);
    return WaitPidResult { pid, wstatus };
}

ErrorOr<bool> isatty(int fd)
{
    int rc = ::isatty(fd);
    if (rc < 0)
        return Error::from_syscall("isatty"sv, errno);
    return rc == 1;
}

ErrorOr<void> link(StringView old_path, StringView new_path)
{
    ByteString old_path_string = old_path;
    ByteString new_path_string = new_path;
    if (::link(old_path_string.characters(), new_path_string.characters()) < 0)
        return Error::from_syscall("link"sv, errno);
    return {};
}

ErrorOr<void> symlink(StringView target, StringView link_path)
{
    ByteString target_string = target;
    ByteString link_path_string = link_path;
    if (::symlink(target_string.characters(), link_path_string.characters()) < 0)
        return Error::from_syscall("symlink"sv, errno);
    return {};
}

ErrorOr<void> mkdir(StringView path, mode_t mode)
{
    if (path.is_null())
        return Error::from_errno(EFAULT);
    ByteString path_string = path;
    if (::mkdir(path_string.characters(), mode) < 0)
        return Error::from_syscall("mkdir"sv, errno);
    return {};
}

ErrorOr<void> chdir(StringView path)
{
    if (path.is_null())
        return Error::from_errno(EFAULT);

    ByteString path_string = path;
    if (::chdir(path_string.characters()) < 0)
        return Error::from_syscall("chdir"sv, errno);
    return {};
}

ErrorOr<void> rmdir(StringView path)
{
    if (path.is_null())
        return Error::from_errno(EFAULT);

    ByteString path_string = path;
    if (::rmdir(path_string.characters()) < 0)
        return Error::from_syscall("rmdir"sv, errno);
    return {};
}

ErrorOr<int> mkstemp(Span<char> pattern)
{
    int fd = ::mkstemp(pattern.data());
    if (fd < 0)
        return Error::from_syscall("mkstemp"sv, errno);
    return fd;
}

ErrorOr<String> mkdtemp(Span<char> pattern)
{
    auto* path = ::mkdtemp(pattern.data());
    if (path == nullptr) {
        return Error::from_errno(errno);
    }

    return String::from_utf8(StringView { path, strlen(path) });
}

ErrorOr<void> rename(StringView old_path, StringView new_path)
{
    if (old_path.is_null() || new_path.is_null())
        return Error::from_errno(EFAULT);

    ByteString old_path_string = old_path;
    ByteString new_path_string = new_path;
    if (::rename(old_path_string.characters(), new_path_string.characters()) < 0)
        return Error::from_syscall("rename"sv, errno);
    return {};
}

ErrorOr<void> unlink(StringView path)
{
    if (path.is_null())
        return Error::from_errno(EFAULT);

    ByteString path_string = path;
    if (::unlink(path_string.characters()) < 0)
        return Error::from_syscall("unlink"sv, errno);
    return {};
}

ErrorOr<void> utimensat(int fd, StringView path, struct timespec const times[2], int flag)
{
    if (path.is_null())
        return Error::from_errno(EFAULT);

    auto builder = TRY(StringBuilder::create());
    TRY(builder.try_append(path));
    TRY(builder.try_append('\0'));

    // Note the explicit null terminators above.
    if (::utimensat(fd, builder.string_view().characters_without_null_termination(), times, flag) < 0)
        return Error::from_syscall("utimensat"sv, errno);
    return {};
}

ErrorOr<struct utsname> uname()
{
    struct utsname uts;
    if (::uname(&uts) < 0)
        return Error::from_syscall("uname"sv, errno);
    return uts;
}

ErrorOr<int> socket(int domain, int type, int protocol)
{
    auto fd = ::socket(domain, type, protocol);
    if (fd < 0)
        return Error::from_syscall("socket"sv, errno);
    return fd;
}

ErrorOr<void> bind(int sockfd, struct sockaddr const* address, socklen_t address_length)
{
    if (::bind(sockfd, address, address_length) < 0)
        return Error::from_syscall("bind"sv, errno);
    return {};
}

ErrorOr<void> listen(int sockfd, int backlog)
{
    if (::listen(sockfd, backlog) < 0)
        return Error::from_syscall("listen"sv, errno);
    return {};
}

ErrorOr<int> accept(int sockfd, struct sockaddr* address, socklen_t* address_length)
{
    auto fd = ::accept(sockfd, address, address_length);
    if (fd < 0)
        return Error::from_syscall("accept"sv, errno);
    return fd;
}

ErrorOr<void> connect(int sockfd, struct sockaddr const* address, socklen_t address_length)
{
    if (::connect(sockfd, address, address_length) < 0)
        return Error::from_syscall("connect"sv, errno);
    return {};
}

ErrorOr<ssize_t> send(int sockfd, void const* buffer, size_t buffer_length, int flags)
{
    auto sent = ::send(sockfd, buffer, buffer_length, flags);
    if (sent < 0)
        return Error::from_syscall("send"sv, errno);
    return sent;
}

ErrorOr<ssize_t> sendmsg(int sockfd, const struct msghdr* message, int flags)
{
    auto sent = ::sendmsg(sockfd, message, flags);
    if (sent < 0)
        return Error::from_syscall("sendmsg"sv, errno);
    return sent;
}

ErrorOr<ssize_t> sendto(int sockfd, void const* source, size_t source_length, int flags, struct sockaddr const* destination, socklen_t destination_length)
{
    auto sent = ::sendto(sockfd, source, source_length, flags, destination, destination_length);
    if (sent < 0)
        return Error::from_syscall("sendto"sv, errno);
    return sent;
}

ErrorOr<ssize_t> recv(int sockfd, void* buffer, size_t length, int flags)
{
    auto received = ::recv(sockfd, buffer, length, flags);
    if (received < 0)
        return Error::from_syscall("recv"sv, errno);
    return received;
}

ErrorOr<ssize_t> recvmsg(int sockfd, struct msghdr* message, int flags)
{
    auto received = ::recvmsg(sockfd, message, flags);
    if (received < 0)
        return Error::from_syscall("recvmsg"sv, errno);
    return received;
}

ErrorOr<ssize_t> recvfrom(int sockfd, void* buffer, size_t buffer_length, int flags, struct sockaddr* address, socklen_t* address_length)
{
    auto received = ::recvfrom(sockfd, buffer, buffer_length, flags, address, address_length);
    if (received < 0)
        return Error::from_syscall("recvfrom"sv, errno);
    return received;
}

ErrorOr<AddressInfoVector> getaddrinfo(char const* nodename, char const* servname, struct addrinfo const& hints)
{
    struct addrinfo* results = nullptr;

    int const rc = ::getaddrinfo(nodename, servname, &hints, &results);
    if (rc != 0) {
        if (rc == EAI_SYSTEM) {
            return Error::from_syscall("getaddrinfo"sv, errno);
        }

        auto const* error_string = gai_strerror(rc);
        return Error::from_string_view({ error_string, strlen(error_string) });
    }

    Vector<struct addrinfo> addresses;

    for (auto* result = results; result != nullptr; result = result->ai_next)
        TRY(addresses.try_append(*result));

    return AddressInfoVector { move(addresses), results };
}

ErrorOr<void> getsockopt(int sockfd, int level, int option, void* value, socklen_t* value_size)
{
    if (::getsockopt(sockfd, level, option, value, value_size) < 0)
        return Error::from_syscall("getsockopt"sv, errno);
    return {};
}

ErrorOr<void> setsockopt(int sockfd, int level, int option, void const* value, socklen_t value_size)
{
    if (::setsockopt(sockfd, level, option, value, value_size) < 0)
        return Error::from_syscall("setsockopt"sv, errno);
    return {};
}

ErrorOr<void> getsockname(int sockfd, struct sockaddr* address, socklen_t* address_length)
{
    if (::getsockname(sockfd, address, address_length) < 0)
        return Error::from_syscall("getsockname"sv, errno);
    return {};
}

ErrorOr<void> getpeername(int sockfd, struct sockaddr* address, socklen_t* address_length)
{
    if (::getpeername(sockfd, address, address_length) < 0)
        return Error::from_syscall("getpeername"sv, errno);
    return {};
}

ErrorOr<void> socketpair(int domain, int type, int protocol, int sv[2])
{
    if (::socketpair(domain, type, protocol, sv) < 0)
        return Error::from_syscall("socketpair"sv, errno);
    return {};
}

ErrorOr<Array<int, 2>> pipe2(int flags)
{
    Array<int, 2> fds;

#if defined(__unix__)
    if (::pipe2(fds.data(), flags) < 0)
        return Error::from_syscall("pipe2"sv, errno);
#else
    if (::pipe(fds.data()) < 0)
        return Error::from_syscall("pipe2"sv, errno);

    // Ensure we don't leak the fds if any of the system calls below fail.
    AK::ArmedScopeGuard close_fds { [&]() {
        MUST(close(fds[0]));
        MUST(close(fds[1]));
    } };

    if ((flags & O_CLOEXEC) != 0) {
        TRY(fcntl(fds[0], F_SETFD, FD_CLOEXEC));
        TRY(fcntl(fds[1], F_SETFD, FD_CLOEXEC));
    }
    if ((flags & O_NONBLOCK) != 0) {
        TRY(fcntl(fds[0], F_SETFL, TRY(fcntl(fds[0], F_GETFL)) | O_NONBLOCK));
        TRY(fcntl(fds[1], F_SETFL, TRY(fcntl(fds[1], F_GETFL)) | O_NONBLOCK));
    }

    close_fds.disarm();
#endif

    return fds;
}

ErrorOr<void> access(StringView pathname, int mode, int flags)
{
    if (pathname.is_null())
        return Error::from_syscall("access"sv, EFAULT);

    ByteString path_string = pathname;
    (void)flags;

    if (::access(path_string.characters(), mode) < 0)
        return Error::from_syscall("access"sv, errno);
    return {};
}

ErrorOr<ByteString> readlink(StringView pathname)
{
    // FIXME: Try again with a larger buffer.
#if defined(AK_OS_GNU_HURD)
    // PATH_MAX is not defined, nor is there an upper limit on path lengths.
    // Let's do this the right way.
    int fd = TRY(open(pathname, O_READ | O_NOLINK));
    auto file = TRY(File::adopt_fd(fd, File::OpenMode::Read));
    auto buffer = TRY(file->read_until_eof());
    // TODO: Get rid of this copy here.
    return ByteString::copy(buffer);
#else
    char data[PATH_MAX];
    ByteString path_string = pathname;
    int rc = ::readlink(path_string.characters(), data, sizeof(data));
    if (rc == -1)
        return Error::from_syscall("readlink"sv, errno);

    return ByteString(data, rc);
#endif
}

ErrorOr<int> poll(Span<struct pollfd> poll_fds, int timeout)
{
    auto const rc = ::poll(poll_fds.data(), poll_fds.size(), timeout);
    if (rc < 0)
        return Error::from_syscall("poll"sv, errno);
    return { rc };
}

unsigned hardware_concurrency()
{
    return sysconf(_SC_NPROCESSORS_ONLN);
}

u64 physical_memory_bytes()
{
    return sysconf(_SC_PHYS_PAGES) * PAGE_SIZE;
}

ErrorOr<ByteString> current_executable_path()
{
    char path[4096] = {};
#if defined(AK_OS_LINUX) || defined(AK_OS_ANDROID)
    auto ret = ::readlink("/proc/self/exe", path, sizeof(path) - 1);
    // Ignore error if it wasn't a symlink
    if (ret == -1 && errno != EINVAL)
        return Error::from_syscall("readlink"sv, errno);
#elif defined(AK_OS_GNU_HURD)
    // We could read /proc/self/exe, but why rely on procfs being mounted
    // if we can do the same thing procfs does and ask the proc server directly?
    process_t proc = getproc();
    if (!MACH_PORT_VALID(proc))
        return Error::from_syscall("getproc"sv, errno);
    kern_return_t err = proc_get_exe(proc, getpid(), path);
    mach_port_deallocate(mach_task_self(), proc);
    if (err) {
        __hurd_fail(static_cast<error_t>(err));
        return Error::from_syscall("proc_get_exe"sv, errno);
    }
#elif defined(AK_OS_DRAGONFLY)
    return TRY(readlink("/proc/curproc/file"sv));
#elif defined(AK_OS_SOLARIS)
    return TRY(readlink("/proc/self/path/a.out"sv));
#elif defined(AK_OS_FREEBSD)
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };
    size_t len = sizeof(path);
    if (sysctl(mib, 4, path, &len, nullptr, 0) < 0)
        return Error::from_syscall("sysctl"sv, errno);
#elif defined(AK_OS_NETBSD)
    int mib[4] = { CTL_KERN, KERN_PROC_ARGS, -1, KERN_PROC_PATHNAME };
    size_t len = sizeof(path);
    if (sysctl(mib, 4, path, &len, nullptr, 0) < 0)
        return Error::from_syscall("sysctl"sv, errno);
#elif defined(AK_OS_MACOS) || defined(AK_OS_IOS)
    u32 size = sizeof(path);
    auto ret = _NSGetExecutablePath(path, &size);
    if (ret != 0)
        return Error::from_errno(ENAMETOOLONG);
#elif defined(AK_OS_HAIKU)
    image_info info = {};
    for (int32 cookie { 0 }; get_next_image_info(B_CURRENT_TEAM, &cookie, &info) == B_OK && info.type != B_APP_IMAGE;)
        ;
    if (info.type != B_APP_IMAGE)
        return Error::from_string_literal("current_executable_path() failed");
    if (sizeof(info.name) > sizeof(path))
        return Error::from_errno(ENAMETOOLONG);
    strlcpy(path, info.name, sizeof(path) - 1);
#elif defined(AK_OS_EMSCRIPTEN)
    return Error::from_string_literal("current_executable_path() unknown on this platform");
#else
#    warning "Not sure how to get current_executable_path on this platform!"
    // GetModuleFileName on Windows, unsure about OpenBSD.
    return Error::from_string_literal("current_executable_path unknown");
#endif
    path[sizeof(path) - 1] = '\0';
    return ByteString { path, strlen(path) };
}

ErrorOr<rlimit> get_resource_limits(int resource)
{
    rlimit limits;

    if (::getrlimit(resource, &limits) != 0)
        return Error::from_syscall("getrlimit"sv, errno);

    return limits;
}

ErrorOr<void> set_resource_limits(int resource, rlim_t limit)
{
    auto limits = TRY(get_resource_limits(resource));
    limits.rlim_cur = min(limit, limits.rlim_max);

    if (::setrlimit(resource, &limits) != 0)
        return Error::from_syscall("setrlimit"sv, errno);

    return {};
}

int getpid()
{
    return ::getpid();
}

bool is_socket(int fd)
{
    auto result = fstat(fd);
    return !result.is_error() && S_ISSOCK(result.value().st_mode);
}

ErrorOr<void> sleep_ms(u32 milliseconds)
{
    if (usleep(1000 * milliseconds) != 0)
        return Error::from_syscall("usleep"sv, errno);
    return {};
}

ErrorOr<void> set_close_on_exec(int fd, bool enabled)
{
    int flags = TRY(fcntl(fd, F_GETFD));

    if (enabled)
        flags |= FD_CLOEXEC;
    else
        flags &= ~FD_CLOEXEC;

    TRY(fcntl(fd, F_SETFD, flags));
    return {};
}

}
