/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <ImageDecoder/Sandbox.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/sched.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#if defined(__GLIBC__)
#    include <malloc.h>
#endif

namespace ImageDecoder {

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

#define SECCOMP_LOAD_SYSCALL_NR BPF_STMT(BPF_LD | BPF_W | BPF_ABS, static_cast<unsigned int>(offsetof(seccomp_data, nr)))
#define SECCOMP_LOAD_ARCHITECTURE BPF_STMT(BPF_LD | BPF_W | BPF_ABS, static_cast<unsigned int>(offsetof(seccomp_data, arch)))
#define SECCOMP_LOAD_ARGUMENT(index) BPF_STMT(BPF_LD | BPF_W | BPF_ABS, static_cast<unsigned int>(offsetof(seccomp_data, args[(index)])))
#define SECCOMP_ALLOW BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)
#define SECCOMP_KILL BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS)
#define SECCOMP_ERRNO(error) BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ((error) & SECCOMP_RET_DATA))
#define SECCOMP_ALLOW_SYSCALL(name) \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_##name, 0, 1), SECCOMP_ALLOW
#define SECCOMP_ALLOW_NOTHING BPF_STMT(BPF_ALU | BPF_ADD | BPF_K, 0)
#define SECCOMP_ALLOW_SYSCALL_IF_DEFINED(name) \
    IF_DEFINED_##name(SECCOMP_ALLOW_SYSCALL(name), SECCOMP_ALLOW_NOTHING)
#define SECCOMP_DENY_READONLY_OPEN_SYSCALL(name, flags_argument_index)                                           \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_##name, 0, 5),                                                      \
        SECCOMP_LOAD_ARGUMENT(flags_argument_index), BPF_STMT(BPF_ALU | BPF_AND | BPF_K, ~read_only_open_flags), \
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1), SECCOMP_ERRNO(EACCES), SECCOMP_LOAD_SYSCALL_NR
#define SECCOMP_ALLOW_THREAD_CLONE_SYSCALL(name)                                                          \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_##name, 0, 8),                                               \
        SECCOMP_LOAD_ARGUMENT(0), BPF_STMT(BPF_ALU | BPF_AND | BPF_K, thread_clone_required_flags),       \
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, thread_clone_required_flags, 0, 4), SECCOMP_LOAD_ARGUMENT(0), \
        BPF_STMT(BPF_ALU | BPF_AND | BPF_K, ~thread_clone_allowed_flags),                                 \
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1), SECCOMP_ALLOW, SECCOMP_LOAD_SYSCALL_NR

#ifdef O_LARGEFILE
static constexpr unsigned read_only_open_flags = O_CLOEXEC | O_LARGEFILE;
#else
static constexpr unsigned read_only_open_flags = O_CLOEXEC;
#endif

