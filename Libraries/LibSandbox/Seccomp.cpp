/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibSandbox/ConnectBroker.h>
#include <LibSandbox/Seccomp.h>
#include <asm/termbits.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/sched.h>
#include <linux/seccomp.h>
#include <linux/sockios.h>
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

static constexpr u32 thread_clone_required_flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD;
static constexpr u32 thread_clone_allowed_flags = thread_clone_required_flags
    | CLONE_SYSVSEM
    | CLONE_SETTLS
    | CLONE_PARENT_SETTID
    | CLONE_CHILD_CLEARTID
    | CLONE_DETACHED
    | CLONE_CHILD_SETTID;
static constexpr u32 fork_clone_flags = CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID | SIGCHLD;

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
#define IF_DEFINED_statx(if_defined, if_not_defined) if_defined
#define IF_DEFINED_symlink(if_defined, if_not_defined) if_defined
#define IF_DEFINED_sysinfo(if_defined, if_not_defined) if_defined
#define IF_DEFINED_tgkill(if_defined, if_not_defined) if_defined
#define IF_DEFINED_unlink(if_defined, if_not_defined) if_defined
#define IF_DEFINED_unlinkat(if_defined, if_not_defined) if_defined
#define IF_DEFINED_umask(if_defined, if_not_defined) if_defined
#define IF_DEFINED_uname(if_defined, if_not_defined) if_defined
#define IF_DEFINED_utimensat(if_defined, if_not_defined) if_defined
#define IF_DEFINED_wait4(if_defined, if_not_defined) if_defined
#define IF_DEFINED_waitid(if_defined, if_not_defined) if_defined
#define IF_DEFINED_write(if_defined, if_not_defined) if_defined
#define IF_DEFINED_writev(if_defined, if_not_defined) if_defined

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
#ifndef __NR_statx
#    undef IF_DEFINED_statx
#    define IF_DEFINED_statx(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_symlink
#    undef IF_DEFINED_symlink
#    define IF_DEFINED_symlink(if_defined, if_not_defined) if_not_defined
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
#ifndef __NR_utimensat
#    undef IF_DEFINED_utimensat
#    define IF_DEFINED_utimensat(if_defined, if_not_defined) if_not_defined
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
#ifdef __NR_faccessat
        CASE_SYSCALL_NAME(faccessat);
#endif
#ifdef __NR_faccessat2
        CASE_SYSCALL_NAME(faccessat2);
#endif
#ifdef __NR_fallocate
        CASE_SYSCALL_NAME(fallocate);
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
#ifdef __NR_fstatfs
        CASE_SYSCALL_NAME(fstatfs);
#endif
#ifdef __NR_futex
        CASE_SYSCALL_NAME(futex);
#endif
#ifdef __NR_getdents64
        CASE_SYSCALL_NAME(getdents64);
#endif
#ifdef __NR_getresgid
        CASE_SYSCALL_NAME(getresgid);
#endif
#ifdef __NR_getresuid
        CASE_SYSCALL_NAME(getresuid);
#endif
#ifdef __NR_ioctl
        CASE_SYSCALL_NAME(ioctl);
#endif
#ifdef __NR_kcmp
        CASE_SYSCALL_NAME(kcmp);
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
#ifdef __NR_stat
        CASE_SYSCALL_NAME(stat);
#endif
#ifdef __NR_statx
        CASE_SYSCALL_NAME(statx);
#endif
#ifdef __NR_symlink
        CASE_SYSCALL_NAME(symlink);
#endif
#ifdef __NR_unlink
        CASE_SYSCALL_NAME(unlink);
#endif
#ifdef __NR_unlinkat
        CASE_SYSCALL_NAME(unlinkat);
#endif
#ifdef __NR_utimensat
        CASE_SYSCALL_NAME(utimensat);
#endif
    default:
        return "unknown";
    }
}

#undef CASE_SYSCALL_NAME

static char s_process_name[16] = "unknown";

// The arguments are copied out before anything is written back, because the result register and the
// first argument register are the same one on some architectures.
struct SyscallRegisters {
    FlatPtr arguments[4];
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
// descriptor the answer carried, zero when it carried none, or a negative errno.
static i64 ask_broker(Detail::ConnectBrokerRequest const& request, int socket_fd, bool expect_descriptor)
{
    if (s_connect_broker_fd < 0)
        return -EACCES;

    int reply_fds[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, reply_fds) < 0)
        return -errno;

    int passed_fds[2] = { reply_fds[1], socket_fd };
    size_t passed_fd_count = socket_fd >= 0 ? 2 : 1;

    iovec request_io {
        .iov_base = const_cast<Detail::ConnectBrokerRequest*>(&request),
        .iov_len = sizeof(request),
    };

    union {
        cmsghdr header;
        char space[CMSG_SPACE(2 * sizeof(int))];
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
        sent = sendmsg(s_connect_broker_fd, &request_message, MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);

    close(reply_fds[1]);

    if (sent < 0) {
        auto saved_errno = errno;
        close(reply_fds[0]);
        return -saved_errno;
    }

