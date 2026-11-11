/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibCore/CrashHandler.h>
#include <LibSandbox/ConnectBroker.h>
#include <LibSandbox/FileBroker.h>
#include <LibSandbox/Sandbox.h>
#include <LibSandbox/Seccomp.h>
#include <asm/termbits.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/netlink.h>
#include <linux/sched.h>
#include <linux/seccomp.h>
#include <linux/sockios.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <signal.h>
#include <stddef.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/ucontext.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef SYS_SECCOMP
#    define SYS_SECCOMP 1
#endif

namespace Sandbox {

namespace {

#if defined(__x86_64__)
static constexpr u32 audit_architecture = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
static constexpr u32 audit_architecture = AUDIT_ARCH_AARCH64;
#elif defined(__riscv) && __riscv_xlen == 64
static constexpr u32 audit_architecture = AUDIT_ARCH_RISCV64;
#else
#    error "Add 64-bit seccomp audit architecture for this Linux architecture"
#endif

static constexpr u16 emulate_fstatat_trap = 1;
static constexpr u16 broker_socket_trap = 2;
static constexpr u16 broker_connect_trap = 3;
static constexpr u16 file_broker_trap = 4;
static constexpr u16 signal_mask_trap = 5;
// A refusal for a system call that the file system filter sends to the broker. The policy can't refuse it with an errno,
// because the kernel takes the filter's trap over that, so it traps too, with the errno added to this.
static constexpr u16 refused_path_syscall_trap = 0x1000;
// A refusal that is logged before the errno added to this goes back, when sandbox debugging is on.
static constexpr u16 logged_refusal_trap = 0x2000;

static constexpr u32 thread_clone_required_flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD;
static constexpr u32 thread_clone_allowed_flags = thread_clone_required_flags
    | CLONE_SYSVSEM
    | CLONE_SETTLS
    | CLONE_PARENT_SETTID
    | CLONE_CHILD_CLEARTID
    | CLONE_DETACHED
    | CLONE_CHILD_SETTID;
static constexpr u32 fork_clone_flags = CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID | SIGCHLD;

// A refusal gives the system call an errno. With sandbox debugging on, it traps first, so the refusal can be logged.
static sock_filter refusal_action(int error)
{
    if (sandbox_debugging_is_enabled())
        return BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP | (logged_refusal_trap + error));
    return BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (static_cast<u32>(error) & SECCOMP_RET_DATA));
}

static constexpr unsigned read_only_open_flags = [] {
    unsigned flags = O_CLOEXEC | O_DIRECTORY | O_NOCTTY | O_NOFOLLOW | O_NONBLOCK;
#ifdef O_LARGEFILE
    flags |= O_LARGEFILE;
#endif
    return flags;
}();

#define SECCOMP_LOAD_SYSCALL_NR BPF_STMT(BPF_LD | BPF_W | BPF_ABS, static_cast<unsigned int>(offsetof(seccomp_data, nr)))
#define SECCOMP_LOAD_ARCHITECTURE BPF_STMT(BPF_LD | BPF_W | BPF_ABS, static_cast<unsigned int>(offsetof(seccomp_data, arch)))
#define SECCOMP_LOAD_ARGUMENT(index) BPF_STMT(BPF_LD | BPF_W | BPF_ABS, static_cast<unsigned int>(offsetof(seccomp_data, args[(index)])))
#define SECCOMP_ALLOW BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)
#define SECCOMP_TRAP BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP)
#define SECCOMP_ERRNO(error) refusal_action(error)
#define SECCOMP_ALLOW_NOTHING BPF_STMT(BPF_ALU | BPF_ADD | BPF_K, 0)
#define SECCOMP_APPEND_ALLOW_SYSCALL(policy, name)                               \
    do {                                                                         \
        (policy).append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_##name, 0, 1)); \
        (policy).append(SECCOMP_ALLOW);                                          \
    } while (0)
#define SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(policy, name) \
    IF_DEFINED_##name(SECCOMP_APPEND_ALLOW_SYSCALL(policy, name), (policy).append(SECCOMP_ALLOW_NOTHING))

// A system call that takes a path is allowed when Landlock decides which paths work. Without Landlock, the file system
// filter beneath every policy sends it to the broker or refuses it, which wins over the allow here.
#define SECCOMP_APPEND_PATH_SYSCALL_IF_DEFINED(policy, name) \
    IF_DEFINED_##name(do {                                                                        \
        (policy).append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_##name, 0, 1));                  \
        (policy).append((policy).path_syscall_action()); } while (0), (policy).append(SECCOMP_ALLOW_NOTHING))

#define IF_DEFINED_access(if_defined, if_not_defined) if_defined
#define IF_DEFINED_bind(if_defined, if_not_defined) if_defined
#define IF_DEFINED_brk(if_defined, if_not_defined) if_defined
#define IF_DEFINED_clock_getres(if_defined, if_not_defined) if_defined
#define IF_DEFINED_clock_gettime(if_defined, if_not_defined) if_defined
#define IF_DEFINED_clock_nanosleep(if_defined, if_not_defined) if_defined
#define IF_DEFINED_clone(if_defined, if_not_defined) if_defined
#define IF_DEFINED_clone3(if_defined, if_not_defined) if_defined
#define IF_DEFINED_close(if_defined, if_not_defined) if_defined
#define IF_DEFINED_connect(if_defined, if_not_defined) if_defined
#define IF_DEFINED_dup(if_defined, if_not_defined) if_defined
#define IF_DEFINED_dup3(if_defined, if_not_defined) if_defined
#define IF_DEFINED_epoll_create1(if_defined, if_not_defined) if_defined
#define IF_DEFINED_epoll_ctl(if_defined, if_not_defined) if_defined
#define IF_DEFINED_epoll_pwait(if_defined, if_not_defined) if_defined
#define IF_DEFINED_epoll_wait(if_defined, if_not_defined) if_defined
#define IF_DEFINED_eventfd2(if_defined, if_not_defined) if_defined
#define IF_DEFINED_faccessat(if_defined, if_not_defined) if_defined
#define IF_DEFINED_faccessat2(if_defined, if_not_defined) if_defined
#define IF_DEFINED_execve(if_defined, if_not_defined) if_defined
#define IF_DEFINED_execveat(if_defined, if_not_defined) if_defined
#define IF_DEFINED_exit(if_defined, if_not_defined) if_defined
#define IF_DEFINED_exit_group(if_defined, if_not_defined) if_defined
#define IF_DEFINED_fallocate(if_defined, if_not_defined) if_defined
#define IF_DEFINED_fchmod(if_defined, if_not_defined) if_defined
#define IF_DEFINED_fcntl(if_defined, if_not_defined) if_defined
#define IF_DEFINED_fcntl64(if_defined, if_not_defined) if_defined
#define IF_DEFINED_fdatasync(if_defined, if_not_defined) if_defined
#define IF_DEFINED_flock(if_defined, if_not_defined) if_defined
#define IF_DEFINED_fstat(if_defined, if_not_defined) if_defined
#define IF_DEFINED_fstatfs(if_defined, if_not_defined) if_defined
#define IF_DEFINED_fsync(if_defined, if_not_defined) if_defined
#define IF_DEFINED_ftruncate(if_defined, if_not_defined) if_defined
#define IF_DEFINED_futex(if_defined, if_not_defined) if_defined
#define IF_DEFINED_futex_time64(if_defined, if_not_defined) if_defined
#define IF_DEFINED_getdents64(if_defined, if_not_defined) if_defined
#define IF_DEFINED_getcpu(if_defined, if_not_defined) if_defined
#define IF_DEFINED_getegid(if_defined, if_not_defined) if_defined
#define IF_DEFINED_geteuid(if_defined, if_not_defined) if_defined
#define IF_DEFINED_getgid(if_defined, if_not_defined) if_defined
#define IF_DEFINED_getpeername(if_defined, if_not_defined) if_defined
#define IF_DEFINED_getpid(if_defined, if_not_defined) if_defined
#define IF_DEFINED_getrandom(if_defined, if_not_defined) if_defined
#define IF_DEFINED_getresgid(if_defined, if_not_defined) if_defined
#define IF_DEFINED_getresuid(if_defined, if_not_defined) if_defined
#define IF_DEFINED_getrlimit(if_defined, if_not_defined) if_defined
#define IF_DEFINED_getrusage(if_defined, if_not_defined) if_defined
#define IF_DEFINED_getsockname(if_defined, if_not_defined) if_defined
#define IF_DEFINED_getsockopt(if_defined, if_not_defined) if_defined
#define IF_DEFINED_gettid(if_defined, if_not_defined) if_defined
#define IF_DEFINED_gettimeofday(if_defined, if_not_defined) if_defined
#define IF_DEFINED_getuid(if_defined, if_not_defined) if_defined
#define IF_DEFINED_ioctl(if_defined, if_not_defined) if_defined
#define IF_DEFINED_link(if_defined, if_not_defined) if_defined
#define IF_DEFINED_linkat(if_defined, if_not_defined) if_defined
#define IF_DEFINED_lseek(if_defined, if_not_defined) if_defined
#define IF_DEFINED_madvise(if_defined, if_not_defined) if_defined
#define IF_DEFINED_membarrier(if_defined, if_not_defined) if_defined
#define IF_DEFINED_memfd_create(if_defined, if_not_defined) if_defined
#define IF_DEFINED_mkdir(if_defined, if_not_defined) if_defined
#define IF_DEFINED_mkdirat(if_defined, if_not_defined) if_defined
#define IF_DEFINED_mmap(if_defined, if_not_defined) if_defined
#define IF_DEFINED_mmap2(if_defined, if_not_defined) if_defined
#define IF_DEFINED_mprotect(if_defined, if_not_defined) if_defined
#define IF_DEFINED_mremap(if_defined, if_not_defined) if_defined
#define IF_DEFINED_munmap(if_defined, if_not_defined) if_defined
#define IF_DEFINED_nanosleep(if_defined, if_not_defined) if_defined
#define IF_DEFINED_newfstatat(if_defined, if_not_defined) if_defined
#define IF_DEFINED_open(if_defined, if_not_defined) if_defined
#define IF_DEFINED_openat(if_defined, if_not_defined) if_defined
#define IF_DEFINED_pipe2(if_defined, if_not_defined) if_defined
#define IF_DEFINED_poll(if_defined, if_not_defined) if_defined
#define IF_DEFINED_pread64(if_defined, if_not_defined) if_defined
#define IF_DEFINED_ppoll(if_defined, if_not_defined) if_defined
#define IF_DEFINED_prctl(if_defined, if_not_defined) if_defined
#define IF_DEFINED_prlimit64(if_defined, if_not_defined) if_defined
#define IF_DEFINED_pselect6(if_defined, if_not_defined) if_defined
#define IF_DEFINED_pwrite64(if_defined, if_not_defined) if_defined
#define IF_DEFINED_read(if_defined, if_not_defined) if_defined
#define IF_DEFINED_readlink(if_defined, if_not_defined) if_defined
#define IF_DEFINED_readlinkat(if_defined, if_not_defined) if_defined
#define IF_DEFINED_readv(if_defined, if_not_defined) if_defined
#define IF_DEFINED_rename(if_defined, if_not_defined) if_defined
#define IF_DEFINED_renameat(if_defined, if_not_defined) if_defined
#define IF_DEFINED_renameat2(if_defined, if_not_defined) if_defined
#define IF_DEFINED_recvfrom(if_defined, if_not_defined) if_defined
#define IF_DEFINED_recvmmsg(if_defined, if_not_defined) if_defined
#define IF_DEFINED_recvmsg(if_defined, if_not_defined) if_defined
#define IF_DEFINED_restart_syscall(if_defined, if_not_defined) if_defined
#define IF_DEFINED_rmdir(if_defined, if_not_defined) if_defined
#define IF_DEFINED_rseq(if_defined, if_not_defined) if_defined
#define IF_DEFINED_rt_sigaction(if_defined, if_not_defined) if_defined
#define IF_DEFINED_rt_sigprocmask(if_defined, if_not_defined) if_defined
#define IF_DEFINED_rt_sigreturn(if_defined, if_not_defined) if_defined
#define IF_DEFINED_sched_getaffinity(if_defined, if_not_defined) if_defined
#define IF_DEFINED_sched_yield(if_defined, if_not_defined) if_defined
#define IF_DEFINED_sendfile(if_defined, if_not_defined) if_defined
#define IF_DEFINED_sendmmsg(if_defined, if_not_defined) if_defined
#define IF_DEFINED_sendmsg(if_defined, if_not_defined) if_defined
#define IF_DEFINED_sendto(if_defined, if_not_defined) if_defined
#define IF_DEFINED_set_robust_list(if_defined, if_not_defined) if_defined
#define IF_DEFINED_set_tid_address(if_defined, if_not_defined) if_defined
#define IF_DEFINED_setsockopt(if_defined, if_not_defined) if_defined
#define IF_DEFINED_sigaltstack(if_defined, if_not_defined) if_defined
#define IF_DEFINED_shutdown(if_defined, if_not_defined) if_defined
#define IF_DEFINED_socket(if_defined, if_not_defined) if_defined
#define IF_DEFINED_socketpair(if_defined, if_not_defined) if_defined
#define IF_DEFINED_stat(if_defined, if_not_defined) if_defined
#define IF_DEFINED_statfs(if_defined, if_not_defined) if_defined
#define IF_DEFINED_statx(if_defined, if_not_defined) if_defined
#define IF_DEFINED_symlink(if_defined, if_not_defined) if_defined
#define IF_DEFINED_symlinkat(if_defined, if_not_defined) if_defined
#define IF_DEFINED_sysinfo(if_defined, if_not_defined) if_defined
#define IF_DEFINED_tgkill(if_defined, if_not_defined) if_defined
#define IF_DEFINED_unlink(if_defined, if_not_defined) if_defined
#define IF_DEFINED_unlinkat(if_defined, if_not_defined) if_defined
#define IF_DEFINED_umask(if_defined, if_not_defined) if_defined
#define IF_DEFINED_uname(if_defined, if_not_defined) if_defined
#define IF_DEFINED_wait4(if_defined, if_not_defined) if_defined
#define IF_DEFINED_waitid(if_defined, if_not_defined) if_defined
#define IF_DEFINED_write(if_defined, if_not_defined) if_defined
#define IF_DEFINED_writev(if_defined, if_not_defined) if_defined

