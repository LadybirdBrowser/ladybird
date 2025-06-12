/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2021-2024, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Noncopyable.h>
#include <AK/OwnPtr.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>

#if !defined(AK_OS_WINDOWS)
#    include <netdb.h>
#    include <poll.h>
#    include <pwd.h>
#    include <spawn.h>
#    include <sys/ioctl.h>
#    include <sys/resource.h>
#    include <sys/socket.h>
#    include <sys/time.h>
#    include <sys/utsname.h>
#    include <sys/wait.h>
#    include <termios.h>
#    include <utime.h>
#else
#    include "SocketAddressWindows.h"
#    include <io.h>
#    define O_CLOEXEC O_NOINHERIT
#    define STDIN_FILENO _get_osfhandle(_fileno(stdin))
#    define STDOUT_FILENO _get_osfhandle(_fileno(stdout))
#    define STDERR_FILENO _get_osfhandle(_fileno(stderr))
#    define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)
#    define S_ISREG(mode) (((mode) & S_IFMT) == S_IFREG)
using sighandler_t = void (*)(int);
#endif

#if !defined(AK_OS_BSD_GENERIC) && !defined(AK_OS_ANDROID) && !defined(AK_OS_WINDOWS)
#    include <shadow.h>
#endif

#ifdef AK_OS_FREEBSD
#    include <sys/ucred.h>
#endif

#ifdef AK_OS_SOLARIS
#    include <sys/filio.h>
#    include <ucred.h>
#endif

