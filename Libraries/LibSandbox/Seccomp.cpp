/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibSandbox/Seccomp.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/sched.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <stddef.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/ucontext.h>
#include <unistd.h>

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

static constexpr u32 thread_clone_required_flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD;
static constexpr u32 thread_clone_allowed_flags = thread_clone_required_flags
    | CLONE_SYSVSEM
    | CLONE_SETTLS
    | CLONE_PARENT_SETTID
    | CLONE_CHILD_CLEARTID
    | CLONE_DETACHED
    | CLONE_CHILD_SETTID;
static constexpr u32 vfork_clone_flags = CLONE_VM | CLONE_VFORK | SIGCHLD;

#ifdef O_LARGEFILE
static constexpr unsigned read_only_open_flags = O_CLOEXEC | O_LARGEFILE;
#else
static constexpr unsigned read_only_open_flags = O_CLOEXEC;
#endif

#define SECCOMP_LOAD_SYSCALL_NR BPF_STMT(BPF_LD | BPF_W | BPF_ABS, static_cast<unsigned int>(offsetof(seccomp_data, nr)))
#define SECCOMP_LOAD_ARCHITECTURE BPF_STMT(BPF_LD | BPF_W | BPF_ABS, static_cast<unsigned int>(offsetof(seccomp_data, arch)))
#define SECCOMP_LOAD_ARGUMENT(index) BPF_STMT(BPF_LD | BPF_W | BPF_ABS, static_cast<unsigned int>(offsetof(seccomp_data, args[(index)])))
#define SECCOMP_ALLOW BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)
#define SECCOMP_TRAP BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP)
#define SECCOMP_ERRNO(error) BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ((error) & SECCOMP_RET_DATA))
#define SECCOMP_ALLOW_NOTHING BPF_STMT(BPF_ALU | BPF_ADD | BPF_K, 0)
#define SECCOMP_APPEND_ALLOW_SYSCALL(policy, name)                               \
    do {                                                                         \
        (policy).append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_##name, 0, 1)); \
        (policy).append(SECCOMP_ALLOW);                                          \
    } while (0)
#define SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(policy, name) \
    IF_DEFINED_##name(SECCOMP_APPEND_ALLOW_SYSCALL(policy, name), (policy).append(SECCOMP_ALLOW_NOTHING))

#define IF_DEFINED_accept(if_defined, if_not_defined) if_defined
#define IF_DEFINED_accept4(if_defined, if_not_defined) if_defined
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
#define IF_DEFINED_execve(if_defined, if_not_defined) if_defined
#define IF_DEFINED_execveat(if_defined, if_not_defined) if_defined
#define IF_DEFINED_exit(if_defined, if_not_defined) if_defined
#define IF_DEFINED_exit_group(if_defined, if_not_defined) if_defined
#define IF_DEFINED_fcntl(if_defined, if_not_defined) if_defined
#define IF_DEFINED_fcntl64(if_defined, if_not_defined) if_defined
#define IF_DEFINED_fdatasync(if_defined, if_not_defined) if_defined
#define IF_DEFINED_flock(if_defined, if_not_defined) if_defined
#define IF_DEFINED_fstat(if_defined, if_not_defined) if_defined
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
#define IF_DEFINED_listen(if_defined, if_not_defined) if_defined
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
#define IF_DEFINED_statx(if_defined, if_not_defined) if_defined
#define IF_DEFINED_sysinfo(if_defined, if_not_defined) if_defined
#define IF_DEFINED_tgkill(if_defined, if_not_defined) if_defined
#define IF_DEFINED_unlink(if_defined, if_not_defined) if_defined
#define IF_DEFINED_unlinkat(if_defined, if_not_defined) if_defined
#define IF_DEFINED_umask(if_defined, if_not_defined) if_defined
#define IF_DEFINED_uname(if_defined, if_not_defined) if_defined
#define IF_DEFINED_wait4(if_defined, if_not_defined) if_defined
#define IF_DEFINED_waitid(if_defined, if_not_defined) if_defined
#define IF_DEFINED_write(if_defined, if_not_defined) if_defined