#define IF_DEFINED_accept4(if_defined, if_not_defined) if_defined
#define IF_DEFINED_brk(if_defined, if_not_defined) if_defined
#define IF_DEFINED_clock_getres(if_defined, if_not_defined) if_defined
#define IF_DEFINED_clock_gettime(if_defined, if_not_defined) if_defined
#define IF_DEFINED_clock_nanosleep(if_defined, if_not_defined) if_defined
#define IF_DEFINED_clone(if_defined, if_not_defined) if_defined
#define IF_DEFINED_clone3(if_defined, if_not_defined) if_defined
#define IF_DEFINED_close(if_defined, if_not_defined) if_defined
#define IF_DEFINED_dup(if_defined, if_not_defined) if_defined
#define IF_DEFINED_dup3(if_defined, if_not_defined) if_defined
#define IF_DEFINED_exit(if_defined, if_not_defined) if_defined
#define IF_DEFINED_exit_group(if_defined, if_not_defined) if_defined
#define IF_DEFINED_fcntl(if_defined, if_not_defined) if_defined
#define IF_DEFINED_fcntl64(if_defined, if_not_defined) if_defined
#define IF_DEFINED_fstat(if_defined, if_not_defined) if_defined
#define IF_DEFINED_ftruncate(if_defined, if_not_defined) if_defined
#define IF_DEFINED_futex(if_defined, if_not_defined) if_defined
#define IF_DEFINED_futex_time64(if_defined, if_not_defined) if_defined
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
#define IF_DEFINED_lseek(if_defined, if_not_defined) if_defined
#define IF_DEFINED_madvise(if_defined, if_not_defined) if_defined
#define IF_DEFINED_membarrier(if_defined, if_not_defined) if_defined
#define IF_DEFINED_memfd_create(if_defined, if_not_defined) if_defined
#define IF_DEFINED_mmap(if_defined, if_not_defined) if_defined
#define IF_DEFINED_mmap2(if_defined, if_not_defined) if_defined
#define IF_DEFINED_mprotect(if_defined, if_not_defined) if_defined
#define IF_DEFINED_mremap(if_defined, if_not_defined) if_defined
#define IF_DEFINED_munmap(if_defined, if_not_defined) if_defined
#define IF_DEFINED_nanosleep(if_defined, if_not_defined) if_defined
#define IF_DEFINED_pipe2(if_defined, if_not_defined) if_defined
#define IF_DEFINED_poll(if_defined, if_not_defined) if_defined
#define IF_DEFINED_ppoll(if_defined, if_not_defined) if_defined
#define IF_DEFINED_prctl(if_defined, if_not_defined) if_defined
#define IF_DEFINED_prlimit64(if_defined, if_not_defined) if_defined
#define IF_DEFINED_pselect6(if_defined, if_not_defined) if_defined
#define IF_DEFINED_pwrite64(if_defined, if_not_defined) if_defined
#define IF_DEFINED_read(if_defined, if_not_defined) if_defined
#define IF_DEFINED_recvfrom(if_defined, if_not_defined) if_defined
#define IF_DEFINED_recvmmsg(if_defined, if_not_defined) if_defined
#define IF_DEFINED_recvmsg(if_defined, if_not_defined) if_defined
#define IF_DEFINED_restart_syscall(if_defined, if_not_defined) if_defined
#define IF_DEFINED_rseq(if_defined, if_not_defined) if_defined
#define IF_DEFINED_rt_sigaction(if_defined, if_not_defined) if_defined
#define IF_DEFINED_rt_sigprocmask(if_defined, if_not_defined) if_defined
#define IF_DEFINED_rt_sigreturn(if_defined, if_not_defined) if_defined
#define IF_DEFINED_sched_getaffinity(if_defined, if_not_defined) if_defined
#define IF_DEFINED_sched_yield(if_defined, if_not_defined) if_defined
#define IF_DEFINED_sendmmsg(if_defined, if_not_defined) if_defined
#define IF_DEFINED_sendmsg(if_defined, if_not_defined) if_defined
#define IF_DEFINED_sendto(if_defined, if_not_defined) if_defined
#define IF_DEFINED_set_robust_list(if_defined, if_not_defined) if_defined
#define IF_DEFINED_set_tid_address(if_defined, if_not_defined) if_defined
#define IF_DEFINED_setsockopt(if_defined, if_not_defined) if_defined
#define IF_DEFINED_sigaltstack(if_defined, if_not_defined) if_defined
#define IF_DEFINED_socketpair(if_defined, if_not_defined) if_defined
#define IF_DEFINED_sysinfo(if_defined, if_not_defined) if_defined
#define IF_DEFINED_tgkill(if_defined, if_not_defined) if_defined
#define IF_DEFINED_umask(if_defined, if_not_defined) if_defined
#define IF_DEFINED_uname(if_defined, if_not_defined) if_defined
#define IF_DEFINED_write(if_defined, if_not_defined) if_defined

#ifndef __NR_accept4
#    undef IF_DEFINED_accept4
#    define IF_DEFINED_accept4(if_defined, if_not_defined) if_not_defined
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
#ifndef __NR_fcntl64
#    undef IF_DEFINED_fcntl64
#    define IF_DEFINED_fcntl64(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_fstat
#    undef IF_DEFINED_fstat
#    define IF_DEFINED_fstat(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_futex_time64
#    undef IF_DEFINED_futex_time64
#    define IF_DEFINED_futex_time64(if_defined, if_not_defined) if_not_defined
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
#ifndef __NR_mmap2
#    undef IF_DEFINED_mmap2
#    define IF_DEFINED_mmap2(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_pipe2
#    undef IF_DEFINED_pipe2
#    define IF_DEFINED_pipe2(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_poll
#    undef IF_DEFINED_poll
#    define IF_DEFINED_poll(if_defined, if_not_defined) if_not_defined
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
#ifndef __NR_sigaltstack
#    undef IF_DEFINED_sigaltstack
#    define IF_DEFINED_sigaltstack(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_sysinfo
#    undef IF_DEFINED_sysinfo
#    define IF_DEFINED_sysinfo(if_defined, if_not_defined) if_not_defined
#endif
#ifndef __NR_umask
#    undef IF_DEFINED_umask
#    define IF_DEFINED_umask(if_defined, if_not_defined) if_not_defined
#endif