#ifndef __NR_access
#    undef IF_DEFINED_access
#    define IF_DEFINED_access(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_bind
#    undef IF_DEFINED_bind
#    define IF_DEFINED_bind(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_connect
#    undef IF_DEFINED_connect
#    define IF_DEFINED_connect(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_clock_getres
#    undef IF_DEFINED_clock_getres
#    define IF_DEFINED_clock_getres(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_clock_gettime
#    undef IF_DEFINED_clock_gettime
#    define IF_DEFINED_clock_gettime(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_clock_nanosleep
#    undef IF_DEFINED_clock_nanosleep
#    define IF_DEFINED_clock_nanosleep(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_clone3
#    undef IF_DEFINED_clone3
#    define IF_DEFINED_clone3(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_dup3
#    undef IF_DEFINED_dup3
#    define IF_DEFINED_dup3(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_epoll_create1
#    undef IF_DEFINED_epoll_create1
#    define IF_DEFINED_epoll_create1(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_epoll_ctl
#    undef IF_DEFINED_epoll_ctl
#    define IF_DEFINED_epoll_ctl(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_epoll_pwait
#    undef IF_DEFINED_epoll_pwait
#    define IF_DEFINED_epoll_pwait(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_epoll_wait
#    undef IF_DEFINED_epoll_wait
#    define IF_DEFINED_epoll_wait(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_eventfd2
#    undef IF_DEFINED_eventfd2
#    define IF_DEFINED_eventfd2(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_faccessat
#    undef IF_DEFINED_faccessat
#    define IF_DEFINED_faccessat(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_faccessat2
#    undef IF_DEFINED_faccessat2
#    define IF_DEFINED_faccessat2(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_execve
#    undef IF_DEFINED_execve
#    define IF_DEFINED_execve(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_execveat
#    undef IF_DEFINED_execveat
#    define IF_DEFINED_execveat(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_fallocate
#    undef IF_DEFINED_fallocate
#    define IF_DEFINED_fallocate(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_fcntl64
#    undef IF_DEFINED_fcntl64
#    define IF_DEFINED_fcntl64(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_fdatasync
#    undef IF_DEFINED_fdatasync
#    define IF_DEFINED_fdatasync(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_flock
#    undef IF_DEFINED_flock
#    define IF_DEFINED_flock(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_fstat
#    undef IF_DEFINED_fstat
#    define IF_DEFINED_fstat(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_fstatfs
#    undef IF_DEFINED_fstatfs
#    define IF_DEFINED_fstatfs(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_fsync
#    undef IF_DEFINED_fsync
#    define IF_DEFINED_fsync(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_futex_time64
#    undef IF_DEFINED_futex_time64
#    define IF_DEFINED_futex_time64(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_getdents64
#    undef IF_DEFINED_getdents64
#    define IF_DEFINED_getdents64(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_link
#    undef IF_DEFINED_link
#    define IF_DEFINED_link(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_linkat
#    undef IF_DEFINED_linkat
#    define IF_DEFINED_linkat(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_getcpu
#    undef IF_DEFINED_getcpu
#    define IF_DEFINED_getcpu(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_getrandom
#    undef IF_DEFINED_getrandom
#    define IF_DEFINED_getrandom(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_getresgid
#    undef IF_DEFINED_getresgid
#    define IF_DEFINED_getresgid(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_getresuid
#    undef IF_DEFINED_getresuid
#    define IF_DEFINED_getresuid(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_membarrier
#    undef IF_DEFINED_membarrier
#    define IF_DEFINED_membarrier(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_memfd_create
#    undef IF_DEFINED_memfd_create
#    define IF_DEFINED_memfd_create(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_mkdir
#    undef IF_DEFINED_mkdir
#    define IF_DEFINED_mkdir(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_mkdirat
#    undef IF_DEFINED_mkdirat
#    define IF_DEFINED_mkdirat(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_mmap2
#    undef IF_DEFINED_mmap2
#    define IF_DEFINED_mmap2(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_pipe2
#    undef IF_DEFINED_pipe2
#    define IF_DEFINED_pipe2(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_newfstatat
#    undef IF_DEFINED_newfstatat
#    define IF_DEFINED_newfstatat(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_open
#    undef IF_DEFINED_open
#    define IF_DEFINED_open(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_openat
#    undef IF_DEFINED_openat
#    define IF_DEFINED_openat(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_poll
#    undef IF_DEFINED_poll
#    define IF_DEFINED_poll(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_pread64
#    undef IF_DEFINED_pread64
#    define IF_DEFINED_pread64(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_readlink
#    undef IF_DEFINED_readlink
#    define IF_DEFINED_readlink(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_readlinkat
#    undef IF_DEFINED_readlinkat
#    define IF_DEFINED_readlinkat(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_rename
#    undef IF_DEFINED_rename
#    define IF_DEFINED_rename(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_renameat
#    undef IF_DEFINED_renameat
#    define IF_DEFINED_renameat(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_renameat2
#    undef IF_DEFINED_renameat2
#    define IF_DEFINED_renameat2(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_rmdir
#    undef IF_DEFINED_rmdir
#    define IF_DEFINED_rmdir(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_ppoll
#    undef IF_DEFINED_ppoll
#    define IF_DEFINED_ppoll(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_prlimit64
#    undef IF_DEFINED_prlimit64
#    define IF_DEFINED_prlimit64(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_pselect6
#    undef IF_DEFINED_pselect6
#    define IF_DEFINED_pselect6(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_pwrite64
#    undef IF_DEFINED_pwrite64
#    define IF_DEFINED_pwrite64(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_recvmmsg
#    undef IF_DEFINED_recvmmsg
#    define IF_DEFINED_recvmmsg(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_restart_syscall
#    undef IF_DEFINED_restart_syscall
#    define IF_DEFINED_restart_syscall(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_rseq
#    undef IF_DEFINED_rseq
#    define IF_DEFINED_rseq(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_sendmmsg
#    undef IF_DEFINED_sendmmsg
#    define IF_DEFINED_sendmmsg(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_sendfile
#    undef IF_DEFINED_sendfile
#    define IF_DEFINED_sendfile(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_sigaltstack
#    undef IF_DEFINED_sigaltstack
#    define IF_DEFINED_sigaltstack(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_socket
#    undef IF_DEFINED_socket
#    define IF_DEFINED_socket(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_shutdown
#    undef IF_DEFINED_shutdown
#    define IF_DEFINED_shutdown(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_stat
#    undef IF_DEFINED_stat
#    define IF_DEFINED_stat(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_statfs
#    undef IF_DEFINED_statfs
#    define IF_DEFINED_statfs(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_statx
#    undef IF_DEFINED_statx
#    define IF_DEFINED_statx(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_symlink
#    undef IF_DEFINED_symlink
#    define IF_DEFINED_symlink(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_symlinkat
#    undef IF_DEFINED_symlinkat
#    define IF_DEFINED_symlinkat(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_unlink
#    undef IF_DEFINED_unlink
#    define IF_DEFINED_unlink(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_unlinkat
#    undef IF_DEFINED_unlinkat
#    define IF_DEFINED_unlinkat(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_sysinfo
#    undef IF_DEFINED_sysinfo
#    define IF_DEFINED_sysinfo(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_umask
#    undef IF_DEFINED_umask
#    define IF_DEFINED_umask(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_wait4
#    undef IF_DEFINED_wait4
#    define IF_DEFINED_wait4(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_waitid
#    undef IF_DEFINED_waitid
#    define IF_DEFINED_waitid(if_defined, if_not_defined) if_not_defined
#endif

static size_t safe_string_length(char const* string)
{
    size_t length = 0;
    while (string[length] != '\0')
        ++length;
    return length;
}

// Builds one line of a log message, so that it reaches stderr in one write and lines from two threads never mix.
class LogLine {
public:
    void append(char const* string) { append_bytes(string, safe_string_length(string)); }

    void append_unsigned(u64 value)
    {
        char buffer[20];
        size_t index = sizeof(buffer);
        do {
            buffer[--index] = static_cast<char>('0' + (value % 10));
            value /= 10;
        } while (value > 0);
        append_bytes(buffer + index, sizeof(buffer) - index);
    }

    void append_hex(u64 value)
    {
        constexpr char const* digits = "0123456789abcdef";
        char buffer[16];
        size_t index = sizeof(buffer);
        do {
            buffer[--index] = digits[value & 0xf];
            value >>= 4;
        } while (value > 0);
        append("0x");
        append_bytes(buffer + index, sizeof(buffer) - index);
    }

    void write_to_stderr()
    {
        append("\n");
        char const* data = m_data;
        size_t length = m_length;
        while (length > 0) {
            auto written = write(STDERR_FILENO, data, length);
            if (written <= 0)
                return;
            data += written;
            length -= static_cast<size_t>(written);
        }
    }

private:
    void append_bytes(char const* bytes, size_t length)
    {
        // The last byte is kept for the newline.
        auto room = sizeof(m_data) - 1 - m_length;
        if (length > room)
            length = room;
        __builtin_memcpy(m_data + m_length, bytes, length);
        m_length += length;
    }

    char m_data[512];
    size_t m_length { 0 };
};

#define CASE_SYSCALL_NAME(name) \
    case __NR_##name:           \
        return #name

