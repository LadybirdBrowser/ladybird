/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AssertionFailure.h>
#include <LibCore/CrashHandler.h>
#include <LibCore/System.h>
#if defined(AK_OS_MACOS)
#    include <LibCore/CrashReportDataMacOS.h>
#    include <mach/mach.h>
#    include <mach/mach_vm.h>
#else
#    include <LibCore/CrashReportDataLinux.h>
#    include <fcntl.h>
#    include <sys/syscall.h>
#endif
#include <signal.h>
#include <sys/ucontext.h>
#include <unistd.h>

namespace Core {

using namespace CrashReportData;

// Immutable after handler installation. Unknown/JIT/late-loaded code is reported
// as unavailable, never as an arbitrary address that could contain page data.
static Array<Image, 2048> s_images;
static size_t s_image_count;
static BinaryID s_executable;
static int s_report_fd = -1;
#if defined(AK_OS_LINUX)
static int s_memory_fd = -1;
#endif
static int s_crashing;
static int s_assertion_state;
static Assertion s_assertion;
static int s_backtrace_state;
static u32 s_backtrace_frame_count;
static u32 s_backtrace_generation;
static thread_local bool s_owns_assertion;
static thread_local u32 s_captured_backtrace_generation;
static thread_local bool s_writing_backtrace;
alignas(16) static Array<u8, 64 * 1024> s_signal_stack;

#if defined(AK_OS_MACOS)
static void collect_images()
{
    for (u32 i = 0; i < _dyld_image_count() && s_image_count < s_images.size(); ++i) {
        auto image = image_at_index(i);
        if (i == 0)
            s_executable = image.binary;
        if (image.end > image.start)
            s_images[s_image_count++] = image;
    }
}

#else
static int collect_image(dl_phdr_info* info, size_t, void*)
{
    if (s_image_count >= s_images.size())
        return 1;
    auto image = image_from_phdr_info(*info);
    if (s_image_count == 0)
        s_executable = image.binary;
    if (image.end > image.start)
        s_images[s_image_count++] = image;
    return 0;
}

static void collect_images()
{
    dl_iterate_phdr(collect_image, nullptr);
}
#endif

static bool write_record(void const* data, size_t size, off_t offset)
{
    auto const* bytes = static_cast<u8 const*>(data);
    while (size) {
        auto written = pwrite(s_report_fd, bytes, size, offset);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return false;
        bytes += written;
        size -= written;
        offset += written;
    }
    return true;
}

static void capture_assertion(AK::AssertionFailureKind kind, char const* message)
{
    s_owns_assertion = false;
    // Keep the first fatal assertion, including when reporting recursively fails.
    int expected = 0;
    if (!__atomic_compare_exchange_n(&s_assertion_state, &expected, 1, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        return;

    s_owns_assertion = true;
    s_assertion = sanitize_assertion(kind, message);

    // Persist before stderr formatting and symbolization, which may themselves
    // crash or fail. Publish the immutable record to the signal handler last.
    ReportHeader header;
    header.executable = s_executable;
    header.assertion = s_assertion;
    write_record(&header, sizeof(header), 0);
    __atomic_store_n(&s_assertion_state, 2, __ATOMIC_RELEASE);
}

static bool read_stack_frame(FlatPtr address, FlatPtr (&frame)[2])
{
#if defined(AK_OS_MACOS)
    mach_vm_size_t size = 0;
    return mach_vm_read_overwrite(mach_task_self(), address, sizeof(frame), reinterpret_cast<mach_vm_address_t>(frame), &size) == KERN_SUCCESS && size == sizeof(frame);
#else
    return pread(s_memory_fd, frame, sizeof(frame), address) == sizeof(frame);
#endif
}

#if defined(AK_OS_MACOS) && ARCH(AARCH64)
static FlatPtr strip_return_address(FlatPtr address)
{
    register FlatPtr link_register asm("x30") = address;
    asm("xpaclri" : "+r"(link_register));
    return link_register;
}
#endif

static ReportFrame frame_for_address(FlatPtr address)
{
    ReportFrame frame;
    for (size_t i = 0; i < s_image_count; ++i) {
        auto const& image = s_images[i];
        if (address >= image.start && address < image.end) {
            frame.binary = image.binary;
            frame.address = address - image.relocation;
            break;
        }
    }
    return frame;
}

static void capture_assertion_backtrace(ReadonlySpan<AK::AssertionBacktraceFrame> frames)
{
    // Without an assertion of its own, a thread captures the backtrace of a Rust panic that may still turn out to be
    // caught. That backtrace stays provisional (state 3) and gives way to any later one.
    bool provisional = !s_owns_assertion;
    if (frames.is_empty() || (provisional && !AK::current_rust_panic_message()))
        return;
    int expected = 0;
    if (!__atomic_compare_exchange_n(&s_backtrace_state, &expected, 1, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        expected = 3;
        if (!__atomic_compare_exchange_n(&s_backtrace_state, &expected, 1, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return;
    }
    s_writing_backtrace = true;
    auto count = min(frames.size(), maximum_frames);
    for (size_t i = 0; i < count; ++i) {
        auto frame = frame_for_address(frames[i].address);
        if (frame.binary.size)
            describe_backtrace_frame(frame, frames[i]);
        if (!write_record(&frame, sizeof(frame), sizeof(ReportHeader) + i * sizeof(frame))) {
            __atomic_store_n(&s_backtrace_state, 0, __ATOMIC_RELEASE);
            s_writing_backtrace = false;
            return;
        }
    }
    s_backtrace_frame_count = count;
    if (provisional) {
        s_captured_backtrace_generation = __atomic_add_fetch(&s_backtrace_generation, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&s_backtrace_state, 3, __ATOMIC_RELEASE);
        s_writing_backtrace = false;
        return;
    }
    write_record(&s_backtrace_frame_count, sizeof(s_backtrace_frame_count), offsetof(ReportHeader, frame_count));
    // Only publish a complete trace. Signal capture remains available if resolution fails.
    __atomic_store_n(&s_backtrace_state, 2, __ATOMIC_RELEASE);
    s_writing_backtrace = false;
}

static u32 capture_native_stack(void* context)
{
    FlatPtr program_counter = 0;
    FlatPtr frame_pointer = 0;
#if defined(AK_OS_LINUX)
    FlatPtr stack_pointer = 0;
#endif
#if defined(AK_OS_MACOS) && ARCH(AARCH64)
    auto const& registers = static_cast<ucontext_t*>(context)->uc_mcontext->__ss;
    program_counter = registers.__pc;
    frame_pointer = registers.__fp;
#elif defined(AK_OS_MACOS) && ARCH(X86_64)
    auto const& registers = static_cast<ucontext_t*>(context)->uc_mcontext->__ss;
    program_counter = registers.__rip;
    frame_pointer = registers.__rbp;
#elif defined(AK_OS_LINUX) && ARCH(X86_64)
    auto const& registers = static_cast<ucontext_t*>(context)->uc_mcontext;
    program_counter = registers.gregs[REG_RIP];
    frame_pointer = registers.gregs[REG_RBP];
    stack_pointer = registers.gregs[REG_RSP];
#elif defined(AK_OS_LINUX) && ARCH(AARCH64)
    auto const& registers = static_cast<ucontext_t*>(context)->uc_mcontext;
    program_counter = registers.pc;
    frame_pointer = registers.regs[29];
    stack_pointer = registers.sp;
#elif defined(AK_OS_LINUX) && ARCH(RISCV64)
    auto const& registers = static_cast<ucontext_t*>(context)->uc_mcontext;
    program_counter = registers.__gregs[REG_PC];
    frame_pointer = registers.__gregs[REG_S0];
    stack_pointer = registers.__gregs[REG_SP];
#else
    (void)context;
#endif

    // On RISC-V, the frame pointer points just past the saved frame pointer and return address.
    constexpr FlatPtr saved_frame_offset = ARCH(RISCV64) ? 2 * sizeof(FlatPtr) : 0;

    u32 frame_count = 0;
    [[maybe_unused]] bool has_binary_identity = false;
    for (; frame_count < maximum_frames && program_counter;) {
        auto frame = frame_for_address(program_counter);
        if (!write_record(&frame, sizeof(frame), sizeof(ReportHeader) + frame_count * sizeof(frame)))
            break;
        has_binary_identity |= frame.binary.size != 0;
        ++frame_count;
        FlatPtr saved_frame[2];
        if (!frame_pointer || frame_pointer % alignof(FlatPtr) || !read_stack_frame(frame_pointer - saved_frame_offset, saved_frame))
            break;
        // Stacks grow downwards on the supported architectures. Bound both the
        // depth and each step, and stop at corrupt/cyclic frame chains.
        if (saved_frame[0] <= frame_pointer || saved_frame[0] - frame_pointer > 8 * 1024 * 1024)
            break;
        frame_pointer = saved_frame[0];
        program_counter = saved_frame[1];
#if defined(AK_OS_MACOS) && ARCH(AARCH64)
        // Return addresses in system libraries may be pointer-authenticated.
        program_counter = strip_return_address(program_counter);
#endif
        if (program_counter)
            --program_counter; // Attribute return addresses to their call sites.
    }

#if defined(AK_OS_LINUX)
    // Without a usable frame chain, retain bounded candidate code addresses from the stack.
    if ((frame_count < 2 || !has_binary_identity) && stack_pointer && stack_pointer % alignof(FlatPtr) == 0) {
        Array<FlatPtr, 1024> stack_words;
        auto bytes_read = pread(s_memory_fd, stack_words.data(), sizeof(stack_words), stack_pointer);
        if (bytes_read >= static_cast<ssize_t>(sizeof(FlatPtr))) {
            auto words_read = static_cast<size_t>(bytes_read) / sizeof(FlatPtr);
            for (auto address : stack_words.span().slice(0, words_read)) {
                auto frame = frame_for_address(address);
                if (frame.binary.size == 0)
                    continue;
                // A full chain of unavailable frames must still leave room for binary identity.
                auto index = min(static_cast<size_t>(frame_count), maximum_frames - 1);
                if (!write_record(&frame, sizeof(frame), sizeof(ReportHeader) + index * sizeof(frame)))
                    break;
                frame_count = index + 1;
                if (frame_count == maximum_frames)
                    break;
            }
        }
    }

#endif
    return frame_count;
}

void CrashHandler::handle_fatal_signal(int signal, siginfo_t* info, void* context)
{
    if (__atomic_exchange_n(&s_crashing, 1, __ATOMIC_RELAXED))
        _exit(128 + signal);

    ReportHeader header { report_magic, signal, info ? info->si_code : 0, s_executable, {} };
    if (__atomic_load_n(&s_assertion_state, __ATOMIC_ACQUIRE) == 2)
        header.assertion = s_assertion;
    else if (auto const* message = AK::current_rust_panic_message())
        header.assertion = sanitize_assertion(AK::AssertionFailureKind::RustPanic, message);
    // Persist the reason first, even if unwinding is impossible or interrupted.
    write_record(&header, sizeof(header), 0);

    // Claim the backtrace, so that no capture on another thread replaces its frames while this handler reads them or
    // writes its own. A provisional backtrace only describes this crash if this thread captured it and is still
    // panicking. While another thread writes a backtrace, leave the frames out instead of writing over them; if this
    // thread crashed while writing one, nothing else writes, so capture the native stack.
    auto backtrace_state = __atomic_load_n(&s_backtrace_state, __ATOMIC_ACQUIRE);
    while (backtrace_state == 0 || backtrace_state == 3) {
        if (__atomic_compare_exchange_n(&s_backtrace_state, &backtrace_state, 2, false, __ATOMIC_ACQ_REL,
                __ATOMIC_ACQUIRE))
            break;
    }
    auto owns_provisional_backtrace = backtrace_state == 3 && AK::current_rust_panic_message()
        && s_captured_backtrace_generation == __atomic_load_n(&s_backtrace_generation, __ATOMIC_RELAXED);
    u32 frame_count = 0;
    if (backtrace_state == 2 || owns_provisional_backtrace)
        frame_count = s_backtrace_frame_count;
    else if (backtrace_state != 1 || s_writing_backtrace)
        frame_count = capture_native_stack(context);
    write_record(&frame_count, sizeof(frame_count), offsetof(ReportHeader, frame_count));

    // Preserve signal termination and the operating system's crash handling.
    struct sigaction action {};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    sigaction(signal, &action, nullptr);
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, signal);
    sigprocmask(SIG_UNBLOCK, &signals, nullptr);
#if defined(AK_OS_LINUX)
    syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), signal);
#else
    // Target the crashing thread before reaching the fallback exit.
    raise(signal);
#endif
    _exit(128 + signal);
}

ErrorOr<void> CrashHandler::initialize(int fd)
{
    TRY(Core::System::set_close_on_exec(fd, true));
    s_report_fd = fd;
#if defined(AK_OS_LINUX)
    // Open before sandboxing so stack reads remain available in helper processes.
    s_memory_fd = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
    if (s_memory_fd < 0)
        return Error::from_errno(errno);
#endif
    collect_images();
    // Resolve the memory-read and file-write entry points before a crash can
    // encounter lazy binding while the dynamic loader's locks are held.
    FlatPtr frame[2] {};
    if (!read_stack_frame(reinterpret_cast<FlatPtr>(&frame), frame))
        return Error::from_string_literal("Could not prepare crash stack capture");
    if (pwrite(fd, frame, 0, 0) < 0)
        return Error::from_errno(errno);
    // Alternate stacks are per-thread. This protects main-thread stack overflow;
    // other threads still get the bounded handler when their stack is usable.
    stack_t stack {};
    stack.ss_sp = s_signal_stack.data();
    stack.ss_size = s_signal_stack.size();
    if (sigaltstack(&stack, nullptr) < 0)
        return Error::from_errno(errno);
    struct sigaction action {};
    action.sa_sigaction = CrashHandler::handle_fatal_signal;
    sigfillset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    for (int signal : { SIGSEGV, SIGBUS, SIGFPE, SIGABRT, SIGILL, SIGTRAP }) {
        if (sigaction(signal, &action, nullptr) < 0)
            return Error::from_errno(errno);
    }
    AK::set_assertion_failure_callback(capture_assertion);
    AK::set_assertion_backtrace_callback(capture_assertion_backtrace);
    return {};
}

}