#define SECCOMP_ALLOW_MMAP_WITHOUT_EXEC(name, protection_argument) \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_##name, 0, 5),        \
        SECCOMP_LOAD_ARGUMENT(protection_argument),                \
        BPF_STMT(BPF_ALU | BPF_AND | BPF_K, PROT_EXEC),            \
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1),              \
        SECCOMP_ALLOW,                                             \
        SECCOMP_LOAD_SYSCALL_NR

#define SECCOMP_ALLOW_IOCTL_COMMAND(command)                \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_ioctl, 0, 5),  \
        SECCOMP_LOAD_ARGUMENT(1),                           \
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, command, 0, 1), \
        SECCOMP_ALLOW,                                      \
        SECCOMP_LOAD_SYSCALL_NR,                            \
        BPF_STMT(BPF_ALU | BPF_ADD | BPF_K, 0)

#define SECCOMP_ALLOW_FCNTL_COMMAND(command)                \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_fcntl, 0, 5),  \
        SECCOMP_LOAD_ARGUMENT(1),                           \
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, command, 0, 1), \
        SECCOMP_ALLOW,                                      \
        SECCOMP_LOAD_SYSCALL_NR,                            \
        BPF_STMT(BPF_ALU | BPF_ADD | BPF_K, 0)

ErrorOr<void> install_no_new_privileges()
{
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0)
        return Error::from_syscall("prctl(PR_SET_NO_NEW_PRIVS)"sv, errno);
    return {};
}

ErrorOr<void> configure_runtime_for_sandbox()
{
#if defined(__GLIBC__) && defined(M_ARENA_MAX)
    if (mallopt(M_ARENA_MAX, 4) == 0)
        return Error::from_string_literal("mallopt(M_ARENA_MAX) failed");
#endif
    return {};
}