static char const* syscall_name(long syscall_number)
{
    switch (syscall_number) {
#ifdef __NR_accept
        CASE_SYSCALL_NAME(accept);
#endif
#ifdef __NR_accept4
        CASE_SYSCALL_NAME(accept4);
#endif
#ifdef __NR_access
        CASE_SYSCALL_NAME(access);
#endif
#ifdef __NR_acct
        CASE_SYSCALL_NAME(acct);
#endif
#ifdef __NR_arch_prctl
        CASE_SYSCALL_NAME(arch_prctl);
#endif
#ifdef __NR_bind
        CASE_SYSCALL_NAME(bind);
#endif
#ifdef __NR_brk
        CASE_SYSCALL_NAME(brk);
#endif
#ifdef __NR_chdir
        CASE_SYSCALL_NAME(chdir);
#endif
#ifdef __NR_chmod
        CASE_SYSCALL_NAME(chmod);
#endif
#ifdef __NR_chown
        CASE_SYSCALL_NAME(chown);
#endif
#ifdef __NR_chroot
        CASE_SYSCALL_NAME(chroot);
#endif
#ifdef __NR_clock_getres
        CASE_SYSCALL_NAME(clock_getres);
#endif
#ifdef __NR_clock_gettime
        CASE_SYSCALL_NAME(clock_gettime);
#endif
#ifdef __NR_clock_nanosleep
        CASE_SYSCALL_NAME(clock_nanosleep);
#endif
#ifdef __NR_clone
        CASE_SYSCALL_NAME(clone);
#endif
#ifdef __NR_clone3
        CASE_SYSCALL_NAME(clone3);
#endif
#ifdef __NR_close
        CASE_SYSCALL_NAME(close);
#endif
#ifdef __NR_close_range
        CASE_SYSCALL_NAME(close_range);
#endif
#ifdef __NR_connect
        CASE_SYSCALL_NAME(connect);
#endif
#ifdef __NR_creat
        CASE_SYSCALL_NAME(creat);
#endif
#ifdef __NR_dup
        CASE_SYSCALL_NAME(dup);
#endif
#ifdef __NR_dup2
        CASE_SYSCALL_NAME(dup2);
#endif
#ifdef __NR_dup3
        CASE_SYSCALL_NAME(dup3);
#endif
#ifdef __NR_epoll_create1
        CASE_SYSCALL_NAME(epoll_create1);
#endif
#ifdef __NR_epoll_ctl
        CASE_SYSCALL_NAME(epoll_ctl);
#endif
#ifdef __NR_epoll_pwait
        CASE_SYSCALL_NAME(epoll_pwait);
#endif
#ifdef __NR_epoll_wait
        CASE_SYSCALL_NAME(epoll_wait);
#endif
#ifdef __NR_eventfd2
        CASE_SYSCALL_NAME(eventfd2);
#endif
#ifdef __NR_execve
        CASE_SYSCALL_NAME(execve);
#endif
#ifdef __NR_execveat
        CASE_SYSCALL_NAME(execveat);
#endif
#ifdef __NR_exit
        CASE_SYSCALL_NAME(exit);
#endif
#ifdef __NR_exit_group
        CASE_SYSCALL_NAME(exit_group);
#endif
#ifdef __NR_faccessat
        CASE_SYSCALL_NAME(faccessat);
#endif
#ifdef __NR_faccessat2
        CASE_SYSCALL_NAME(faccessat2);
#endif
#ifdef __NR_fallocate
        CASE_SYSCALL_NAME(fallocate);
#endif
#ifdef __NR_fanotify_mark
        CASE_SYSCALL_NAME(fanotify_mark);
#endif
#ifdef __NR_fchdir
        CASE_SYSCALL_NAME(fchdir);
#endif
#ifdef __NR_fchmod
        CASE_SYSCALL_NAME(fchmod);
#endif
#ifdef __NR_fchmodat
        CASE_SYSCALL_NAME(fchmodat);
#endif
#ifdef __NR_fchmodat2
        CASE_SYSCALL_NAME(fchmodat2);
#endif
#ifdef __NR_fchownat
        CASE_SYSCALL_NAME(fchownat);
#endif
#ifdef __NR_fcntl
        CASE_SYSCALL_NAME(fcntl);
#endif
#ifdef __NR_fcntl64
        CASE_SYSCALL_NAME(fcntl64);
#endif
#ifdef __NR_fdatasync
        CASE_SYSCALL_NAME(fdatasync);
#endif
#ifdef __NR_flock
        CASE_SYSCALL_NAME(flock);
#endif
#ifdef __NR_fork
        CASE_SYSCALL_NAME(fork);
#endif
#ifdef __NR_fstat
        CASE_SYSCALL_NAME(fstat);
#endif
#ifdef __NR_fstatfs
        CASE_SYSCALL_NAME(fstatfs);
#endif
#ifdef __NR_fsync
        CASE_SYSCALL_NAME(fsync);
#endif
#ifdef __NR_ftruncate
        CASE_SYSCALL_NAME(ftruncate);
#endif
#ifdef __NR_futex
        CASE_SYSCALL_NAME(futex);
#endif
#ifdef __NR_futex_time64
        CASE_SYSCALL_NAME(futex_time64);
#endif
#ifdef __NR_futimesat
        CASE_SYSCALL_NAME(futimesat);
#endif
#ifdef __NR_getcpu
        CASE_SYSCALL_NAME(getcpu);
#endif
#ifdef __NR_getcwd
        CASE_SYSCALL_NAME(getcwd);
#endif
#ifdef __NR_getdents64
        CASE_SYSCALL_NAME(getdents64);
#endif
#ifdef __NR_getegid
        CASE_SYSCALL_NAME(getegid);
#endif
#ifdef __NR_geteuid
        CASE_SYSCALL_NAME(geteuid);
#endif
#ifdef __NR_getgid
        CASE_SYSCALL_NAME(getgid);
#endif
#ifdef __NR_getpeername
        CASE_SYSCALL_NAME(getpeername);
#endif
#ifdef __NR_getpid
        CASE_SYSCALL_NAME(getpid);
#endif
#ifdef __NR_getppid
        CASE_SYSCALL_NAME(getppid);
#endif
#ifdef __NR_getpriority
        CASE_SYSCALL_NAME(getpriority);
#endif
#ifdef __NR_getrandom
        CASE_SYSCALL_NAME(getrandom);
#endif
#ifdef __NR_getresgid
        CASE_SYSCALL_NAME(getresgid);
#endif
#ifdef __NR_getresuid
        CASE_SYSCALL_NAME(getresuid);
#endif
#ifdef __NR_getrlimit
        CASE_SYSCALL_NAME(getrlimit);
#endif
#ifdef __NR_getrusage
        CASE_SYSCALL_NAME(getrusage);
#endif
#ifdef __NR_getsockname
        CASE_SYSCALL_NAME(getsockname);
#endif
#ifdef __NR_getsockopt
        CASE_SYSCALL_NAME(getsockopt);
#endif
#ifdef __NR_gettid
        CASE_SYSCALL_NAME(gettid);
#endif
#ifdef __NR_gettimeofday
        CASE_SYSCALL_NAME(gettimeofday);
#endif
#ifdef __NR_getuid
        CASE_SYSCALL_NAME(getuid);
#endif
#ifdef __NR_getxattr
        CASE_SYSCALL_NAME(getxattr);
#endif
#ifdef __NR_inotify_add_watch
        CASE_SYSCALL_NAME(inotify_add_watch);
#endif
#ifdef __NR_ioctl
        CASE_SYSCALL_NAME(ioctl);
#endif
#ifdef __NR_kcmp
        CASE_SYSCALL_NAME(kcmp);
#endif
#ifdef __NR_lchown
        CASE_SYSCALL_NAME(lchown);
#endif
#ifdef __NR_lgetxattr
        CASE_SYSCALL_NAME(lgetxattr);
#endif
#ifdef __NR_link
        CASE_SYSCALL_NAME(link);
#endif
#ifdef __NR_linkat
        CASE_SYSCALL_NAME(linkat);
#endif
#ifdef __NR_listxattr
        CASE_SYSCALL_NAME(listxattr);
#endif
#ifdef __NR_llistxattr
        CASE_SYSCALL_NAME(llistxattr);
#endif
#ifdef __NR_lremovexattr
        CASE_SYSCALL_NAME(lremovexattr);
#endif
#ifdef __NR_lseek
        CASE_SYSCALL_NAME(lseek);
#endif
#ifdef __NR_lsetxattr
        CASE_SYSCALL_NAME(lsetxattr);
#endif
#ifdef __NR_lstat
        CASE_SYSCALL_NAME(lstat);
#endif
#ifdef __NR_madvise
        CASE_SYSCALL_NAME(madvise);
#endif
#ifdef __NR_membarrier
        CASE_SYSCALL_NAME(membarrier);
#endif
#ifdef __NR_memfd_create
        CASE_SYSCALL_NAME(memfd_create);
#endif
#ifdef __NR_mkdir
        CASE_SYSCALL_NAME(mkdir);
#endif
#ifdef __NR_mkdirat
        CASE_SYSCALL_NAME(mkdirat);
#endif
#ifdef __NR_mknod
        CASE_SYSCALL_NAME(mknod);
#endif
#ifdef __NR_mknodat
        CASE_SYSCALL_NAME(mknodat);
#endif
#ifdef __NR_mmap
        CASE_SYSCALL_NAME(mmap);
#endif
#ifdef __NR_mmap2
        CASE_SYSCALL_NAME(mmap2);
#endif
#ifdef __NR_mount
        CASE_SYSCALL_NAME(mount);
#endif
#ifdef __NR_mprotect
        CASE_SYSCALL_NAME(mprotect);
#endif
#ifdef __NR_mremap
        CASE_SYSCALL_NAME(mremap);
#endif
#ifdef __NR_munmap
        CASE_SYSCALL_NAME(munmap);
#endif
#ifdef __NR_name_to_handle_at
        CASE_SYSCALL_NAME(name_to_handle_at);
#endif
#ifdef __NR_nanosleep
        CASE_SYSCALL_NAME(nanosleep);
#endif
#ifdef __NR_newfstatat
        CASE_SYSCALL_NAME(newfstatat);
#endif
#ifdef __NR_open
        CASE_SYSCALL_NAME(open);
#endif
#ifdef __NR_open_by_handle_at
        CASE_SYSCALL_NAME(open_by_handle_at);
#endif
#ifdef __NR_openat
        CASE_SYSCALL_NAME(openat);
#endif
#ifdef __NR_pipe2
        CASE_SYSCALL_NAME(pipe2);
#endif
#ifdef __NR_pivot_root
        CASE_SYSCALL_NAME(pivot_root);
#endif
#ifdef __NR_pkey_mprotect
        CASE_SYSCALL_NAME(pkey_mprotect);
#endif
#ifdef __NR_poll
        CASE_SYSCALL_NAME(poll);
#endif
#ifdef __NR_ppoll
        CASE_SYSCALL_NAME(ppoll);
#endif
#ifdef __NR_prctl
        CASE_SYSCALL_NAME(prctl);
#endif
#ifdef __NR_pread64
        CASE_SYSCALL_NAME(pread64);
#endif
#ifdef __NR_prlimit64
        CASE_SYSCALL_NAME(prlimit64);
#endif
#ifdef __NR_process_vm_readv
        CASE_SYSCALL_NAME(process_vm_readv);
#endif
#ifdef __NR_process_vm_writev
        CASE_SYSCALL_NAME(process_vm_writev);
#endif
#ifdef __NR_pselect6
        CASE_SYSCALL_NAME(pselect6);
#endif
#ifdef __NR_ptrace
        CASE_SYSCALL_NAME(ptrace);
#endif
#ifdef __NR_pwrite64
        CASE_SYSCALL_NAME(pwrite64);
#endif
#ifdef __NR_quotactl
        CASE_SYSCALL_NAME(quotactl);
#endif
#ifdef __NR_read
        CASE_SYSCALL_NAME(read);
#endif
#ifdef __NR_readlink
        CASE_SYSCALL_NAME(readlink);
#endif
#ifdef __NR_readlinkat
        CASE_SYSCALL_NAME(readlinkat);
#endif
#ifdef __NR_readv
        CASE_SYSCALL_NAME(readv);
#endif
#ifdef __NR_recvfrom
        CASE_SYSCALL_NAME(recvfrom);
#endif
#ifdef __NR_recvmmsg
        CASE_SYSCALL_NAME(recvmmsg);
#endif
#ifdef __NR_recvmsg
        CASE_SYSCALL_NAME(recvmsg);
#endif
#ifdef __NR_removexattr
        CASE_SYSCALL_NAME(removexattr);
#endif
#ifdef __NR_rename
        CASE_SYSCALL_NAME(rename);
#endif
#ifdef __NR_renameat
        CASE_SYSCALL_NAME(renameat);
#endif
#ifdef __NR_renameat2
        CASE_SYSCALL_NAME(renameat2);
#endif
#ifdef __NR_restart_syscall
        CASE_SYSCALL_NAME(restart_syscall);
#endif
#ifdef __NR_rmdir
        CASE_SYSCALL_NAME(rmdir);
#endif
#ifdef __NR_rseq
        CASE_SYSCALL_NAME(rseq);
#endif
#ifdef __NR_rt_sigaction
        CASE_SYSCALL_NAME(rt_sigaction);
#endif
#ifdef __NR_rt_sigprocmask
        CASE_SYSCALL_NAME(rt_sigprocmask);
#endif
#ifdef __NR_rt_sigreturn
        CASE_SYSCALL_NAME(rt_sigreturn);
#endif
#ifdef __NR_sched_get_priority_max
        CASE_SYSCALL_NAME(sched_get_priority_max);
#endif
#ifdef __NR_sched_get_priority_min
        CASE_SYSCALL_NAME(sched_get_priority_min);
#endif
#ifdef __NR_sched_getaffinity
        CASE_SYSCALL_NAME(sched_getaffinity);
#endif
#ifdef __NR_sched_getscheduler
        CASE_SYSCALL_NAME(sched_getscheduler);
#endif
#ifdef __NR_sched_setaffinity
        CASE_SYSCALL_NAME(sched_setaffinity);
#endif
#ifdef __NR_sched_setparam
        CASE_SYSCALL_NAME(sched_setparam);
#endif
#ifdef __NR_sched_setscheduler
        CASE_SYSCALL_NAME(sched_setscheduler);
#endif
#ifdef __NR_sched_yield
        CASE_SYSCALL_NAME(sched_yield);
#endif
#ifdef __NR_seccomp
        CASE_SYSCALL_NAME(seccomp);
#endif
#ifdef __NR_sendfile
        CASE_SYSCALL_NAME(sendfile);
#endif
#ifdef __NR_sendmmsg
        CASE_SYSCALL_NAME(sendmmsg);
#endif
#ifdef __NR_sendmsg
        CASE_SYSCALL_NAME(sendmsg);
#endif
#ifdef __NR_sendto
        CASE_SYSCALL_NAME(sendto);
#endif
#ifdef __NR_set_robust_list
        CASE_SYSCALL_NAME(set_robust_list);
#endif
#ifdef __NR_set_tid_address
        CASE_SYSCALL_NAME(set_tid_address);
#endif
#ifdef __NR_setpriority
        CASE_SYSCALL_NAME(setpriority);
#endif
#ifdef __NR_setsockopt
        CASE_SYSCALL_NAME(setsockopt);
#endif
#ifdef __NR_setxattr
        CASE_SYSCALL_NAME(setxattr);
#endif
#ifdef __NR_shutdown
        CASE_SYSCALL_NAME(shutdown);
#endif
#ifdef __NR_sigaltstack
        CASE_SYSCALL_NAME(sigaltstack);
#endif
#ifdef __NR_socket
        CASE_SYSCALL_NAME(socket);
#endif
#ifdef __NR_socketpair
        CASE_SYSCALL_NAME(socketpair);
#endif
#ifdef __NR_stat
        CASE_SYSCALL_NAME(stat);
#endif
#ifdef __NR_statfs
        CASE_SYSCALL_NAME(statfs);
#endif
#ifdef __NR_statx
        CASE_SYSCALL_NAME(statx);
#endif
#ifdef __NR_swapoff
        CASE_SYSCALL_NAME(swapoff);
#endif
#ifdef __NR_swapon
        CASE_SYSCALL_NAME(swapon);
#endif
#ifdef __NR_symlink
        CASE_SYSCALL_NAME(symlink);
#endif
#ifdef __NR_symlinkat
        CASE_SYSCALL_NAME(symlinkat);
#endif
#ifdef __NR_sysinfo
        CASE_SYSCALL_NAME(sysinfo);
#endif
#ifdef __NR_tgkill
        CASE_SYSCALL_NAME(tgkill);
#endif
#ifdef __NR_truncate
        CASE_SYSCALL_NAME(truncate);
#endif
#ifdef __NR_umask
        CASE_SYSCALL_NAME(umask);
#endif
#ifdef __NR_umount2
        CASE_SYSCALL_NAME(umount2);
#endif
#ifdef __NR_uname
        CASE_SYSCALL_NAME(uname);
#endif
#ifdef __NR_unlink
        CASE_SYSCALL_NAME(unlink);
#endif
#ifdef __NR_unlinkat
        CASE_SYSCALL_NAME(unlinkat);
#endif
#ifdef __NR_uselib
        CASE_SYSCALL_NAME(uselib);
#endif
#ifdef __NR_utime
        CASE_SYSCALL_NAME(utime);
#endif
#ifdef __NR_utimensat
        CASE_SYSCALL_NAME(utimensat);
#endif
#ifdef __NR_utimes
        CASE_SYSCALL_NAME(utimes);
#endif
#ifdef __NR_vfork
        CASE_SYSCALL_NAME(vfork);
#endif
#ifdef __NR_wait4
        CASE_SYSCALL_NAME(wait4);
#endif
#ifdef __NR_waitid
        CASE_SYSCALL_NAME(waitid);
#endif
#ifdef __NR_write
        CASE_SYSCALL_NAME(write);
#endif
#ifdef __NR_writev
        CASE_SYSCALL_NAME(writev);
#endif
    default:
        return "unknown";
    }
}

#undef CASE_SYSCALL_NAME

static char s_process_name[16] = "unknown";
// Read when the handler is installed, since the handler itself can't look at the environment.
static bool s_logs_refusals = false;

// The arguments are copied out before anything is written back, because the result register and the
// first argument register are the same one on some architectures.
struct SyscallRegisters {
    FlatPtr arguments[6];
    FlatPtr* result;
};

static SyscallRegisters syscall_registers(void* context)
{
    auto& machine_context = static_cast<ucontext_t*>(context)->uc_mcontext;
#if defined(__x86_64__)
    static_assert(sizeof(machine_context.gregs[0]) == sizeof(FlatPtr));
    return {
        {
            static_cast<FlatPtr>(machine_context.gregs[REG_RDI]),
            static_cast<FlatPtr>(machine_context.gregs[REG_RSI]),
            static_cast<FlatPtr>(machine_context.gregs[REG_RDX]),
            static_cast<FlatPtr>(machine_context.gregs[REG_R10]),
            static_cast<FlatPtr>(machine_context.gregs[REG_R8]),
            static_cast<FlatPtr>(machine_context.gregs[REG_R9]),
        },
        reinterpret_cast<FlatPtr*>(&machine_context.gregs[REG_RAX]),
    };
#elif defined(__aarch64__)
    static_assert(sizeof(machine_context.regs[0]) == sizeof(FlatPtr));
    return {
        {
            static_cast<FlatPtr>(machine_context.regs[0]),
            static_cast<FlatPtr>(machine_context.regs[1]),
            static_cast<FlatPtr>(machine_context.regs[2]),
            static_cast<FlatPtr>(machine_context.regs[3]),
            static_cast<FlatPtr>(machine_context.regs[4]),
            static_cast<FlatPtr>(machine_context.regs[5]),
        },
        reinterpret_cast<FlatPtr*>(&machine_context.regs[0]),
    };
#elif defined(__riscv) && __riscv_xlen == 64
    static_assert(sizeof(machine_context.__gregs[0]) == sizeof(FlatPtr));
    return {
        {
            static_cast<FlatPtr>(machine_context.__gregs[REG_A0]),
            static_cast<FlatPtr>(machine_context.__gregs[REG_A0 + 1]),
            static_cast<FlatPtr>(machine_context.__gregs[REG_A0 + 2]),
            static_cast<FlatPtr>(machine_context.__gregs[REG_A0 + 3]),
            static_cast<FlatPtr>(machine_context.__gregs[REG_A0 + 4]),
            static_cast<FlatPtr>(machine_context.__gregs[REG_A0 + 5]),
        },
        reinterpret_cast<FlatPtr*>(&machine_context.__gregs[REG_A0]),
    };
#endif
}

