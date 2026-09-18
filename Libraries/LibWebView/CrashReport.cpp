/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/Hex.h>
#include <AK/StringBuilder.h>
#include <LibCore/CrashReportData.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibWebCommon/Loader/UserAgent.h>
#include <LibWebView/BuildInformation.h>
#include <LibWebView/CrashReport.h>
#include <LibWebView/CrashReportStore.h>
#include <LibWebView/ProcessManager.h>

#if !defined(AK_OS_WINDOWS)
#    include <signal.h>
#    include <sys/utsname.h>
#    include <sys/wait.h>
#    include <unistd.h>
#endif

namespace WebView {

using namespace Core::CrashReportData;

#if !defined(AK_OS_WINDOWS)

ErrorOr<NonnullOwnPtr<CrashReport>> CrashReport::create(ProcessType process_type)
{
    auto pattern = ByteString::formatted("{}/ladybird-crash-XXXXXX", Core::StandardPaths::tempfile_directory());
    auto pattern_buffer = TRY(ByteBuffer::create_zeroed(pattern.length() + 1));
    pattern.bytes().copy_to(pattern_buffer);
    auto fd = TRY(Core::System::mkstemp({ reinterpret_cast<char*>(pattern_buffer.data()), pattern_buffer.size() }));
    // No scratch data survives on disk after the processes close their handles.
    auto file = TRY(Core::File::adopt_fd(fd, Core::File::OpenMode::ReadWrite));
    TRY(Core::System::unlink(StringView { pattern_buffer.bytes().trim(pattern.length()) }));
    TRY(Core::System::set_close_on_exec(fd, true));
    return make<CrashReport>(move(file), process_type);
}

static StringView signal_name(int signal)
{
    switch (signal) {
    case SIGSEGV:
        return "SIGSEGV"sv;
    case SIGBUS:
        return "SIGBUS"sv;
    case SIGFPE:
        return "SIGFPE"sv;
    case SIGABRT:
        return "SIGABRT"sv;
    case SIGILL:
        return "SIGILL"sv;
    case SIGTRAP:
        return "SIGTRAP"sv;
    case SIGKILL:
        return "SIGKILL"sv;
#    ifdef SIGSYS
    case SIGSYS:
        return "SIGSYS"sv;
#    endif
    default:
        return "unknown"sv;
    }
}

ErrorOr<void> CrashReport::save(int wait_status, ByteString const& path)
{
    if ((WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0) || (WIFSIGNALED(wait_status) && (WTERMSIG(wait_status) == SIGTERM || WTERMSIG(wait_status) == SIGKILL)))
        return {};

    StringBuilder builder;
    builder.appendff("Ladybird crash report, format 1\nProcess: {}\n", process_name_from_type(m_process_type));
    builder.appendff("Version: {}\nPlatform: {}\nArchitecture: {}\n", BROWSER_VERSION, OS_STRING, CPU_STRING);
    append_build_information_for_process(builder, m_process_type);
    builder.appendff("Process uptime (seconds): {}\n", (MonotonicTime::now() - m_started_at).to_seconds());
#    ifdef NDEBUG
    builder.append("Build configuration: release\n"sv);
#    else
    builder.append("Build configuration: debug\n"sv);
#    endif
    utsname system {};
    if (uname(&system) == 0) {
        // Only the numeric release, never nodename or custom kernel build text.
        auto release = StringView { system.release, strnlen(system.release, sizeof(system.release)) };
        size_t length = 0;
        while (length < release.length() && (is_ascii_digit(release[length]) || release[length] == '.'))
            ++length;
        builder.appendff("Kernel release: {}\n", release.substring_view(0, length));
    }
    if (WIFSIGNALED(wait_status))
        builder.appendff("Termination signal: {} ({})\n", signal_name(WTERMSIG(wait_status)), WTERMSIG(wait_status));
    else if (WIFEXITED(wait_status))
        builder.appendff("Exit code: {}\n", WEXITSTATUS(wait_status));

    ReportHeader header;
    auto has_header = pread(fd(), &header, sizeof(header), 0) == sizeof(header) && header.magic == report_magic;
    if (has_header) {
        if (header.signal)
            builder.appendff("Captured signal: {} ({})\nSignal code: {}\n", signal_name(header.signal), header.signal, header.code);
        auto const& assertion = header.assertion;
        if ((assertion.kind == 1 || assertion.kind == 2 || assertion.kind == 3) && assertion.length <= assertion.message.size()) {
            builder.append(assertion.kind == 1 ? "Verification failed: "sv : assertion.kind == 2 ? "Assertion failed: "sv
                                                                                                 : "Rust panic: "sv);
            for (u32 i = 0; i < assertion.length; ++i) {
                auto ch = assertion.message[i];
                builder.append(is_ascii_printable(ch) ? ch : '?');
            }
            if (assertion.truncated)
                builder.append(" [truncated]"sv);
            builder.append('\n');
        }
        if (header.executable.size <= header.executable.bytes.size())
            builder.appendff("Executable build ID: {}\n", encode_hex(header.executable.bytes.span().trim(header.executable.size)));
    }
    builder.append("\nNative stack (binary build ID, object address):\n"sv);
    size_t frame_count = 0;
    if (has_header) {
        for (; frame_count < min(header.frame_count, maximum_frames); ++frame_count) {
            ReportFrame frame;
            if (pread(fd(), &frame, sizeof(frame), sizeof(header) + frame_count * sizeof(frame)) != sizeof(frame))
                break;
            if (frame.binary.size > frame.binary.bytes.size() || frame.description_length > frame.description.size()) {
                builder.appendff("#{} unavailable\n", frame_count);
                continue;
            }
            if (frame.binary.size)
                builder.appendff("#{} {} {:#x}", frame_count, encode_hex(frame.binary.bytes.span().trim(frame.binary.size)), frame.address);
            else
                builder.appendff("#{} unavailable", frame_count);
            if (frame.description_length) {
                builder.append(' ');
                for (u32 i = 0; i < frame.description_length; ++i)
                    builder.append(is_ascii_printable(frame.description[i]) ? frame.description[i] : '?');
            } else if (frame.binary.size) {
                auto symbol = symbolicate_frame(frame);
                if (!symbol.is_empty())
                    builder.appendff(" {}", symbol);
            }
            builder.append('\n');
        }
    }
    if (!frame_count)
        builder.append("Unavailable: the process exited without a captured native stack.\n"sv);
    builder.append("\nStacks may be partial.\n"sv);

    return CrashReportStore { path }.store_report(m_process_type, builder.string_view());
}

#else

ErrorOr<NonnullOwnPtr<CrashReport>> CrashReport::create(ProcessType)
{
    return Error::from_string_literal("Crash reports are not supported on Windows yet");
}

ErrorOr<void> CrashReport::save(int, ByteString const&) { return {}; }

#endif

#if !defined(AK_OS_MACOS) && !defined(AK_OS_LINUX)
ByteString CrashReport::symbolicate_frame(ReportFrame const&)
{
    return {};
}

bool CrashReport::is_supported()
{
    return false;
}

#endif

}