    Detail::ConnectBrokerResponse response {};
    iovec response_io {
        .iov_base = &response,
        .iov_len = sizeof(response),
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

    int answered_fd = -1;
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
        written = write(fds[1], source, length);
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

// socket() hands back a real socket that the Browser made. It is inert here, because connect, bind
// and listen are not syscalls this process may make, and it is the caller's own socket, so whatever
// the caller configures on it before connecting still applies afterwards.
static void emulate_socket(void* context)
{
    auto registers = syscall_registers(context);

    Detail::ConnectBrokerRequest request {};
    request.magic = Detail::connect_broker_magic;
    request.operation = static_cast<u32>(Detail::ConnectBrokerOperation::CreateSocket);
    request.socket_domain = static_cast<int>(registers.arguments[0]);
    request.socket_type = static_cast<int>(registers.arguments[1]);
    request.socket_protocol = static_cast<int>(registers.arguments[2]);

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
    }

    write_string("Sandbox violation in ");
    write_string(s_process_name);
    write_string(": disallowed syscall ");
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

    (void)prctl(PR_GET_NAME, s_process_name, 0ul, 0ul, 0ul);
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
#ifdef __NR_access
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_access, 0, 1));
    append(SECCOMP_ERRNO(EACCES));
#endif
#ifdef __NR_faccessat
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_faccessat, 0, 1));
    append(SECCOMP_ERRNO(EACCES));
#endif
#ifdef __NR_faccessat2
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_faccessat2, 0, 1));
    append(SECCOMP_ERRNO(EACCES));
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
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, faccessat);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, faccessat2);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, fstatfs);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, getdents64);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, newfstatat);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, readlink);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, readlinkat);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, stat);
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
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, symlink);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, rmdir);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, fsync);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, fdatasync);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, fallocate);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, fchmod);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, flock);
    // NB: Mesa's shader disk cache updates entry mtimes for LRU eviction, and glibc routes the
    //     whole utime() family through utimensat() on modern kernels.
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, utimensat);

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

    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, execve);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, execveat);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, wait4);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, waitid);
}

// NB: Seccomp cannot follow the sockaddr pointer that connect() is given, and Landlock does not
//     mediate UNIX socket connections, so neither mechanism can restrict which endpoint a process
//     reaches. The domain argument of socket() is the only part of this the kernel lets us filter,
//     and connect() fails with EAFNOSUPPORT when the address family does not match the socket.
//     Being able to create a socket in a domain is therefore the same thing as being able to
//     address every endpoint in it.
void SeccompPolicy::append_allow_socket_with_domains([[maybe_unused]] ReadonlySpan<u32> domains)
{
#ifdef __NR_socket
    // Every jump offset below is a byte, so the group has to stay well short of 255 domains.
    VERIFY(!domains.is_empty());
    VERIFY(domains.size() < 32);

    // Jump past the whole group when this is not socket(). The group is one argument load, one
    // comparison per domain, and the allow.
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_socket, 0, static_cast<u8>(domains.size() + 2)));
    append(SECCOMP_LOAD_ARGUMENT(0));
    for (size_t i = 0; i < domains.size(); ++i) {
        // A matching domain jumps to the allow. The last comparison is the one that has to step
        // over the allow when it does not match.
        auto instructions_to_allow = static_cast<u8>(domains.size() - i - 1);
        auto instructions_past_allow = static_cast<u8>(i + 1 == domains.size() ? 1 : 0);
        append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, domains[i], instructions_to_allow, instructions_past_allow));
    }
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);
#endif
}

// NB: This group covers sockets that the process already has. The Browser mints every Ladybird IPC
//     channel with socketpair() and hands the helper a connected descriptor, so no sandboxed helper
//     needs to create or connect a socket of its own to talk to us.
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
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, getsockopt);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, setsockopt);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, getsockname);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, getpeername);
}

// The caller gets no socket of its own. socket(AF_UNIX) is emulated with a placeholder, connect()
// goes to the Browser, and the Browser connects only to a path it put on its own allowlist. Every
// other domain is refused outright, so this cannot become a way onto the network either.
void SeccompPolicy::broker_unix_socket_connections()
{
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
}

void set_connect_broker_fd(int fd)
{
    s_connect_broker_fd = fd;
}

void SeccompPolicy::allow_network()
{
    // NB: AF_NETLINK is here because glibc reaches for it when it enumerates local interfaces.
    static constexpr Array<u32, 3> domains { AF_INET, AF_INET6, AF_NETLINK };
    append_allow_socket_with_domains(domains);

    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, connect);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, bind);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, listen);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, accept);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, accept4);
    SECCOMP_APPEND_ALLOW_SYSCALL_IF_DEFINED(*this, shutdown);

    // Required by glibc's if_nametoindex() when resolving IPv6 link-local nameserver scopes.
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_ioctl, 0, 5));
    append(SECCOMP_LOAD_ARGUMENT(1));
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SIOCGIFINDEX, 0, 1));
    append(SECCOMP_ALLOW);
    append(SECCOMP_LOAD_SYSCALL_NR);
    append(BPF_STMT(BPF_ALU | BPF_ADD | BPF_K, 0));
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

    // A socket domain that no group asked for is refused rather than fatal. Libraries probe
    // transports they can live without; glibc, for one, tries an nscd socket before it falls back
    // to reading files, and killing the process over that would be a stability bug, not security.
#ifdef __NR_socket
    append(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_socket, 0, 1));
    append(SECCOMP_ERRNO(EAFNOSUPPORT));
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

}