// A syscall reports failure as the negated error number, so this is how the kernel would have
// written it had the syscall actually run.
static void set_syscall_result(SyscallRegisters const& registers, i64 result)
{
    *registers.result = static_cast<FlatPtr>(result);
}

#if defined(__NR_newfstatat) && defined(__NR_fstat)
static void emulate_fstatat(void* context)
{
    auto registers = syscall_registers(context);
    auto fd = static_cast<int>(registers.arguments[0]);
    auto* path = reinterpret_cast<char const*>(registers.arguments[1]);
    auto* buffer = reinterpret_cast<void*>(registers.arguments[2]);
    auto flags = static_cast<int>(registers.arguments[3]);

    // NB: glibc can implement fstat() using newfstatat(). Follow Chromium's SIGSYSFstatatHandler
    //     and Firefox's StatAtTrap by translating empty-path descriptor queries back to fstat().
    //     Seccomp cannot inspect pathname strings, and AT_EMPTY_PATH also permits nonempty paths.
    //     https://chromium.googlesource.com/chromium/src/+/main/sandbox/linux/seccomp-bpf-helpers/sigsys_handlers.cc
    //     https://searchfox.org/firefox-main/source/security/sandbox/linux/SandboxFilter.cpp
    set_syscall_result(registers, -EACCES);
    if (flags != AT_EMPTY_PATH || (path && path[0] != '\0'))
        return;

    auto saved_errno = errno;
    auto syscall_result = syscall(__NR_fstat, fd, buffer);
    set_syscall_result(registers, syscall_result < 0 ? -errno : syscall_result);
    errno = saved_errno;
}
#endif

static int s_connect_broker_fd = -1;

// Runs inside the SIGSYS handler, so it may only touch descriptors and syscalls. Each call carries
// its own reply socket, which is what keeps it safe to run on several threads at once. Returns the
// size of the answer or a negative errno, and stores the descriptor the answer carried, if any.
static i64 exchange_with_broker(int broker_fd, void const* request, size_t request_size, int const* extra_fds, size_t extra_fd_count, void* response, size_t response_size, int& answered_fd)
{
    answered_fd = -1;
    VERIFY(extra_fd_count <= 2);

    int reply_fds[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, reply_fds) < 0)
        return -errno;

    int passed_fds[3] = { reply_fds[1], -1, -1 };
    for (size_t i = 0; i < extra_fd_count; ++i)
        passed_fds[i + 1] = extra_fds[i];
    size_t passed_fd_count = 1 + extra_fd_count;

    iovec request_io {
        .iov_base = const_cast<void*>(request),
        .iov_len = request_size,
    };

    union {
        cmsghdr header;
        char space[CMSG_SPACE(3 * sizeof(int))];
    } request_control {};

    msghdr request_message {
        .msg_name = nullptr,
        .msg_namelen = 0,
        .msg_iov = &request_io,
        .msg_iovlen = 1,
        .msg_control = &request_control,
        .msg_controllen = CMSG_SPACE(passed_fd_count * sizeof(int)),
        .msg_flags = 0,
    };

    auto* request_header = CMSG_FIRSTHDR(&request_message);
    request_header->cmsg_level = SOL_SOCKET;
    request_header->cmsg_type = SCM_RIGHTS;
    request_header->cmsg_len = CMSG_LEN(passed_fd_count * sizeof(int));
    __builtin_memcpy(CMSG_DATA(request_header), passed_fds, passed_fd_count * sizeof(int));

    ssize_t sent = 0;
    do {
        sent = sendmsg(broker_fd, &request_message, MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);

    close(reply_fds[1]);

    if (sent < 0) {
        auto saved_errno = errno;
        close(reply_fds[0]);
        return -saved_errno;
    }

    iovec response_io {
        .iov_base = response,
        .iov_len = response_size,
    };

    union {
        cmsghdr header;
        char space[CMSG_SPACE(sizeof(int))];
    } response_control {};

    msghdr response_message {
        .msg_name = nullptr,
        .msg_namelen = 0,
        .msg_iov = &response_io,
        .msg_iovlen = 1,
        .msg_control = &response_control,
        .msg_controllen = sizeof(response_control),
        .msg_flags = 0,
    };

    ssize_t received = 0;
    do {
        received = recvmsg(reply_fds[0], &response_message, MSG_CMSG_CLOEXEC);
    } while (received < 0 && errno == EINTR);

    auto saved_errno = errno;
    close(reply_fds[0]);

    if (received < 0)
        return -saved_errno;

    for (auto* header = CMSG_FIRSTHDR(&response_message); header; header = CMSG_NXTHDR(&response_message, header)) {
        if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS)
            continue;
        if (header->cmsg_len < CMSG_LEN(0))
            continue;
        auto count = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (size_t i = 0; i < count; ++i) {
            int fd = -1;
            __builtin_memcpy(&fd, CMSG_DATA(header) + i * sizeof(int), sizeof(fd));
            if (answered_fd < 0)
                answered_fd = fd;
            else
                close(fd);
        }
    }

    return received;
}

// Returns the descriptor the answer carried, zero when it carried none, or a negative errno.
static i64 ask_broker(Detail::ConnectBrokerRequest const& request, int socket_fd, bool expect_descriptor)
{
    if (s_connect_broker_fd < 0)
        return -EACCES;

    Detail::ConnectBrokerResponse response {};
    int answered_fd = -1;
    auto received = exchange_with_broker(s_connect_broker_fd, &request, sizeof(request), &socket_fd, socket_fd >= 0 ? 1 : 0, &response, sizeof(response), answered_fd);
    if (received < 0)
        return received;

    auto fail = [&](i64 error) {
        if (answered_fd >= 0)
            close(answered_fd);
        return error;
    };

    if (static_cast<size_t>(received) != sizeof(response) || response.magic != Detail::connect_broker_magic)
        return fail(-EACCES);
    if (response.error != 0)
        return fail(-response.error);
    if (!expect_descriptor)
        return fail(0);
    if (answered_fd < 0)
        return -EACCES;

    return answered_fd;
}

// Reading the caller's address directly would turn a bad pointer into a fault, where the syscall
// this stands in for returns EFAULT. Copying it through a pipe leaves the checking to the kernel,
// and a short copy means the far end is unreadable, which is the same answer.
static i64 copy_from_caller(void* destination, void const* source, size_t length)
{
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) < 0)
        return -errno;

    i64 result = 0;
    ssize_t written = 0;
    do {
        // A path is read up to the end of its page, past the memory that holds it. AddressSanitizer checks the bytes
        // that write() sends, and its report would make system calls that trap again inside this handler, where the
        // kernel kills the process for it. The kernel checks the bytes anyway, so this goes to it directly.
        written = syscall(__NR_write, fds[1], source, length);
    } while (written < 0 && errno == EINTR);

    if (written < 0)
        result = -errno;
    else if (static_cast<size_t>(written) != length)
        result = -EFAULT;

    if (result == 0) {
        ssize_t read_back = 0;
        do {
            read_back = read(fds[0], destination, length);
        } while (read_back < 0 && errno == EINTR);

        if (read_back < 0)
            result = -errno;
        else if (static_cast<size_t>(read_back) != length)
            result = -EFAULT;
    }

    close(fds[0]);
    close(fds[1]);
    return result;
}