ErrorOr<void> restrict_filesystem_with_landlock()
{
#if defined(__NR_landlock_create_ruleset) && defined(__NR_landlock_restrict_self)
    auto landlock_abi = syscall(__NR_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (landlock_abi < 0) {
        if (errno == ENOSYS || errno == EOPNOTSUPP || errno == EINVAL)
            return {};
        return Error::from_syscall("landlock_create_ruleset(LANDLOCK_CREATE_RULESET_VERSION)"sv, errno);
    }
    if (landlock_abi == 0)
        return {};

    landlock_ruleset_attr ruleset_attributes {};
    ruleset_attributes.handled_access_fs = LANDLOCK_ACCESS_FS_EXECUTE
        | LANDLOCK_ACCESS_FS_WRITE_FILE
        | LANDLOCK_ACCESS_FS_READ_FILE
        | LANDLOCK_ACCESS_FS_READ_DIR
        | LANDLOCK_ACCESS_FS_REMOVE_DIR
        | LANDLOCK_ACCESS_FS_REMOVE_FILE
        | LANDLOCK_ACCESS_FS_MAKE_CHAR
        | LANDLOCK_ACCESS_FS_MAKE_DIR
        | LANDLOCK_ACCESS_FS_MAKE_REG
        | LANDLOCK_ACCESS_FS_MAKE_SOCK
        | LANDLOCK_ACCESS_FS_MAKE_FIFO
        | LANDLOCK_ACCESS_FS_MAKE_BLOCK
        | LANDLOCK_ACCESS_FS_MAKE_SYM;

#    ifdef LANDLOCK_ACCESS_FS_REFER
    if (landlock_abi >= 2)
        ruleset_attributes.handled_access_fs |= LANDLOCK_ACCESS_FS_REFER;
#    endif
#    ifdef LANDLOCK_ACCESS_FS_TRUNCATE
    if (landlock_abi >= 3)
        ruleset_attributes.handled_access_fs |= LANDLOCK_ACCESS_FS_TRUNCATE;
#    endif
#    if defined(LANDLOCK_ACCESS_NET_BIND_TCP) && defined(LANDLOCK_ACCESS_NET_CONNECT_TCP)
    if (landlock_abi >= 4)
        ruleset_attributes.handled_access_net = LANDLOCK_ACCESS_NET_BIND_TCP | LANDLOCK_ACCESS_NET_CONNECT_TCP;

    auto ruleset_attributes_size = landlock_abi >= 4
        ? sizeof(ruleset_attributes)
        : offsetof(landlock_ruleset_attr, handled_access_net);
#    else
    auto ruleset_attributes_size = sizeof(ruleset_attributes);
#    endif
    auto ruleset_fd = syscall(__NR_landlock_create_ruleset, &ruleset_attributes, ruleset_attributes_size, 0);
    if (ruleset_fd < 0)
        return Error::from_syscall("landlock_create_ruleset"sv, errno);

    ArmedScopeGuard close_ruleset_fd = [&] {
        close(static_cast<int>(ruleset_fd));
    };

    if (syscall(__NR_landlock_restrict_self, ruleset_fd, 0) < 0)
        return Error::from_syscall("landlock_restrict_self"sv, errno);
#endif

    return {};
}

ErrorOr<void> install_seccomp_filter()
{
    sock_filter filter[] = {
        SECCOMP_LOAD_ARCHITECTURE,
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, audit_architecture, 1, 0),
        SECCOMP_KILL,

        SECCOMP_LOAD_SYSCALL_NR,

    // Deny read-only runtime probes without granting filesystem access.
#ifdef __NR_open
        SECCOMP_DENY_READONLY_OPEN_SYSCALL(open, 1),
#endif
#ifdef __NR_openat
        SECCOMP_DENY_READONLY_OPEN_SYSCALL(openat, 2),
#endif

        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(read),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(write),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(close),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(poll),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(ppoll),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(pselect6),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(recvmsg),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(recvfrom),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(recvmmsg),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(sendmsg),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(sendto),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(sendmmsg),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(socketpair),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(getsockopt),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(setsockopt),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(getsockname),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(getpeername),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(fstat),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(dup),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(dup3),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(pipe2),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(ftruncate),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(lseek),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(pwrite64),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(memfd_create),

        SECCOMP_ALLOW_MMAP_WITHOUT_EXEC(mmap, 2),
#ifdef __NR_mmap2
        SECCOMP_ALLOW_MMAP_WITHOUT_EXEC(mmap2, 2),
#endif
        SECCOMP_ALLOW_MMAP_WITHOUT_EXEC(mprotect, 2),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(mremap),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(munmap),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(madvise),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(brk),

        SECCOMP_ALLOW_FCNTL_COMMAND(F_GETFD),
        SECCOMP_ALLOW_FCNTL_COMMAND(F_SETFD),
        SECCOMP_ALLOW_FCNTL_COMMAND(F_GETFL),
        SECCOMP_ALLOW_FCNTL_COMMAND(F_SETFL),
        SECCOMP_ALLOW_IOCTL_COMMAND(FIONBIO),

#ifdef __NR_clone
        SECCOMP_ALLOW_THREAD_CLONE_SYSCALL(clone),
#endif
#ifdef __NR_clone3
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_clone3, 0, 1),
        SECCOMP_ERRNO(ENOSYS),
#endif
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(futex),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(futex_time64),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(set_tid_address),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(set_robust_list),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(rseq),

        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(rt_sigaction),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(rt_sigprocmask),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(rt_sigreturn),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(sigaltstack),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(tgkill),

        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(clock_gettime),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(clock_getres),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(clock_nanosleep),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(nanosleep),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(restart_syscall),

        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(getpid),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(gettid),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(getuid),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(geteuid),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(getgid),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(getegid),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(getrandom),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(gettimeofday),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(getrlimit),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(prlimit64),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(getrusage),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(sched_getaffinity),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(sched_yield),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(getcpu),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(membarrier),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(sysinfo),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(uname),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(umask),

        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(prctl),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(exit),
        SECCOMP_ALLOW_SYSCALL_IF_DEFINED(exit_group),

        SECCOMP_KILL,
    };

    sock_fprog program {
        .len = static_cast<unsigned short>(sizeof(filter) / sizeof(filter[0])),
        .filter = filter,
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

ErrorOr<void> apply_sandbox()
{
    TRY(install_no_new_privileges());
    TRY(configure_runtime_for_sandbox());
    TRY(restrict_filesystem_with_landlock());
    TRY(install_seccomp_filter());
    return {};
}

}