#ifndef __NR_accept
#    undef IF_DEFINED_accept
#    define IF_DEFINED_accept(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_accept4
#    undef IF_DEFINED_accept4
#    define IF_DEFINED_accept4(if_defined, if_not_defined) if_not_defined
#endif
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
#ifndef __NR_execve
#    undef IF_DEFINED_execve
#    define IF_DEFINED_execve(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_execveat
#    undef IF_DEFINED_execveat
#    define IF_DEFINED_execveat(if_defined, if_not_defined) if_not_defined
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
#ifndef __NR_listen
#    undef IF_DEFINED_listen
#    define IF_DEFINED_listen(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_getcpu
#    undef IF_DEFINED_getcpu
#    define IF_DEFINED_getcpu(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_getrandom
#    undef IF_DEFINED_getrandom
#    define IF_DEFINED_getrandom(if_defined, if_not_defined) if_not_defined
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
#ifndef __NR_statx
#    undef IF_DEFINED_statx
#    define IF_DEFINED_statx(if_defined, if_not_defined) if_not_defined
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

static void write_bytes(char const* string, size_t length)
{
    while (length > 0) {
        auto written = write(STDERR_FILENO, string, length);
        if (written <= 0)
            return;
        string += written;
        length -= static_cast<size_t>(written);
    }
}

static void write_string(char const* string)
{
    write_bytes(string, safe_string_length(string));
}

static void write_unsigned(u64 value)
{
    char buffer[20];
    size_t index = sizeof(buffer);
    do {
        buffer[--index] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value > 0);
    write_bytes(buffer + index, sizeof(buffer) - index);
}

static void write_hex(u64 value)
{
    constexpr char const* digits = "0123456789abcdef";
    char buffer[16];
    size_t index = sizeof(buffer);
    do {
        buffer[--index] = digits[value & 0xf];
        value >>= 4;
    } while (value > 0);
    write_string("0x");
    write_bytes(buffer + index, sizeof(buffer) - index);
}

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
#ifdef __NR_bind
        CASE_SYSCALL_NAME(bind);
#endif
#ifdef __NR_clone
        CASE_SYSCALL_NAME(clone);
#endif
#ifdef __NR_clone3
        CASE_SYSCALL_NAME(clone3);
#endif
#ifdef __NR_connect
        CASE_SYSCALL_NAME(connect);
#endif
#ifdef __NR_execve
        CASE_SYSCALL_NAME(execve);
#endif
#ifdef __NR_execveat
        CASE_SYSCALL_NAME(execveat);
#endif
#ifdef __NR_fcntl
        CASE_SYSCALL_NAME(fcntl);
#endif
#ifdef __NR_fcntl64
        CASE_SYSCALL_NAME(fcntl64);
#endif
#ifdef __NR_fstat
        CASE_SYSCALL_NAME(fstat);
#endif
#ifdef __NR_futex
        CASE_SYSCALL_NAME(futex);
#endif
#ifdef __NR_getdents64
        CASE_SYSCALL_NAME(getdents64);
#endif
#ifdef __NR_ioctl
        CASE_SYSCALL_NAME(ioctl);
#endif
#ifdef __NR_link
        CASE_SYSCALL_NAME(link);
#endif
#ifdef __NR_linkat
        CASE_SYSCALL_NAME(linkat);
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
#ifdef __NR_mmap
        CASE_SYSCALL_NAME(mmap);
#endif
#ifdef __NR_mmap2
        CASE_SYSCALL_NAME(mmap2);
#endif
#ifdef __NR_mprotect
        CASE_SYSCALL_NAME(mprotect);
#endif
#ifdef __NR_newfstatat
        CASE_SYSCALL_NAME(newfstatat);
#endif
#ifdef __NR_open
        CASE_SYSCALL_NAME(open);
#endif
#ifdef __NR_openat
        CASE_SYSCALL_NAME(openat);
#endif
#ifdef __NR_prctl
        CASE_SYSCALL_NAME(prctl);
#endif
#ifdef __NR_readlink
        CASE_SYSCALL_NAME(readlink);
#endif
#ifdef __NR_readlinkat
        CASE_SYSCALL_NAME(readlinkat);
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
#ifdef __NR_rmdir
        CASE_SYSCALL_NAME(rmdir);
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
#ifdef __NR_seccomp
        CASE_SYSCALL_NAME(seccomp);
#endif
#ifdef __NR_socket
        CASE_SYSCALL_NAME(socket);
#endif
#ifdef __NR_statx
        CASE_SYSCALL_NAME(statx);
#endif
#ifdef __NR_unlink
        CASE_SYSCALL_NAME(unlink);
#endif
#ifdef __NR_unlinkat
        CASE_SYSCALL_NAME(unlinkat);
#endif
    default:
        return "unknown";
    }
}

#undef CASE_SYSCALL_NAME