// socket() hands back a stream or seqpacket socket that the Browser made. Only the broker can
// connect it, and bind and listen are blocked. Datagram sockets must not be handed out: sending
// to an address would bypass connect(). The caller can configure its socket before connecting.
static void emulate_socket(void* context)
{
    auto registers = syscall_registers(context);

    Detail::ConnectBrokerRequest request {};
    request.magic = Detail::connect_broker_magic;
    request.operation = static_cast<u32>(Detail::ConnectBrokerOperation::CreateSocket);
    request.socket_domain = static_cast<int>(registers.arguments[0]);
    request.socket_type = static_cast<int>(registers.arguments[1]);
    request.socket_protocol = static_cast<int>(registers.arguments[2]);

    auto socket_type = request.socket_type & ~(SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (socket_type != SOCK_STREAM && socket_type != SOCK_SEQPACKET) {
        set_syscall_result(registers, -ESOCKTNOSUPPORT);
        return;
    }

    auto saved_errno = errno;

    auto result = ask_broker(request, -1, true);

    // The descriptor arrives close-on-exec, because that is how it travels safely. Put the caller's
    // own preference back on it.
    if (result >= 0 && (request.socket_type & SOCK_CLOEXEC) == 0) {
        if (fcntl(static_cast<int>(result), F_SETFD, 0) < 0) {
            auto failure = -errno;
            close(static_cast<int>(result));
            result = failure;
        }
    }

    set_syscall_result(registers, result);
    errno = saved_errno;
}

static i64 connect_through_broker(int fd, void const* address, socklen_t address_length)
{
    if (fd < 0)
        return -EBADF;
    if (address_length < static_cast<socklen_t>(offsetof(sockaddr_un, sun_path)) || address_length > static_cast<socklen_t>(sizeof(sockaddr_un)))
        return -EINVAL;

    sockaddr_un local_address {};
    if (auto result = copy_from_caller(&local_address, address, address_length); result < 0)
        return result;

    if (local_address.sun_family != AF_UNIX)
        return -EAFNOSUPPORT;

    auto available = static_cast<size_t>(address_length) - offsetof(sockaddr_un, sun_path);
    if (available > sizeof(local_address.sun_path))
        available = sizeof(local_address.sun_path);

    // An abstract socket starts with a null byte. Nothing we broker uses one, and the abstract
    // namespace is shared by the whole network namespace, so it is not worth carrying.
    if (available == 0 || local_address.sun_path[0] == '\0')
        return -EACCES;

    size_t path_length = 0;
    while (path_length < available && local_address.sun_path[path_length] != '\0')
        ++path_length;

    Detail::ConnectBrokerRequest request {};
    request.magic = Detail::connect_broker_magic;
    request.operation = static_cast<u32>(Detail::ConnectBrokerOperation::Connect);
    request.socket_domain = AF_UNIX;
    request.path_length = static_cast<u32>(path_length);
    __builtin_memcpy(request.path, local_address.sun_path, path_length);

    return ask_broker(request, fd, false);
}

static void emulate_connect(void* context)
{
    auto registers = syscall_registers(context);
    auto fd = static_cast<int>(registers.arguments[0]);
    auto* address = reinterpret_cast<void const*>(registers.arguments[1]);
    auto address_length = static_cast<socklen_t>(registers.arguments[2]);

    auto saved_errno = errno;
    set_syscall_result(registers, connect_through_broker(fd, address, address_length));
    errno = saved_errno;
}

// Reads the string one page at a time, because a string can end right before a page that can't be read.
static i64 copy_path_from_caller(char* destination, char const* source, u32& length)
{
    static constexpr size_t smallest_page_size = 4096;

    size_t copied = 0;
    while (copied < PATH_MAX) {
        auto address = reinterpret_cast<FlatPtr>(source) + copied;
        auto chunk = min(smallest_page_size - address % smallest_page_size, PATH_MAX - copied);
        if (auto result = copy_from_caller(destination + copied, source + copied, chunk); result < 0)
            return result;
        for (size_t i = 0; i < chunk; ++i) {
            if (destination[copied + i] == '\0') {
                length = static_cast<u32>(copied + i);
                return 0;
            }
        }
        copied += chunk;
    }
    return -ENAMETOOLONG;
}

struct FileBrokerPath {
    int directory_fd { AT_FDCWD };
    char const* path { nullptr };
};

static i64 exchange_with_file_broker(Detail::FileBrokerRequest const& request, int const* fds, size_t fd_count, Detail::FileBrokerResponse& response)
{
    int answered_fd = -1;
    auto received = exchange_with_broker(Detail::file_broker_socket(), &request, sizeof(request), fds, fd_count, &response, sizeof(response), answered_fd);
    if (received < 0)
        return received == -EPIPE ? -EACCES : received;

    auto is_valid = static_cast<size_t>(received) == sizeof(response) && response.magic == Detail::file_broker_magic;
    if (!is_valid || response.error != 0) {
        if (answered_fd >= 0)
            close(answered_fd);
        return is_valid ? -response.error : -EACCES;
    }

    // The kernel drops a descriptor that doesn't fit in our table. Zero would read as a successful open of stdin.
    if (request.operation == static_cast<u32>(Detail::FileBrokerOperation::Open) && answered_fd < 0)
        return -EMFILE;
    return answered_fd >= 0 ? answered_fd : 0;
}

static i64 ask_file_broker(Detail::FileBrokerOperation operation, FileBrokerPath first, FileBrokerPath second, int flags, u32 mode, Detail::FileBrokerResponse& response)
{
    Detail::FileBrokerRequest request;
    __builtin_memset(&request, 0, sizeof(request));
    request.magic = Detail::file_broker_magic;
    request.operation = static_cast<u32>(operation);
    request.flags = flags;
    request.mode = mode;

    if (auto result = copy_path_from_caller(request.path, first.path, request.path_length); result < 0)
        return result;
    if (second.path) {
        if (auto result = copy_path_from_caller(request.second_path, second.path, request.second_path_length); result < 0)
            return result;
    }

    int directory_fds[2];
    size_t directory_fd_count = 0;
    if (first.directory_fd != AT_FDCWD && request.path[0] != '/') {
        request.directory_fd_mask |= 1;
        directory_fds[directory_fd_count++] = first.directory_fd;
    }
    if (second.path && second.directory_fd != AT_FDCWD && request.second_path[0] != '/') {
        request.directory_fd_mask |= 2;
        directory_fds[directory_fd_count++] = second.directory_fd;
    }

    return exchange_with_file_broker(request, directory_fds, directory_fd_count, response);
}

static i64 bind_through_broker(int fd, void const* address, socklen_t length)
{
    Detail::FileBrokerRequest request;
    __builtin_memset(&request, 0, sizeof(request));
    request.magic = Detail::file_broker_magic;
    request.operation = static_cast<u32>(Detail::FileBrokerOperation::BindSocket);
    request.directory_fd_mask = 1;

    if (length < sizeof(sa_family_t) || length > sizeof(sockaddr_storage))
        return -EINVAL;
    request.path_length = length;
    if (auto result = copy_from_caller(request.path, address, length); result < 0)
        return result;

    Detail::FileBrokerResponse response;
    return exchange_with_file_broker(request, &fd, 1, response);
}

static i64 open_through_broker(int directory_fd, char const* path, int flags, u32 mode)
{
    Detail::FileBrokerResponse response;
    auto result = ask_file_broker(Detail::FileBrokerOperation::Open, { directory_fd, path }, {}, flags, mode, response);

    // The descriptor arrives close-on-exec, because that is how it travels safely.
    if (result >= 0 && (flags & O_CLOEXEC) == 0 && fcntl(static_cast<int>(result), F_SETFD, 0) < 0) {
        auto failure = -errno;
        close(static_cast<int>(result));
        return failure;
    }
    return result;
}

static i64 stat_through_broker(int directory_fd, char const* path, void* buffer, bool follow_symlinks)
{
    Detail::FileBrokerResponse response;
    auto operation = follow_symlinks ? Detail::FileBrokerOperation::Stat : Detail::FileBrokerOperation::SymlinkStat;
    auto result = ask_file_broker(operation, { directory_fd, path }, {}, 0, 0, response);
    if (result < 0)
        return result;
    return copy_from_caller(buffer, &response.stat_result, sizeof(response.stat_result));
}

static i64 read_link_through_broker(int directory_fd, char const* path, char* buffer, size_t buffer_size)
{
    Detail::FileBrokerResponse response;
    auto result = ask_file_broker(Detail::FileBrokerOperation::ReadLink, { directory_fd, path }, {}, 0, 0, response);
    if (result < 0)
        return result;
    auto length = min<size_t>(response.link_length, buffer_size);
    if (auto copied = copy_from_caller(buffer, response.link, length); copied < 0)
        return copied;
    return static_cast<i64>(length);
}

static i64 simple_broker_call(Detail::FileBrokerOperation operation, int directory_fd, char const* path, int flags, u32 mode)
{
    Detail::FileBrokerResponse response;
    return ask_file_broker(operation, { directory_fd, path }, {}, flags, mode, response);
}

static i64 rename_through_broker(int old_directory_fd, char const* old_path, int new_directory_fd, char const* new_path)
{
    Detail::FileBrokerResponse response;
    return ask_file_broker(Detail::FileBrokerOperation::Rename, { old_directory_fd, old_path }, { new_directory_fd, new_path }, 0, 0, response);
}

static i64 perform_file_syscall(int syscall_number, SyscallRegisters const& registers)
{
    using Operation = Detail::FileBrokerOperation;
    auto number = [&](size_t index) { return static_cast<int>(registers.arguments[index]); };
    auto path = [&](size_t index) { return reinterpret_cast<char const*>(registers.arguments[index]); };

    switch (syscall_number) {
#ifdef __NR_bind
    case __NR_bind:
        return bind_through_broker(number(0), reinterpret_cast<void const*>(registers.arguments[1]), static_cast<socklen_t>(number(2)));
#endif
#ifdef __NR_open
    case __NR_open:
        return open_through_broker(AT_FDCWD, path(0), number(1), static_cast<u32>(number(2)));
#endif
#ifdef __NR_openat
    case __NR_openat:
        return open_through_broker(number(0), path(1), number(2), static_cast<u32>(number(3)));
#endif
#ifdef __NR_access
    case __NR_access:
        return simple_broker_call(Operation::Access, AT_FDCWD, path(0), number(1), 0);
#endif
#ifdef __NR_faccessat
    case __NR_faccessat:
        return simple_broker_call(Operation::Access, number(0), path(1), number(2), 0);
#endif
#ifdef __NR_faccessat2
    case __NR_faccessat2:
        return simple_broker_call(Operation::Access, number(0), path(1), number(2), static_cast<u32>(number(3)));
#endif
#ifdef __NR_stat
    case __NR_stat:
        return stat_through_broker(AT_FDCWD, path(0), reinterpret_cast<void*>(registers.arguments[1]), true);
#endif
#ifdef __NR_newfstatat
    case __NR_newfstatat: {
        auto flags = number(3);
        if (flags & AT_EMPTY_PATH) {
            // Since Linux 6.11, AT_EMPTY_PATH accepts a null path as well.
            char first_byte = '\0';
            if (path(1)) {
                if (auto result = copy_from_caller(&first_byte, path(1), 1); result < 0)
                    return result;
            }
#    ifdef __NR_fstat
            if (first_byte == '\0') {
                auto result = syscall(__NR_fstat, number(0), registers.arguments[2]);
                return result < 0 ? -errno : result;
            }
#    endif
        }
        return stat_through_broker(number(0), path(1), reinterpret_cast<void*>(registers.arguments[2]), (flags & AT_SYMLINK_NOFOLLOW) == 0);
    }
#endif
#ifdef __NR_readlink
    case __NR_readlink:
        return read_link_through_broker(AT_FDCWD, path(0), reinterpret_cast<char*>(registers.arguments[1]), registers.arguments[2]);
#endif
#ifdef __NR_readlinkat
    case __NR_readlinkat:
        return read_link_through_broker(number(0), path(1), reinterpret_cast<char*>(registers.arguments[2]), registers.arguments[3]);
#endif
#ifdef __NR_mkdir
    case __NR_mkdir:
        return simple_broker_call(Operation::MakeDirectory, AT_FDCWD, path(0), 0, static_cast<u32>(number(1)));
#endif
#ifdef __NR_mkdirat
    case __NR_mkdirat:
        return simple_broker_call(Operation::MakeDirectory, number(0), path(1), 0, static_cast<u32>(number(2)));
#endif
#ifdef __NR_unlink
    case __NR_unlink:
        return simple_broker_call(Operation::RemoveFile, AT_FDCWD, path(0), 0, 0);
#endif
#ifdef __NR_rmdir
    case __NR_rmdir:
        return simple_broker_call(Operation::RemoveDirectory, AT_FDCWD, path(0), 0, 0);
#endif
#ifdef __NR_unlinkat
    case __NR_unlinkat:
        return simple_broker_call((number(2) & AT_REMOVEDIR) ? Operation::RemoveDirectory : Operation::RemoveFile, number(0), path(1), 0, 0);
#endif
#ifdef __NR_rename
    case __NR_rename:
        return rename_through_broker(AT_FDCWD, path(0), AT_FDCWD, path(1));
#endif
#ifdef __NR_renameat
    case __NR_renameat:
        return rename_through_broker(number(0), path(1), number(2), path(3));
#endif
#ifdef __NR_renameat2
    case __NR_renameat2:
        // The broker only replaces, so anything else has to fall back to doing without, as on a kernel without flags.
        if (registers.arguments[4] != 0)
            return -ENOSYS;
        return rename_through_broker(number(0), path(1), number(2), path(3));
#endif
    default:
        return -ENOSYS;
    }
}

static void emulate_file_syscall(int syscall_number, void* context)
{
    auto registers = syscall_registers(context);
    auto saved_errno = errno;
    set_syscall_result(registers, perform_file_syscall(syscall_number, registers));
    errno = saved_errno;
}

// The kernel kills a process outright for a trap that arrives while SIGSYS is blocked, so a path that a thread looks up
// then would end the helper instead of reaching the broker. glibc blocks every signal while a thread exits, and freeing
// that thread's memory can read /proc/sys/vm/overcommit_memory. This keeps SIGSYS out of every mask that the helper
// asks for. The handler's context holds the mask that the thread gets back when the handler returns.
static i64 change_signal_mask(SyscallRegisters const& registers, void* context)
{
    static constexpr u64 unblockable = (1ull << (SIGKILL - 1)) | (1ull << (SIGSTOP - 1)) | (1ull << (SIGSYS - 1));

    auto how = static_cast<int>(registers.arguments[0]);
    auto const* new_mask = reinterpret_cast<u64 const*>(registers.arguments[1]);
    auto* old_mask = reinterpret_cast<u64*>(registers.arguments[2]);
    if (registers.arguments[3] != sizeof(u64))
        return -EINVAL;

    auto& thread_mask = static_cast<ucontext_t*>(context)->uc_sigmask;
    u64 current = 0;
    __builtin_memcpy(&current, &thread_mask, sizeof(current));

    auto updated = current;
    if (new_mask) {
        u64 requested = 0;
        __builtin_memcpy(&requested, new_mask, sizeof(requested));
        switch (how) {
        case SIG_BLOCK:
            updated |= requested;
            break;
        case SIG_UNBLOCK:
            updated &= ~requested;
            break;
        case SIG_SETMASK:
            updated = requested;
            break;
        default:
            return -EINVAL;
        }
    }

    if (old_mask)
        __builtin_memcpy(old_mask, &current, sizeof(current));

    updated &= ~unblockable;
    __builtin_memcpy(&thread_mask, &updated, sizeof(updated));
    return 0;
}

static void emulate_signal_mask_change(void* context)
{
    auto registers = syscall_registers(context);
    set_syscall_result(registers, change_signal_mask(registers, context));
}

static void append_syscall_details(LogLine& line, siginfo_t const* info, void* context)
{
    line.append("syscall ");
    line.append(syscall_name(info->si_syscall));
    line.append(" (");
    line.append_unsigned(static_cast<u64>(info->si_syscall));
    line.append("), architecture ");
    line.append_hex(info->si_arch);
    line.append(", instruction pointer ");
    line.append_hex(reinterpret_cast<FlatPtr>(info->si_call_addr));

    auto registers = syscall_registers(context);
    line.append(", args [");
    for (size_t i = 0; i < array_size(registers.arguments); ++i) {
        if (i != 0)
            line.append(", ");
        line.append_hex(registers.arguments[i]);
    }
    line.append("]");
}

static void handle_sigsys(int, siginfo_t* info, void* context)
{
    if (info->si_code == SYS_SECCOMP && info->si_arch == audit_architecture) {
#if defined(__NR_newfstatat) && defined(__NR_fstat)
        if (info->si_syscall == __NR_newfstatat && info->si_errno == emulate_fstatat_trap) {
            emulate_fstatat(context);
            return;
        }
#endif
        if (info->si_errno == broker_socket_trap) {
            emulate_socket(context);
            return;
        }
        if (info->si_errno == broker_connect_trap) {
            emulate_connect(context);
            return;
        }
        if (info->si_errno == file_broker_trap) {
            emulate_file_syscall(info->si_syscall, context);
            return;
        }
#ifdef __NR_rt_sigprocmask
        if (info->si_syscall == __NR_rt_sigprocmask && info->si_errno == signal_mask_trap) {
            emulate_signal_mask_change(context);
            return;
        }
#endif
        for (auto trap : { refused_path_syscall_trap, logged_refusal_trap }) {
            if (info->si_errno > trap && info->si_errno < trap + 0x1000) {
                auto error = info->si_errno - trap;
                if (s_logs_refusals) {
                    LogLine line;
                    line.append("Sandbox refusal in ");
                    line.append(s_process_name);
                    line.append(": errno ");
                    line.append_unsigned(static_cast<u64>(error));
                    line.append(" for ");
                    append_syscall_details(line, info, context);
                    line.write_to_stderr();
                }
                set_syscall_result(syscall_registers(context), -error);
                return;
            }
        }
    }

    LogLine line;
    line.append("Sandbox violation in ");
    line.append(s_process_name);
    line.append(": disallowed ");
    append_syscall_details(line, info, context);
    line.write_to_stderr();

    Core::CrashHandler::handle_fatal_signal(SIGSYS, info, context);
}

static ErrorOr<void> install_sigsys_handler()
{
    struct sigaction action {};
    action.sa_sigaction = handle_sigsys;
    action.sa_flags = SA_SIGINFO;
    if (sigemptyset(&action.sa_mask) < 0)
        return Error::from_syscall("sigemptyset"sv, errno);
    if (sigaction(SIGSYS, &action, nullptr) < 0)
        return Error::from_syscall("sigaction(SIGSYS)"sv, errno);

    (void)prctl(PR_GET_NAME, s_process_name, 0ul, 0ul, 0ul);
    s_logs_refusals = sandbox_debugging_is_enabled();
    return {};
}

}

sock_filter SeccompPolicy::path_syscall_action()
{
    if (file_broker_is_active())
        m_sends_paths_to_file_broker = true;
    return SECCOMP_ALLOW;
}

static sock_filter refused_path_syscall_action(int error)
{
    if (file_broker_is_active())
        return BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP | (refused_path_syscall_trap + error));
    return SECCOMP_ERRNO(error);
}

SeccompPolicy::SeccompPolicy()
{
    append_architecture_check();
    append_load_syscall_number();
}

void SeccompPolicy::append(sock_filter filter)
{
    MUST(m_filter.try_append(filter));
}

void SeccompPolicy::append_architecture_check()
{
    append(SECCOMP_LOAD_ARCHITECTURE);
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, audit_architecture, 1, 0));
    append(SECCOMP_TRAP);
}

void SeccompPolicy::append_load_syscall_number()
{
    append(SECCOMP_LOAD_SYSCALL_NR);
}

void SeccompPolicy::append_kill()
{
    append(SECCOMP_TRAP);
}

void SeccompPolicy::deny_readonly_filesystem_probes()
{
#ifdef __NR_open
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_open, 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(1));
    append(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, ~read_only_open_flags));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1));
    append(refused_path_syscall_action(EACCES));
    append(SECCOMP_LOAD_SYSCALL_NR);
#endif
#ifdef __NR_openat
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_openat, 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(2));
    append(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, ~read_only_open_flags));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1));
    append(refused_path_syscall_action(EACCES));
    append(SECCOMP_LOAD_SYSCALL_NR);
#endif
#ifdef __NR_access
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_access, 0, 1));
    append(refused_path_syscall_action(EACCES));
#endif
#ifdef __NR_faccessat
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_faccessat, 0, 1));
    append(refused_path_syscall_action(EACCES));
#endif
#ifdef __NR_faccessat2
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_faccessat2, 0, 1));
    append(refused_path_syscall_action(EACCES));
#endif
}

void SeccompPolicy::allow_readonly_file_opens()
{
#ifdef __NR_open
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_open, 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(1));
    append(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, ~read_only_open_flags));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1));
    append(path_syscall_action());
    append(SECCOMP_LOAD_SYSCALL_NR);
#endif
#ifdef __NR_openat
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_openat, 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(2));
    append(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, ~read_only_open_flags));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1));
    append(path_syscall_action());
    append(SECCOMP_LOAD_SYSCALL_NR);
#endif
}