namespace Core::System {

#if !defined(AK_OS_MACOS) && !defined(AK_OS_HAIKU)
ErrorOr<int> accept4(int sockfd, struct sockaddr*, socklen_t*, int flags);
#endif

ErrorOr<void> sigaction(int signal, struct sigaction const* action, struct sigaction* old_action);
#if defined(AK_OS_SOLARIS)
ErrorOr<SIG_TYP> signal(int signal, SIG_TYP handler);
#elif defined(AK_OS_BSD_GENERIC)
ErrorOr<sig_t> signal(int signal, sig_t handler);
#else
ErrorOr<sighandler_t> signal(int signal, sighandler_t handler);
#endif
ErrorOr<struct stat> fstat(int fd);
ErrorOr<struct stat> fstatat(int fd, StringView path, int flags);
ErrorOr<int> fcntl(int fd, int command, ...);
ErrorOr<void*> mmap(void* address, size_t, int protection, int flags, int fd, off_t, size_t alignment = 0, StringView name = {});
ErrorOr<void> munmap(void* address, size_t);
ErrorOr<int> anon_create(size_t size, int options);
ErrorOr<int> open(StringView path, int options, mode_t mode = 0);
ErrorOr<int> openat(int fd, StringView path, int options, mode_t mode = 0);
ErrorOr<void> close(int fd);
ErrorOr<void> ftruncate(int fd, off_t length);
ErrorOr<struct stat> stat(StringView path);
ErrorOr<struct stat> lstat(StringView path);
ErrorOr<ssize_t> read(int fd, Bytes buffer);
ErrorOr<ssize_t> write(int fd, ReadonlyBytes buffer);
ErrorOr<int> dup(int source_fd);
ErrorOr<int> dup2(int source_fd, int destination_fd);
ErrorOr<ByteString> getcwd();
ErrorOr<void> ioctl(int fd, unsigned request, ...);
ErrorOr<struct termios> tcgetattr(int fd);
ErrorOr<void> tcsetattr(int fd, int optional_actions, struct termios const&);
ErrorOr<void> chmod(StringView pathname, mode_t mode);
ErrorOr<off_t> lseek(int fd, off_t, int whence);

ErrorOr<bool> isatty(int fd);
ErrorOr<void> link(StringView old_path, StringView new_path);
ErrorOr<void> symlink(StringView target, StringView link_path);
ErrorOr<void> mkdir(StringView path, mode_t);
ErrorOr<void> chdir(StringView path);
ErrorOr<void> rmdir(StringView path);
ErrorOr<int> mkstemp(Span<char> pattern);
ErrorOr<String> mkdtemp(Span<char> pattern);
ErrorOr<void> fchmod(int fd, mode_t mode);
ErrorOr<void> rename(StringView old_path, StringView new_path);
ErrorOr<void> unlink(StringView path);
ErrorOr<void> utimensat(int fd, StringView path, struct timespec const times[2], int flag);
ErrorOr<Array<int, 2>> pipe2(int flags);

ErrorOr<int> socket(int domain, int type, int protocol);
ErrorOr<void> bind(int sockfd, struct sockaddr const*, socklen_t);
ErrorOr<void> listen(int sockfd, int backlog);
ErrorOr<int> accept(int sockfd, struct sockaddr*, socklen_t*);
ErrorOr<void> connect(int sockfd, struct sockaddr const*, socklen_t);
ErrorOr<ssize_t> send(int sockfd, void const*, size_t, int flags);
ErrorOr<ssize_t> sendmsg(int sockfd, const struct msghdr*, int flags);
ErrorOr<ssize_t> sendto(int sockfd, void const*, size_t, int flags, struct sockaddr const*, socklen_t);
ErrorOr<ssize_t> recv(int sockfd, void*, size_t, int flags);
ErrorOr<ssize_t> recvmsg(int sockfd, struct msghdr*, int flags);
ErrorOr<ssize_t> recvfrom(int sockfd, void*, size_t, int flags, struct sockaddr*, socklen_t*);
ErrorOr<void> getsockopt(int sockfd, int level, int option, void* value, socklen_t* value_size);
ErrorOr<void> setsockopt(int sockfd, int level, int option, void const* value, socklen_t value_size);
ErrorOr<void> getsockname(int sockfd, struct sockaddr*, socklen_t*);
ErrorOr<void> getpeername(int sockfd, struct sockaddr*, socklen_t*);
ErrorOr<void> socketpair(int domain, int type, int protocol, int sv[2]);

ErrorOr<void> access(StringView pathname, int mode, int flags = 0);
ErrorOr<ByteString> readlink(StringView pathname);
ErrorOr<int> poll(Span<struct pollfd>, int timeout);

#if !defined(AK_OS_WINDOWS)
ErrorOr<void> kill(pid_t, int signal);
ErrorOr<void> chown(StringView pathname, uid_t uid, gid_t gid);
ErrorOr<pid_t> posix_spawn(StringView path, posix_spawn_file_actions_t const* file_actions, posix_spawnattr_t const* attr, char* const arguments[], char* const envp[]);
ErrorOr<pid_t> posix_spawnp(StringView path, posix_spawn_file_actions_t* const file_actions, posix_spawnattr_t* const attr, char* const arguments[], char* const envp[]);

struct WaitPidResult {
    pid_t pid;
    int status;
};
ErrorOr<WaitPidResult> waitpid(pid_t waitee, int options = 0);
ErrorOr<void> fchown(int fd, uid_t, gid_t);
ErrorOr<struct utsname> uname();

class AddressInfoVector {
    AK_MAKE_NONCOPYABLE(AddressInfoVector);
    AK_MAKE_DEFAULT_MOVABLE(AddressInfoVector);

public:
    ~AddressInfoVector() = default;

    ReadonlySpan<struct addrinfo> addresses() const { return m_addresses; }

private:
    friend ErrorOr<AddressInfoVector> getaddrinfo(char const* nodename, char const* servname, struct addrinfo const& hints);

    AddressInfoVector(Vector<struct addrinfo>&& addresses, struct addrinfo* ptr)
        : m_addresses(move(addresses))
        , m_ptr(adopt_own_if_nonnull(ptr))
    {
    }

    struct AddrInfoDeleter {
        void operator()(struct addrinfo* ptr)
        {
            if (ptr)
                ::freeaddrinfo(ptr);
        }
    };

    Vector<struct addrinfo> m_addresses {};
    OwnPtr<struct addrinfo, AddrInfoDeleter> m_ptr {};
};

ErrorOr<AddressInfoVector> getaddrinfo(char const* nodename, char const* servname, struct addrinfo const& hints);
#endif

unsigned hardware_concurrency();
u64 physical_memory_bytes();

ErrorOr<ByteString> current_executable_path();

#if !defined(AK_OS_WINDOWS)
ErrorOr<rlimit> get_resource_limits(int resource);
ErrorOr<void> set_resource_limits(int resource, rlim_t limit);
#endif

int getpid();
bool is_socket(int fd);
ErrorOr<void> sleep_ms(u32 milliseconds);
ErrorOr<void> set_close_on_exec(int fd, bool enabled);

}