static void handle_sigsys(int, siginfo_t* info, void* context)
{
    write_string("Sandbox violation: disallowed syscall ");
    write_string(syscall_name(info->si_syscall));
    write_string(" (");
    write_unsigned(static_cast<u64>(info->si_syscall));
    write_string("), architecture ");
    write_hex(info->si_arch);
    write_string(", instruction pointer ");
    write_hex(reinterpret_cast<FlatPtr>(info->si_call_addr));

#if defined(__x86_64__)
    auto* machine_context = &static_cast<ucontext_t*>(context)->uc_mcontext;
    write_string(", args [");
    write_hex(static_cast<u64>(machine_context->gregs[REG_RDI]));
    write_string(", ");
    write_hex(static_cast<u64>(machine_context->gregs[REG_RSI]));
    write_string(", ");
    write_hex(static_cast<u64>(machine_context->gregs[REG_RDX]));
    write_string(", ");
    write_hex(static_cast<u64>(machine_context->gregs[REG_R10]));
    write_string(", ");
    write_hex(static_cast<u64>(machine_context->gregs[REG_R8]));
    write_string(", ");
    write_hex(static_cast<u64>(machine_context->gregs[REG_R9]));
    write_string("]");
#elif defined(__aarch64__)
    auto* machine_context = &static_cast<ucontext_t*>(context)->uc_mcontext;
    write_string(", args [");
    for (size_t i = 0; i < 6; ++i) {
        if (i != 0)
            write_string(", ");
        write_hex(static_cast<u64>(machine_context->regs[i]));
    }
    write_string("]");
#endif

    write_string("\n");

    struct sigaction action {};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSYS, &action, nullptr);
    raise(SIGSYS);
    _exit(128 + SIGSYS);
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
    return {};
}

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
    append(SECCOMP_ERRNO(EACCES));
    append(SECCOMP_LOAD_SYSCALL_NR);
#endif
#ifdef __NR_openat
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_openat, 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(2));
    append(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, ~read_only_open_flags));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1));
    append(SECCOMP_ERRNO(EACCES));
    append(SECCOMP_LOAD_SYSCALL_NR);
#endif
}

void SeccompPolicy::allow_readonly_file_opens()
{
#ifdef __NR_open
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_open, 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(1));
    append(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, ~read_only_open_flags));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);
#endif
#ifdef __NR_openat
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_openat, 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(2));
    append(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, ~read_only_open_flags));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);
#endif
}

void SeccompPolicy::allow_filesystem_metadata_queries()
{
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, access);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, getdents64);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, newfstatat);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, readlink);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, readlinkat);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, statx);
}

void SeccompPolicy::allow_filesystem_writes()
{
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, open);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, openat);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, unlink);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, unlinkat);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, link);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, linkat);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, rename);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, renameat);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, renameat2);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, mkdir);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, mkdirat);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, rmdir);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, fsync);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, fdatasync);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, flock);

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
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, write);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, close);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, fstat);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, dup);
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
}

void SeccompPolicy::allow_process_creation()
{
#ifdef __NR_clone
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_clone, 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(0));
    append(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, ~vfork_clone_flags));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);
#endif

    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, execve);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, execveat);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, wait4);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, waitid);
}

void SeccompPolicy::allow_ipc()
{
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, eventfd2);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, poll);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, ppoll);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, pselect6);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, recvmsg);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, recvfrom);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, recvmmsg);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, sendmsg);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, sendto);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, sendmmsg);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, socketpair);
#ifdef __NR_socket
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_socket, 0, 3));
    append(SECCOMP_LOAD_ARGUMENT(0));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_UNIX, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);
#endif
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, connect);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, getsockopt);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, setsockopt);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, getsockname);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, getpeername);
}

void SeccompPolicy::allow_network()
{
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, socket);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, connect);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, bind);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, listen);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, accept);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, accept4);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, shutdown);
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
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, tgkill);
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
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, ioctl);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, eventfd2);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, epoll_create1);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, epoll_ctl);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, epoll_wait);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, epoll_pwait);
}

void SeccompPolicy::allow_process_metadata()
{
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, getpid);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, gettid);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, getuid);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, geteuid);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, getgid);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, getegid);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, getrandom);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, gettimeofday);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, getrlimit);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, prlimit64);
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
}

void SeccompPolicy::allow_prctl()
{
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, prctl);
}

void SeccompPolicy::allow_exit()
{
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, exit);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, exit_group);
}

ErrorOr<void> SeccompPolicy::install()
{
    TRY(install_sigsys_handler());

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

}