void SeccompPolicy::allow_filesystem_metadata_queries()
{
    SECCOMP_APPEND_PATH_SYSCALL_IF_DEFINED(*this, access);
    SECCOMP_APPEND_PATH_SYSCALL_IF_DEFINED(*this, faccessat);
    SECCOMP_APPEND_PATH_SYSCALL_IF_DEFINED(*this, faccessat2);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, fstatfs);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, getdents64);
    SECCOMP_APPEND_PATH_SYSCALL_IF_DEFINED(*this, newfstatat);
    SECCOMP_APPEND_PATH_SYSCALL_IF_DEFINED(*this, readlink);
    SECCOMP_APPEND_PATH_SYSCALL_IF_DEFINED(*this, readlinkat);
    SECCOMP_APPEND_PATH_SYSCALL_IF_DEFINED(*this, stat);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, statfs);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, statx);
}

void SeccompPolicy::allow_filesystem_writes()
{
    SECCOMP_APPEND_PATH_SYSCALL_IF_DEFINED(*this, open);
    SECCOMP_APPEND_PATH_SYSCALL_IF_DEFINED(*this, openat);
    SECCOMP_APPEND_PATH_SYSCALL_IF_DEFINED(*this, unlink);
    SECCOMP_APPEND_PATH_SYSCALL_IF_DEFINED(*this, unlinkat);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, link);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, linkat);
    SECCOMP_APPEND_PATH_SYSCALL_IF_DEFINED(*this, rename);
    SECCOMP_APPEND_PATH_SYSCALL_IF_DEFINED(*this, renameat);
    SECCOMP_APPEND_PATH_SYSCALL_IF_DEFINED(*this, renameat2);
    SECCOMP_APPEND_PATH_SYSCALL_IF_DEFINED(*this, mkdir);
    SECCOMP_APPEND_PATH_SYSCALL_IF_DEFINED(*this, mkdirat);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, symlink);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, symlinkat);
    SECCOMP_APPEND_PATH_SYSCALL_IF_DEFINED(*this, rmdir);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, fsync);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, fdatasync);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, fallocate);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, fchmod);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, flock);
#ifdef __NR_utimensat
    // Landlock does not restrict timestamp changes. Mesa's shader cache attempts these,
    // so return an ordinary error instead of terminating the process.
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_utimensat, 0, 1));
    append(SECCOMP_ERRNO(EPERM));
#endif

    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_fcntl, 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(1));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, F_GETLK, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);
    append(BPF_STMT(BPF_ALU | BPF_ADD | BPF_K, 0));

    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_fcntl, 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(1));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, F_SETLK, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);
    append(BPF_STMT(BPF_ALU | BPF_ADD | BPF_K, 0));

    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_fcntl, 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(1));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, F_SETLKW, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);
    append(BPF_STMT(BPF_ALU | BPF_ADD | BPF_K, 0));
}

void SeccompPolicy::allow_file_descriptor_operations()
{
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, read);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, readv);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, write);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, writev);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, close);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, fstat);
#if defined(__NR_newfstatat) && defined(__NR_fstat)
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_newfstatat, 0, 1));
    append(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP | emulate_fstatat_trap));
#endif
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, dup);
#ifdef __NR_dup2
    SECCOMP_APPEND_ALLOW_SYSCALL(*this, dup2);
#endif
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, dup3);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, pipe2);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, ftruncate);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, lseek);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, pread64);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, pwrite64);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, sendfile);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, memfd_create);

    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_fcntl, 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(1));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, F_GETFD, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);
    append(BPF_STMT(BPF_ALU | BPF_ADD | BPF_K, 0));

    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_fcntl, 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(1));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, F_SETFD, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);
    append(BPF_STMT(BPF_ALU | BPF_ADD | BPF_K, 0));

    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_fcntl, 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(1));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, F_GETFL, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);
    append(BPF_STMT(BPF_ALU | BPF_ADD | BPF_K, 0));

    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_fcntl, 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(1));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, F_SETFL, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);
    append(BPF_STMT(BPF_ALU | BPF_ADD | BPF_K, 0));

    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_fcntl, 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(1));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, F_DUPFD_CLOEXEC, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);
    append(BPF_STMT(BPF_ALU | BPF_ADD | BPF_K, 0));

#ifdef F_DUPFD_QUERY
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_fcntl, 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(1));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, F_DUPFD_QUERY, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);
    append(BPF_STMT(BPF_ALU | BPF_ADD | BPF_K, 0));
#endif

#ifdef F_ADD_SEALS
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_fcntl, 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(1));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, F_ADD_SEALS, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);
    append(BPF_STMT(BPF_ALU | BPF_ADD | BPF_K, 0));
#endif

#ifdef F_GET_SEALS
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_fcntl, 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(1));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, F_GET_SEALS, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);
    append(BPF_STMT(BPF_ALU | BPF_ADD | BPF_K, 0));
#endif

    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_ioctl, 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(1));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, FIONBIO, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);
    append(BPF_STMT(BPF_ALU | BPF_ADD | BPF_K, 0));

    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_ioctl, 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(1));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, FIONREAD, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);
    append(BPF_STMT(BPF_ALU | BPF_ADD | BPF_K, 0));

    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_ioctl, 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(1));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, TCGETS, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);
    append(BPF_STMT(BPF_ALU | BPF_ADD | BPF_K, 0));

#ifdef TCGETS2
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_ioctl, 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(1));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, TCGETS2, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);
    append(BPF_STMT(BPF_ALU | BPF_ADD | BPF_K, 0));
#endif
}

void SeccompPolicy::allow_process_creation()
{
    // Compiler children inherit this filter but have different thread group IDs. Landlock's signal scope confines their
    // signals to the sandbox's process tree. Without it, any thread group could be signalled, including the Browser's,
    // so only this process may be, and a child can't signal at all. It can't exec anything there either, so it can only
    // be a fork that ends with its parent.
    if (signals_stay_within_the_process_tree()) {
        SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, tgkill);
    } else {
#ifdef __NR_tgkill
        append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_tgkill, 0, 4));
        append(SECCOMP_LOAD_ARGUMENT(0));
        append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<u32>(getpid()), 0, 1));
        append(SECCOMP_ALLOW);
        append(SECCOMP_LOAD_SYSCALL_NR);
#endif
    }
    // Core::Process::spawn() asks for compiler children to die with their parent.
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_prctl, 0, 6));
    append(SECCOMP_LOAD_ARGUMENT(0));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, PR_SET_PDEATHSIG, 0, 3));
    append(SECCOMP_LOAD_ARGUMENT(1));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SIGKILL, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);
#ifdef __NR_arch_prctl
    SECCOMP_APPEND_ALLOW_SYSCALL(*this, arch_prctl);
#endif
#ifdef __NR_getppid
    SECCOMP_APPEND_ALLOW_SYSCALL(*this, getppid);
#endif
#ifdef __NR_fork
    SECCOMP_APPEND_ALLOW_SYSCALL(*this, fork);
#endif

#ifdef __NR_clone
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_clone, 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(0));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, fork_clone_flags, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);
    append(BPF_STMT(BPF_ALU | BPF_ADD | BPF_K, 0));

    // musl implements fork() as clone(SIGCHLD, ...).
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_clone, 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(0));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SIGCHLD, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);
    append(BPF_STMT(BPF_ALU | BPF_ADD | BPF_K, 0));
#endif

    // Core::Process::spawn() closes every descriptor that the child was not given, just before exec().
#ifdef __NR_close_range
    SECCOMP_APPEND_ALLOW_SYSCALL(*this, close_range);
#endif
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, execve);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, execveat);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, wait4);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, waitid);
}

// NB: Socket options reach deep into the kernel. SO_ATTACH_FILTER, for one, loads a classic BPF program into it. So we
//     allow only the options that we know to be used, and install() refuses every other one with EPERM.
void SeccompPolicy::append_allow_socket_options(u32 syscall_number, u32 level, ReadonlySpan<u32> options)
{
    // Every jump offset below is a byte, so the group has to stay well short of 255 options.
    VERIFY(!options.is_empty());
    VERIFY(options.size() < 32);
    auto option_count = static_cast<u8>(options.size());

    // The group is the syscall comparison, the level load and comparison, the option load, one comparison per option,
    // the allow, and the reload of the syscall number. Any other syscall jumps past all of it.
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, syscall_number, 0, static_cast<u8>(option_count + 5)));
    append(SECCOMP_LOAD_ARGUMENT(1));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, level, 0, static_cast<u8>(option_count + 2)));
    append(SECCOMP_LOAD_ARGUMENT(2));
    for (size_t i = 0; i < options.size(); ++i) {
        // A matching option jumps to the allow. The last comparison is the one that has to step over the allow when
        // it does not match.
        auto instructions_to_allow = static_cast<u8>(options.size() - i - 1);
        auto instructions_past_allow = static_cast<u8>(i + 1 == options.size() ? 1 : 0);
        append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, options[i], instructions_to_allow, instructions_past_allow));
    }
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);
}

// NB: This group covers sockets that the process already has. The Browser mints every Ladybird IPC
//     channel with socketpair() and hands the helper a connected descriptor, so no sandboxed helper
//     needs to create or connect a socket of its own to talk to us.
void SeccompPolicy::allow_ipc()
{
    // LibIPC's Rust transports shut their socket down when they stop, so the peer sees them go at once.
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, shutdown);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, eventfd2);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, poll);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, ppoll);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, pselect6);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, recvmsg);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, recvfrom);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, recvmmsg);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, sendmsg);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, sendmmsg);
#ifdef __NR_sendto
    // send() also uses sendto(), with a null destination. Check both halves of the pointer.
    // An addressed send falls through so allow_network() can permit it for RequestServer.
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_sendto, 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(4));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 3));
    append(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, static_cast<u32>(offsetof(seccomp_data, args[4]) + sizeof(u32))));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);
#endif
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, getsockname);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, getpeername);

#if defined(__NR_getsockopt) && defined(__NR_setsockopt)
    // LibIPC sizes the buffers of its sockets.
    static constexpr Array<u32, 2> buffer_sizes { SO_SNDBUF, SO_RCVBUF };
    append_allow_socket_options(__NR_setsockopt, SOL_SOCKET, buffer_sizes);

    static constexpr Array<u32, 5> queries { SO_ERROR, SO_TYPE, SO_PEERCRED, SO_SNDBUF, SO_RCVBUF };
    append_allow_socket_options(__NR_getsockopt, SOL_SOCKET, queries);
#endif
}

// Event loops outside of LibCore, such as Rust's tokio, wait on their descriptors with epoll.
void SeccompPolicy::allow_epoll()
{
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, epoll_create1);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, epoll_ctl);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, epoll_wait);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, epoll_pwait);
}

// NB: A helper mints a socket pair to hand one end of a new IPC channel to another process. A helper that only ever
//     uses the channels the Browser handed it does not need this.
void SeccompPolicy::allow_socket_pairs()
{
#ifdef __NR_socketpair
    // The kernel creates both sockets in the requested domain before it asks the domain whether it can pair them,
    // loading the protocol's module if need be. Every other domain refuses to pair, so the domain is all that a
    // request outside AF_UNIX would reach, and it can be a rarely used one.
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_socketpair, 0, 9));
    append(SECCOMP_LOAD_ARGUMENT(0));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_UNIX, 1, 0));
    append(SECCOMP_ERRNO(EAFNOSUPPORT));
    // A datagram socketpair can send to arbitrary UNIX socket addresses without connect().
    append(SECCOMP_LOAD_ARGUMENT(1));
    append(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, static_cast<u32>(~(SOCK_CLOEXEC | SOCK_NONBLOCK))));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SOCK_STREAM, 2, 0));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SOCK_SEQPACKET, 1, 0));
    append(SECCOMP_ERRNO(ESOCKTNOSUPPORT));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);
#endif
}

// socket(AF_UNIX) asks the Browser for a stream or seqpacket socket. connect() goes to the
// Browser too, which connects only to the one endpoint it brokers. Every other domain is refused
// outright, so this cannot become a way onto the network either.
void SeccompPolicy::broker_unix_socket_connections()
{
    // Each request to the broker carries a socket pair of its own for the answer.
    allow_socket_pairs();

#ifdef __NR_socket
    // Any other domain falls through to the refusal that install() puts at the end.
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_socket, 0, 3));
    append(SECCOMP_LOAD_ARGUMENT(0));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_UNIX, 0, 1));
    append(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP | broker_socket_trap));
    append(SECCOMP_LOAD_SYSCALL_NR);
#endif
#ifdef __NR_connect
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_connect, 0, 1));
    append(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP | broker_connect_trap));
#endif

#if defined(__NR_getsockopt) && defined(__NR_setsockopt)
    // libpulse asks for credentials and for a higher priority on its connection to the audio server.
    static constexpr Array<u32, 2> client_options { SO_PASSCRED, SO_PRIORITY };
    append_allow_socket_options(__NR_setsockopt, SOL_SOCKET, client_options);
    append_allow_socket_options(__NR_getsockopt, SOL_SOCKET, client_options);
#endif
}

void set_connect_broker_fd(int fd)
{
    s_connect_broker_fd = fd;
}

void SeccompPolicy::allow_pulseaudio_client_file_operations()
{
    // libpulse re-creates its runtime directory before it looks for a socket there, and removes a directory it made
    // again when it can't use it; Landlock decides where that succeeds.
    SECCOMP_APPEND_PATH_SYSCALL_IF_DEFINED(*this, mkdir);
    SECCOMP_APPEND_PATH_SYSCALL_IF_DEFINED(*this, mkdirat);
    SECCOMP_APPEND_PATH_SYSCALL_IF_DEFINED(*this, rmdir);
#ifdef __NR_unlinkat
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_unlinkat, 0, 4));
    append(SECCOMP_LOAD_ARGUMENT(2));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AT_REMOVEDIR, 0, 1));
    append(path_syscall_action());
    append(SECCOMP_LOAD_SYSCALL_NR);
#endif

    // With no cookie yet, libpulse opens one for writing, to create it. Refuse that the way Landlock refuses a write
    // there, rather than killing the process: libpulse then opens the cookie read-only, and goes on without one if
    // there's none to read. A read-only open falls through to whatever else the policy allows.
#ifdef __NR_open
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_open, 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(1));
    append(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, ~read_only_open_flags));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0));
    append(refused_path_syscall_action(EACCES));
    append(SECCOMP_LOAD_SYSCALL_NR);
#endif
#ifdef __NR_openat
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_openat, 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(2));
    append(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, ~read_only_open_flags));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0));
    append(refused_path_syscall_action(EACCES));
    append(SECCOMP_LOAD_SYSCALL_NR);
#endif

    // It also holds a record lock on its cookie while reading it.
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_fcntl, 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(1));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, F_SETLK, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);
    append(BPF_STMT(BPF_ALU | BPF_ADD | BPF_K, 0));

    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_fcntl, 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(1));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, F_SETLKW, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);
    append(BPF_STMT(BPF_ALU | BPF_ADD | BPF_K, 0));
}

