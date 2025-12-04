/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2021-2024, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/StringView.h>
#include <LibCore/AddressInfoVector.h>
#include <LibCore/Export.h>
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
#    include <sys/wait.h>
#    include <termios.h>
#    include <utime.h>
#else
#    include <io.h>

#    define O_CLOEXEC O_NOINHERIT
#    define STDIN_FILENO to_fd(reinterpret_cast<void*>(_get_osfhandle(_fileno(stdin))))
#    define STDOUT_FILENO to_fd(reinterpret_cast<void*>(_get_osfhandle(_fileno(stdout))))
#    define STDERR_FILENO to_fd(reinterpret_cast<void*>(_get_osfhandle(_fileno(stderr))))
#    define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)
#    define S_ISREG(mode) (((mode) & S_IFMT) == S_IFREG)

using sighandler_t = void (*)(int);
using socklen_t = int;

struct addrinfo;
struct sockaddr;
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
CORE_API ErrorOr<SIG_TYP> signal(int signal, SIG_TYP handler);
#elif defined(AK_OS_BSD_GENERIC)
CORE_API ErrorOr<sig_t> signal(int signal, sig_t handler);
#else
CORE_API ErrorOr<sighandler_t> signal(int signal, sighandler_t handler);
#endif
CORE_API ErrorOr<struct stat> fstat(int fd);
ErrorOr<struct stat> fstatat(int fd, StringView path, int flags);
CORE_API ErrorOr<int> fcntl(int fd, int command, ...);
ErrorOr<void*> mmap(void* address, size_t, int protection, int flags, int fd, off_t, size_t alignment = 0, StringView name = {});
ErrorOr<void> munmap(void* address, size_t);
ErrorOr<int> anon_create(size_t size, int options);
CORE_API ErrorOr<int> open(StringView path, int options, mode_t mode = 0);
ErrorOr<int> openat(int fd, StringView path, int options, mode_t mode = 0);
CORE_API ErrorOr<void> close(int fd);
ErrorOr<void> ftruncate(int fd, off_t length);
CORE_API ErrorOr<struct stat> stat(StringView path);
CORE_API ErrorOr<struct stat> lstat(StringView path);
CORE_API ErrorOr<size_t> read(int fd, Bytes buffer);
CORE_API ErrorOr<size_t> write(int fd, ReadonlyBytes buffer);
CORE_API ErrorOr<int> dup(int source_fd);
ErrorOr<int> dup2(int source_fd, int destination_fd);
CORE_API ErrorOr<ByteString> getcwd();
CORE_API ErrorOr<void> ioctl(int fd, unsigned request, ...);
ErrorOr<struct termios> tcgetattr(int fd);
ErrorOr<void> tcsetattr(int fd, int optional_actions, struct termios const&);
CORE_API ErrorOr<void> chmod(StringView pathname, mode_t mode);
ErrorOr<off_t> lseek(int fd, off_t, int whence);

CORE_API ErrorOr<bool> isatty(int fd);
CORE_API ErrorOr<void> link(StringView old_path, StringView new_path);
CORE_API ErrorOr<void> symlink(StringView target, StringView link_path);
CORE_API ErrorOr<void> mkdir(StringView path, mode_t);
CORE_API ErrorOr<void> chdir(StringView path);
CORE_API ErrorOr<void> rmdir(StringView path);
CORE_API ErrorOr<int> mkstemp(Span<char> pattern);
CORE_API ErrorOr<void> fchmod(int fd, mode_t mode);
CORE_API ErrorOr<void> rename(StringView old_path, StringView new_path);
CORE_API ErrorOr<void> unlink(StringView path);
CORE_API ErrorOr<void> utimensat(int fd, StringView path, struct timespec const times[2], int flag);
CORE_API ErrorOr<Array<int, 2>> pipe2(int flags);

CORE_API ErrorOr<int> socket(int domain, int type, int protocol);
CORE_API ErrorOr<void> bind(int sockfd, struct sockaddr const*, socklen_t);
CORE_API ErrorOr<void> listen(int sockfd, int backlog);
ErrorOr<int> accept(int sockfd, struct sockaddr*, socklen_t*);
ErrorOr<void> connect(int sockfd, struct sockaddr const*, socklen_t);
CORE_API ErrorOr<size_t> send(int sockfd, ReadonlyBytes, int flags);
ErrorOr<size_t> sendmsg(int sockfd, const struct msghdr*, int flags);
ErrorOr<size_t> sendto(int sockfd, ReadonlyBytes, int flags, struct sockaddr const*, socklen_t);
ErrorOr<size_t> recv(int sockfd, Bytes, int flags);
ErrorOr<size_t> recvmsg(int sockfd, struct msghdr*, int flags);
ErrorOr<size_t> recvfrom(int sockfd, Bytes, int flags, struct sockaddr*, socklen_t*);
ErrorOr<void> getsockopt(int sockfd, int level, int option, void* value, socklen_t* value_size);
CORE_API ErrorOr<void> setsockopt(int sockfd, int level, int option, void const* value, socklen_t value_size);
ErrorOr<void> getsockname(int sockfd, struct sockaddr*, socklen_t*);
ErrorOr<void> getpeername(int sockfd, struct sockaddr*, socklen_t*);
CORE_API ErrorOr<void> socketpair(int domain, int type, int protocol, int sv[2]);

CORE_API ErrorOr<void> access(StringView pathname, int mode, int flags = 0);
ErrorOr<ByteString> readlink(StringView pathname);
CORE_API ErrorOr<int> poll(Span<struct pollfd>, int timeout);

CORE_API ErrorOr<void> kill(pid_t, int signal);
#if !defined(AK_OS_WINDOWS)
CORE_API ErrorOr<void> chown(StringView pathname, uid_t uid, gid_t gid);
ErrorOr<pid_t> posix_spawn(StringView path, posix_spawn_file_actions_t const* file_actions, posix_spawnattr_t const* attr, char* const arguments[], char* const envp[]);
ErrorOr<pid_t> posix_spawnp(StringView path, posix_spawn_file_actions_t* const file_actions, posix_spawnattr_t* const attr, char* const arguments[], char* const envp[]);

struct WaitPidResult {
    pid_t pid;
    int status;
};
CORE_API ErrorOr<WaitPidResult> waitpid(pid_t waitee, int options = 0);
CORE_API ErrorOr<void> fchown(int fd, uid_t, gid_t);
#endif

ErrorOr<AddressInfoVector> getaddrinfo(char const* nodename, char const* servname, struct addrinfo const& hints);

CORE_API unsigned hardware_concurrency();
CORE_API u64 physical_memory_bytes();

CORE_API ErrorOr<ByteString> current_executable_path();

#if !defined(AK_OS_WINDOWS)
ErrorOr<rlimit> get_resource_limits(int resource);
CORE_API ErrorOr<void> set_resource_limits(int resource, rlim_t limit);
#endif

CORE_API int getpid();
CORE_API bool is_socket(int fd);
CORE_API ErrorOr<void> sleep_ms(u32 milliseconds);
CORE_API ErrorOr<void> set_close_on_exec(int fd, bool enabled);

CORE_API ErrorOr<size_t> transfer_file_through_socket(int source_fd, int target_fd, size_t source_offset, size_t source_length);

}