// NB: Seccomp cannot follow the sockaddr pointer that connect() is given, and Landlock does not
//     mediate UNIX socket connections, so neither mechanism can restrict which endpoint a process
//     reaches. The domain argument of socket() is the only part of this the kernel lets us filter,
//     and connect() fails with EAFNOSUPPORT when the address family does not match the socket.
//     Being able to create a socket in a domain is therefore the same thing as being able to
//     address every endpoint in it.
void SeccompPolicy::append_allow_route_netlink_sockets()
{
#ifdef __NR_socket
    // glibc enumerates local interfaces with NETLINK_ROUTE sockets.
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_socket, 0, 11));
    append(SECCOMP_LOAD_ARGUMENT(0));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_NETLINK, 0, 8));
    append(SECCOMP_LOAD_ARGUMENT(1));
    append(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, ~static_cast<u32>(SOCK_CLOEXEC | SOCK_NONBLOCK)));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SOCK_RAW, 1, 0));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SOCK_DGRAM, 0, 3));
    append(SECCOMP_LOAD_ARGUMENT(2));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, NETLINK_ROUTE, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_ERRNO(EPERM));
    append(SECCOMP_LOAD_SYSCALL_NR);
#endif
}

void SeccompPolicy::allow_network()
{
    append_allow_route_netlink_sockets();

#ifdef __NR_socket
    // Internet sockets serve TCP streams and UDP datagrams, including the default protocol. Other domains fall
    // through to the groups after this one.
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_socket, 0, 16));
    append(SECCOMP_LOAD_ARGUMENT(0));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_INET, 1, 0));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_INET6, 0, 13));
    append(SECCOMP_LOAD_ARGUMENT(1));
    append(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, ~static_cast<u32>(SOCK_CLOEXEC | SOCK_NONBLOCK)));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SOCK_STREAM, 0, 4));
    append(SECCOMP_LOAD_ARGUMENT(2));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, IPPROTO_TCP, 0, 6));
    append(SECCOMP_ALLOW);
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SOCK_DGRAM, 0, 4));
    append(SECCOMP_LOAD_ARGUMENT(2));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, IPPROTO_UDP, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_ERRNO(EPERM));
    append(SECCOMP_LOAD_SYSCALL_NR);
#endif

    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, sendto);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, connect);
    // Binding a UNIX socket creates a file, which is for Landlock or the file broker to decide on.
    SECCOMP_APPEND_PATH_SYSCALL_IF_DEFINED(*this, bind);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, shutdown);

#ifdef __NR_setsockopt
    // libcurl turns off Nagle's algorithm, and glibc's resolver asks for ICMP errors on its UDP sockets. For HTTP/3,
    // libcurl stops the kernel from fragmenting QUIC packets and lets it coalesce the ones that it receives.
    static constexpr Array<u32, 1> tcp_options { TCP_NODELAY };
    append_allow_socket_options(__NR_setsockopt, IPPROTO_TCP, tcp_options);
    static constexpr Array<u32, 1> udp_options { UDP_GRO };
    append_allow_socket_options(__NR_setsockopt, IPPROTO_UDP, udp_options);
    static constexpr Array<u32, 2> ipv4_options { IP_RECVERR, IP_MTU_DISCOVER };
    append_allow_socket_options(__NR_setsockopt, IPPROTO_IP, ipv4_options);
    static constexpr Array<u32, 3> ipv6_options { IPV6_RECVERR, IPV6_V6ONLY, IPV6_MTU_DISCOVER };
    append_allow_socket_options(__NR_setsockopt, IPPROTO_IPV6, ipv6_options);
#endif

    // Required by glibc's if_nametoindex() when resolving IPv6 link-local nameserver scopes.
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_ioctl, 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(1));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SIOCGIFINDEX, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);
    append(BPF_STMT(BPF_ALU | BPF_ADD | BPF_K, 0));
}

// For the file broker's own policy. The broker runs without the file system filter, so it may bind outright, while
// the helpers' policies send bind through the broker.
void SeccompPolicy::allow_binding_sockets()
{
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, bind);
}

// ICE only exchanges UDP datagrams, so unlike allow_network() this leaves out streams. Its mDNS responder shares the
// mDNS port with the system's and joins the mDNS multicast group.
void SeccompPolicy::allow_datagram_network()
{
    append_allow_route_netlink_sockets();

#ifdef __NR_socket
    // Internet sockets serve UDP datagrams, including the default protocol. Other domains fall through to the groups
    // after this one.
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_socket, 0, 11));
    append(SECCOMP_LOAD_ARGUMENT(0));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_INET, 1, 0));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_INET6, 0, 8));
    append(SECCOMP_LOAD_ARGUMENT(1));
    append(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, ~static_cast<u32>(SOCK_CLOEXEC | SOCK_NONBLOCK)));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SOCK_DGRAM, 0, 4));
    append(SECCOMP_LOAD_ARGUMENT(2));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, IPPROTO_UDP, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_ERRNO(EPERM));
    append(SECCOMP_LOAD_SYSCALL_NR);
#endif

    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, sendto);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, connect);
    // Binding a UNIX socket creates a file, which is for Landlock or the file broker to decide on.
    SECCOMP_APPEND_PATH_SYSCALL_IF_DEFINED(*this, bind);

#ifdef __NR_setsockopt
    static constexpr Array<u32, 3> socket_options { SO_REUSEADDR, SO_REUSEPORT, SO_BROADCAST };
    append_allow_socket_options(__NR_setsockopt, SOL_SOCKET, socket_options);
    static constexpr Array<u32, 1> ipv4_options { IP_ADD_MEMBERSHIP };
    append_allow_socket_options(__NR_setsockopt, IPPROTO_IP, ipv4_options);
#endif
}

void SeccompPolicy::allow_memory_without_executable_mappings()
{
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_mmap, 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(2));
    append(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, PROT_EXEC));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);

#ifdef __NR_mmap2
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_mmap2, 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(2));
    append(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, PROT_EXEC));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);
#endif

    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_mprotect, 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(2));
    append(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, PROT_EXEC));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);

    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, mremap);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, munmap);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, madvise);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, brk);
}

void SeccompPolicy::allow_executable_memory_mappings()
{
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_mmap, 0, 8));
    append(SECCOMP_LOAD_ARGUMENT(2));
    append(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, PROT_EXEC));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 4, 0));
    append(SECCOMP_LOAD_ARGUMENT(2));
    append(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, PROT_WRITE));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);

#ifdef __NR_mmap2
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_mmap2, 0, 8));
    append(SECCOMP_LOAD_ARGUMENT(2));
    append(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, PROT_EXEC));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 4, 0));
    append(SECCOMP_LOAD_ARGUMENT(2));
    append(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, PROT_WRITE));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);
#endif

    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_mprotect, 0, 8));
    append(SECCOMP_LOAD_ARGUMENT(2));
    append(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, PROT_EXEC));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 4, 0));
    append(SECCOMP_LOAD_ARGUMENT(2));
    append(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, PROT_WRITE));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);
}

void SeccompPolicy::allow_writable_executable_memory_mappings()
{
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_mmap, 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(2));
    append(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, PROT_WRITE | PROT_EXEC));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, PROT_WRITE | PROT_EXEC, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);

#ifdef __NR_mmap2
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_mmap2, 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(2));
    append(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, PROT_WRITE | PROT_EXEC));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, PROT_WRITE | PROT_EXEC, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);
#endif

    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_mprotect, 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(2));
    append(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, PROT_WRITE | PROT_EXEC));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, PROT_WRITE | PROT_EXEC, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);
}

// Memory protection keys, for JIT code whose pages are writable and executable
// but which no thread may write unless it enables writes to the key's pages
// in its own register. Only pages with a key may be writable and executable.
void SeccompPolicy::allow_memory_protection_keys()
{
#ifdef __NR_pkey_mprotect
    // NB: LibJS allocates its key while it loads, before the sandbox starts.
    //     With pkey_alloc() allowed, sandboxed code could make itself a key
    //     that it may write. We deny key 0 and key -1 (0xFFFFFFFF). Key 0 is
    //     the default key, which every thread may write, and -1 makes
    //     pkey_mprotect() behave like mprotect(). Either value would let
    //     sandboxed code make pages writable and executable without a key.
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_pkey_mprotect, 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(3));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 2, 0));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0xFFFFFFFF, 1, 0));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);
#endif
}

void SeccompPolicy::allow_threads()
{
#ifdef __NR_clone
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_clone, 0, 8));
    append(SECCOMP_LOAD_ARGUMENT(0));
    append(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, thread_clone_required_flags));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, thread_clone_required_flags, 0, 4));
    append(SECCOMP_LOAD_ARGUMENT(0));
    append(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, ~thread_clone_allowed_flags));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);
#endif
#ifdef __NR_clone3
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_clone3, 0, 1));
    append(SECCOMP_ERRNO(ENOSYS));
#endif
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, futex);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, futex_time64);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, set_tid_address);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, set_robust_list);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, rseq);
}

void SeccompPolicy::allow_signals()
{
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, rt_sigaction);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, rt_sigprocmask);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, rt_sigreturn);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, sigaltstack);
#ifdef __NR_tgkill
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_tgkill, 0, 4));
    append(SECCOMP_LOAD_ARGUMENT(0));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<u32>(getpid()), 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);
#endif
}

void SeccompPolicy::allow_clocks()
{
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, clock_gettime);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, clock_getres);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, clock_nanosleep);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, nanosleep);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, restart_syscall);
}

void SeccompPolicy::allow_gpu_device_operations()
{
    // NVIDIA tries to chmod its device nodes. Refuse permission changes with an ordinary
    // error so driver initialization can continue without granting pathname-based chmod.
#ifdef __NR_chmod
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_chmod, 0, 1));
    append(SECCOMP_ERRNO(EPERM));
#endif
#ifdef __NR_fchmodat
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_fchmodat, 0, 1));
    append(SECCOMP_ERRNO(EPERM));
#endif
#ifdef __NR_fchmodat2
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_fchmodat2, 0, 1));
    append(SECCOMP_ERRNO(EPERM));
#endif

    // DRM, DMA-BUF, sync files, udmabuf, and NVIDIA's device interfaces. Keep unrelated
    // device and terminal controls out of the GPU policy. Individual driver commands
    // remain the driver's responsibility; ioctl type numbers are not unique device IDs.
    static constexpr Array types {
        'd',
        'b',
        '>',
        'u',
        'F',
        'm',
#if defined(__aarch64__)
        // NVIDIA Tegra's nvmap and nvhost interfaces.
        'N',
        'H',
#endif
    };
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_ioctl, 0, 2 * types.size() + 3));
    append(SECCOMP_LOAD_ARGUMENT(1));
    append(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, _IOC_TYPEMASK << _IOC_TYPESHIFT));
    for (auto type : types) {
        append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<u32>(type) << _IOC_TYPESHIFT, 0, 1));
        append(SECCOMP_ALLOW);
    }
    append(SECCOMP_LOAD_SYSCALL_NR);

    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, eventfd2);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, epoll_create1);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, epoll_ctl);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, epoll_wait);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, epoll_pwait);

#ifdef __NR_kcmp
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_kcmp, 0, 1));
    append(SECCOMP_ERRNO(ENOSYS));
#endif
}

void SeccompPolicy::allow_process_metadata()
{
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, getpid);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, gettid);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, getuid);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, geteuid);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, getgid);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, getegid);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, getresgid);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, getresuid);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, getrandom);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, gettimeofday);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, getrlimit);
#ifdef __NR_prlimit64
    // glibc implements getrlimit() with prlimit64(). Only self queries are needed.
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_prlimit64, 0, 10));
    append(SECCOMP_LOAD_ARGUMENT(0));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<u32>(getpid()), 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(2));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 3));
    append(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, args[2]) + sizeof(u32)));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_ERRNO(EPERM));
    append(SECCOMP_LOAD_SYSCALL_NR);
#endif
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, getrusage);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, sched_getaffinity);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, sched_yield);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, getcpu);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, membarrier);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, sysinfo);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, uname);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, umask);

#ifdef __NR_sched_getscheduler
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_sched_getscheduler, 0, 1));
    append(SECCOMP_ALLOW);
#endif
#ifdef __NR_sched_get_priority_max
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_sched_get_priority_max, 0, 1));
    append(SECCOMP_ALLOW);
#endif
#ifdef __NR_sched_get_priority_min
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_sched_get_priority_min, 0, 1));
    append(SECCOMP_ALLOW);
#endif
#ifdef __NR_getpriority
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_getpriority, 0, 1));
    append(SECCOMP_ALLOW);
#endif
#ifdef __NR_setpriority
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_setpriority, 0, 1));
    append(SECCOMP_ERRNO(EPERM));
#endif
#ifdef __NR_sched_setscheduler
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_sched_setscheduler, 0, 1));
    append(SECCOMP_ERRNO(EPERM));
#endif
#ifdef __NR_sched_setparam
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_sched_setparam, 0, 1));
    append(SECCOMP_ERRNO(EPERM));
#endif
#ifdef __NR_sched_setaffinity
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_sched_setaffinity, 0, 1));
    append(SECCOMP_ERRNO(EPERM));
#endif
}

void SeccompPolicy::allow_common_runtime()
{
    allow_memory_without_executable_mappings();
    allow_threads();
    allow_signals();
    allow_clocks();
    allow_process_metadata();
    allow_prctl();
    allow_exit();
    deny_current_directory_queries();
}

void SeccompPolicy::deny_current_directory_queries()
{
#ifdef __NR_getcwd
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_getcwd, 0, 1));
    append(SECCOMP_ERRNO(ENOENT));
#endif
}

void SeccompPolicy::allow_prctl()
{
    // Thread names and libpulse's capability probe are the common runtime operations.
    static constexpr Array<u32, 3> options { PR_GET_NAME, PR_SET_NAME, PR_CAPBSET_READ };
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_prctl, 0, 2 * options.size() + 2));
    append(SECCOMP_LOAD_ARGUMENT(0));
    for (auto option : options) {
        append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, option, 0, 1));
        append(SECCOMP_ALLOW);
    }
    append(SECCOMP_LOAD_SYSCALL_NR);

#if defined(PR_SET_VMA) && defined(PR_SET_VMA_ANON_NAME)
    // glibc and allocators name anonymous memory mappings.
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_prctl, 0, 6));
    append(SECCOMP_LOAD_ARGUMENT(0));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, PR_SET_VMA, 0, 3));
    append(SECCOMP_LOAD_ARGUMENT(1));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, PR_SET_VMA_ANON_NAME, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);
#endif
}

void SeccompPolicy::allow_exit()
{
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, exit);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, exit_group);
}

ErrorOr<void> SeccompPolicy::install()
{
    TRY(install_sigsys_handler());

    // A logged refusal traps, and the kernel kills a process for a trap that arrives while SIGSYS is blocked, as it is
    // in glibc's pthread_create(). The file broker's filter keeps SIGSYS out of every mask, so without the broker this
    // policy has to, ahead of any group that allows rt_sigprocmask.
#ifdef __NR_rt_sigprocmask
    if (sandbox_debugging_is_enabled() && !file_broker_is_active()) {
        static constexpr size_t after_the_syscall_number_load = 4;
        VERIFY(m_filter[after_the_syscall_number_load - 1].k == offsetof(seccomp_data, nr));
        sock_filter const keep_sigsys_unblocked[] = {
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_rt_sigprocmask, 0, 4),
            SECCOMP_LOAD_ARGUMENT(0),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SIG_UNBLOCK, 0, 1),
            SECCOMP_ALLOW,
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP | signal_mask_trap),
        };
        for (size_t i = 0; i < array_size(keep_sigsys_unblocked); ++i)
            MUST(m_filter.try_insert(after_the_syscall_number_load + i, keep_sigsys_unblocked[i]));

        sigset_t trap_signal;
        sigemptyset(&trap_signal);
        sigaddset(&trap_signal, SIGSYS);
        if (auto error = pthread_sigmask(SIG_UNBLOCK, &trap_signal, nullptr); error != 0)
            return Error::from_syscall("pthread_sigmask"sv, error);
    }
#endif

    // The trap handler can't make a system call that traps again, so what it needs to reach the broker has to be allowed
    // outright. A policy that sends nothing there keeps these closed, so that one that pairs no sockets still can't.
    if (m_sends_paths_to_file_broker) {
        SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, sendmsg);
        SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, recvmsg);
        SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, pipe2);
        SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, read);
        SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, write);
        SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, close);
        allow_socket_pairs();
    }

    // A socket domain that no group asked for is refused rather than fatal. Libraries probe
    // transports they can live without; glibc, for one, tries an nscd socket before it falls back
    // to reading files, and killing the process over that would be a stability bug, not security.
#ifdef __NR_socket
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_socket, 0, 1));
    append(SECCOMP_ERRNO(EAFNOSUPPORT));
#endif

#ifdef __NR_sendto
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_sendto, 0, 1));
    append(SECCOMP_ERRNO(EPERM));
#endif

#ifdef __NR_tgkill
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_tgkill, 0, 1));
    append(SECCOMP_ERRNO(EPERM));
#endif

#ifdef __NR_prctl
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_prctl, 0, 1));
    append(SECCOMP_ERRNO(EPERM));
#endif

    // The same goes for a socket option that no group asked for.
#ifdef __NR_setsockopt
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_setsockopt, 0, 1));
    append(SECCOMP_ERRNO(EPERM));
#endif
#ifdef __NR_getsockopt
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_getsockopt, 0, 1));
    append(SECCOMP_ERRNO(EPERM));
#endif

    // So does creating a process, when no group allowed it. NVIDIA's EGL library, for one, tries to start nvidia-modprobe
    // with posix_spawn() when it loads, even on a machine without an NVIDIA GPU. With an error, it gives up and glvnd
    // moves on to the next vendor.
#ifdef __NR_clone
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_clone, 0, 1));
    append(SECCOMP_ERRNO(EPERM));
#endif
#ifdef __NR_fork
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_fork, 0, 1));
    append(SECCOMP_ERRNO(EPERM));
#endif
#ifdef __NR_vfork
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_vfork, 0, 1));
    append(SECCOMP_ERRNO(EPERM));
#endif

    append_kill();

    sock_fprog program {
        .len = static_cast<unsigned short>(m_filter.size()),
        .filter = m_filter.data(),
    };

#ifdef __NR_seccomp
    if (syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, &program) < 0)
        return Error::from_syscall("seccomp(SECCOMP_SET_MODE_FILTER)"sv, errno);
#else
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) < 0)
        return Error::from_syscall("prctl(PR_SET_SECCOMP)"sv, errno);
#endif

    return {};
}

namespace Detail {

// Every architecture numbers the system calls from 424 on alike, so these hold even where the headers predate them.
// A number past the newest one that was checked here could be a new way to reach a path, so it gets ENOSYS. That
// includes the x32 system calls on x86_64.
static constexpr u32 first_unchecked_syscall_number = 472;

ErrorOr<void> install_file_system_filter()
{
    TRY(install_sigsys_handler());
    TRY(install_no_new_privileges());

    // From here on, the filter keeps SIGSYS out of every mask. One that is blocked already has to be let through too.
    sigset_t trap_signal;
    sigemptyset(&trap_signal);
    sigaddset(&trap_signal, SIGSYS);
    if (auto error = pthread_sigmask(SIG_UNBLOCK, &trap_signal, nullptr); error != 0)
        return Error::from_syscall("pthread_sigmask"sv, error);

    struct Answer {
        u32 syscall_number;
        u32 action;
    };
    static constexpr u32 brokered = SECCOMP_RET_TRAP | file_broker_trap;
    static constexpr auto refused = [](int error) { return SECCOMP_RET_ERRNO | static_cast<u32>(error); };

    // System calls that take a path go to the broker. The others are refused: the broker has nothing like them, or they
    // reach files without a path, or into the broker itself.
    static constexpr Answer answers[] = {
#ifdef __NR_open
        { __NR_open, brokered },
#endif
#ifdef __NR_openat
        { __NR_openat, brokered },
#endif
#ifdef __NR_access
        { __NR_access, brokered },
#endif
#ifdef __NR_faccessat
        { __NR_faccessat, brokered },
#endif
#ifdef __NR_faccessat2
        { __NR_faccessat2, brokered },
#endif
#ifdef __NR_stat
        { __NR_stat, brokered },
#endif
#ifdef __NR_newfstatat
        { __NR_newfstatat, brokered },
#endif
#ifdef __NR_readlink
        { __NR_readlink, brokered },
#endif
#ifdef __NR_readlinkat
        { __NR_readlinkat, brokered },
#endif
#ifdef __NR_mkdir
        { __NR_mkdir, brokered },
#endif
#ifdef __NR_mkdirat
        { __NR_mkdirat, brokered },
#endif
#ifdef __NR_unlink
        { __NR_unlink, brokered },
#endif
#ifdef __NR_rmdir
        { __NR_rmdir, brokered },
#endif
#ifdef __NR_unlinkat
        { __NR_unlinkat, brokered },
#endif
#ifdef __NR_rename
        { __NR_rename, brokered },
#endif
#ifdef __NR_renameat
        { __NR_renameat, brokered },
#endif
#ifdef __NR_renameat2
        { __NR_renameat2, brokered },
#endif
#ifdef __NR_bind
        { __NR_bind, brokered },
#endif
#ifdef __NR_lstat
        { __NR_lstat, refused(ENOSYS) },
#endif
#ifdef __NR_statx
        { __NR_statx, refused(ENOSYS) },
#endif
#ifdef __NR_statfs
        { __NR_statfs, refused(ENOSYS) },
#endif
#ifdef __NR_link
        { __NR_link, refused(EPERM) },
#endif
#ifdef __NR_linkat
        { __NR_linkat, refused(EPERM) },
#endif
#ifdef __NR_symlink
        { __NR_symlink, refused(EPERM) },
#endif
#ifdef __NR_symlinkat
        { __NR_symlinkat, refused(EPERM) },
#endif
#ifdef __NR_execve
        { __NR_execve, refused(EACCES) },
#endif
#ifdef __NR_execveat
        { __NR_execveat, refused(EACCES) },
#endif
#ifdef __NR_creat
        { __NR_creat, refused(EACCES) },
#endif
#ifdef __NR_truncate
        { __NR_truncate, refused(EACCES) },
#endif
#ifdef __NR_inotify_add_watch
        { __NR_inotify_add_watch, refused(EACCES) },
#endif
#ifdef __NR_chmod
        { __NR_chmod, refused(EPERM) },
#endif
#ifdef __NR_fchmodat
        { __NR_fchmodat, refused(EPERM) },
#endif
#ifdef __NR_chown
        { __NR_chown, refused(EPERM) },
#endif
#ifdef __NR_lchown
        { __NR_lchown, refused(EPERM) },
#endif
#ifdef __NR_fchownat
        { __NR_fchownat, refused(EPERM) },
#endif
#ifdef __NR_mknod
        { __NR_mknod, refused(EPERM) },
#endif
#ifdef __NR_mknodat
        { __NR_mknodat, refused(EPERM) },
#endif
#ifdef __NR_utime
        { __NR_utime, refused(EPERM) },
#endif
#ifdef __NR_utimes
        { __NR_utimes, refused(EPERM) },
#endif
#ifdef __NR_futimesat
        { __NR_futimesat, refused(EPERM) },
#endif
#ifdef __NR_utimensat
        { __NR_utimensat, refused(EPERM) },
#endif
#ifdef __NR_chdir
        { __NR_chdir, refused(EPERM) },
#endif
#ifdef __NR_fchdir
        { __NR_fchdir, refused(EPERM) },
#endif
#ifdef __NR_chroot
        { __NR_chroot, refused(EPERM) },
#endif
#ifdef __NR_pivot_root
        { __NR_pivot_root, refused(EPERM) },
#endif
#ifdef __NR_mount
        { __NR_mount, refused(EPERM) },
#endif
#ifdef __NR_umount2
        { __NR_umount2, refused(EPERM) },
#endif
#ifdef __NR_swapon
        { __NR_swapon, refused(EPERM) },
#endif
#ifdef __NR_swapoff
        { __NR_swapoff, refused(EPERM) },
#endif
#ifdef __NR_acct
        { __NR_acct, refused(EPERM) },
#endif
#ifdef __NR_quotactl
        { __NR_quotactl, refused(EPERM) },
#endif
#ifdef __NR_uselib
        { __NR_uselib, refused(EPERM) },
#endif
#ifdef __NR_name_to_handle_at
        { __NR_name_to_handle_at, refused(EPERM) },
#endif
#ifdef __NR_open_by_handle_at
        { __NR_open_by_handle_at, refused(EPERM) },
#endif
#ifdef __NR_fanotify_mark
        { __NR_fanotify_mark, refused(EPERM) },
#endif
#ifdef __NR_ptrace
        { __NR_ptrace, refused(EPERM) },
#endif
#ifdef __NR_process_vm_readv
        { __NR_process_vm_readv, refused(EPERM) },
#endif
#ifdef __NR_process_vm_writev
        { __NR_process_vm_writev, refused(EPERM) },
#endif
#ifdef __NR_setxattr
        { __NR_setxattr, refused(EOPNOTSUPP) },
#endif
#ifdef __NR_lsetxattr
        { __NR_lsetxattr, refused(EOPNOTSUPP) },
#endif
#ifdef __NR_getxattr
        { __NR_getxattr, refused(EOPNOTSUPP) },
#endif
#ifdef __NR_lgetxattr
        { __NR_lgetxattr, refused(EOPNOTSUPP) },
#endif
#ifdef __NR_listxattr
        { __NR_listxattr, refused(EOPNOTSUPP) },
#endif
#ifdef __NR_llistxattr
        { __NR_llistxattr, refused(EOPNOTSUPP) },
#endif
#ifdef __NR_removexattr
        { __NR_removexattr, refused(EOPNOTSUPP) },
#endif
#ifdef __NR_lremovexattr
        { __NR_lremovexattr, refused(EOPNOTSUPP) },
#endif
        { 425, refused(ENOSYS) },     // io_uring_setup
        { 426, refused(ENOSYS) },     // io_uring_enter
        { 427, refused(ENOSYS) },     // io_uring_register
        { 428, refused(EPERM) },      // open_tree
        { 429, refused(EPERM) },      // move_mount
        { 430, refused(EPERM) },      // fsopen
        { 431, refused(EPERM) },      // fsconfig
        { 432, refused(EPERM) },      // fsmount
        { 433, refused(EPERM) },      // fspick
        { 437, refused(ENOSYS) },     // openat2
        { 438, refused(EPERM) },      // pidfd_getfd
        { 442, refused(EPERM) },      // mount_setattr
        { 443, refused(EPERM) },      // quotactl_fd
        { 452, refused(EPERM) },      // fchmodat2
        { 463, refused(EOPNOTSUPP) }, // setxattrat
        { 464, refused(EOPNOTSUPP) }, // getxattrat
        { 465, refused(EOPNOTSUPP) }, // listxattrat
        { 466, refused(EOPNOTSUPP) }, // removexattrat
        { 467, refused(EPERM) },      // open_tree_attr
        { 468, refused(EACCES) },     // file_getattr
        { 469, refused(EPERM) },      // file_setattr
    };

    Vector<sock_filter> filter;
    TRY(filter.try_append(SECCOMP_LOAD_ARCHITECTURE));
    TRY(filter.try_append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, audit_architecture, 1, 0)));
    TRY(filter.try_append(SECCOMP_TRAP));
    TRY(filter.try_append(SECCOMP_LOAD_SYSCALL_NR));
    TRY(filter.try_append(BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, first_unchecked_syscall_number, 0, 1)));
    TRY(filter.try_append(SECCOMP_ERRNO(ENOSYS)));
#ifdef __NR_rt_sigprocmask
    // Only a mask that could block SIGSYS needs a look.
    TRY(filter.try_append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_rt_sigprocmask, 0, 4)));
    TRY(filter.try_append(SECCOMP_LOAD_ARGUMENT(0)));
    TRY(filter.try_append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SIG_UNBLOCK, 0, 1)));
    TRY(filter.try_append(SECCOMP_ALLOW));
    TRY(filter.try_append(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP | signal_mask_trap)));
#endif
    auto logs_refusals = sandbox_debugging_is_enabled();
    for (auto const& answer : answers) {
        auto action = answer.action;
        // With sandbox debugging on, a refusal traps so that it can be logged, like refusal_action() does.
        if (logs_refusals && (action & SECCOMP_RET_ACTION_FULL) == SECCOMP_RET_ERRNO)
            action = SECCOMP_RET_TRAP | (logged_refusal_trap + (action & SECCOMP_RET_DATA));
        TRY(filter.try_append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, answer.syscall_number, 0, 1)));
        TRY(filter.try_append(BPF_STMT(BPF_RET | BPF_K, action)));
    }
    TRY(filter.try_append(SECCOMP_ALLOW));

    sock_fprog program {
        .len = static_cast<unsigned short>(filter.size()),
        .filter = filter.data(),
    };
    if (syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, &program) < 0)
        return Error::from_syscall("seccomp(SECCOMP_SET_MODE_FILTER)"sv, errno);
    return {};
}

}

}
