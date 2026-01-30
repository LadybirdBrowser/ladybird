/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AudioServer/AudioServerConnection.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Process.h>
#include <LibCore/System.h>
#include <LibCore/SystemServerTakeover.h>
#include <LibThreading/Thread.h>
#if defined(AK_OS_MACOS)
#    include <LibCore/Platform/ProcessStatisticsMach.h>
#endif

#include <AK/ByteBuffer.h>
#include <AK/CharacterTypes.h>
#include <AK/OwnPtr.h>

namespace {

class SgrSequenceStripper {
public:
    ByteBuffer strip(ReadonlyBytes input)
    {
        ByteBuffer output;
        output.ensure_capacity(input.size());

        for (u8 byte : input) {
            if (m_pending.is_empty()) {
                if (byte == 0x1b) {
                    m_pending.append(byte);
                    continue;
                }
                output.append(byte);
                continue;
            }

            m_pending.append(byte);

            if (m_pending.size() == 2) {
                // We only strip CSI ... m sequences.
                if (m_pending[1] != '[') {
                    output.append(m_pending);
                    m_pending.clear();
                }
                continue;
            }

            // We have ESC[... in pending.
            if (byte == 'm') {
                // Verify the CSI params are digits/; only.
                bool valid = true;
                for (size_t i = 2; i + 1 < m_pending.size(); ++i) {
                    u8 c = m_pending[i];
                    if (!is_ascii_digit(c) && c != ';') {
                        valid = false;
                        break;
                    }
                }
                if (valid) {
                    m_pending.clear();
                } else {
                    output.append(m_pending);
                    m_pending.clear();
                }
                continue;
            }

            if (!is_ascii_digit(byte) && byte != ';') {
                output.append(m_pending);
                m_pending.clear();
            }
        }

        return output;
    }

    ByteBuffer flush()
    {
        ByteBuffer output;
        if (!m_pending.is_empty()) {
            output = move(m_pending);
            m_pending.clear();
        }
        return output;
    }

private:
    ByteBuffer m_pending;
};

}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    AK::set_rich_debug_enabled(true);

    if (auto dump_path = Core::Environment::get("AUDIO_SERVER_STDERR_DUMP"sv); dump_path.has_value() && !dump_path->is_empty()) {
        auto dump_fd_or_error = Core::System::open(*dump_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (dump_fd_or_error.is_error()) {
            warnln("AudioServer: Failed to open AUDIO_SERVER_STDERR_DUMP={} ({})", *dump_path, dump_fd_or_error.error());
        } else {
            int dump_fd = dump_fd_or_error.release_value();

            auto original_stderr_fd_or_error = Core::System::dup(STDERR_FILENO);
            if (original_stderr_fd_or_error.is_error()) {
                warnln("AudioServer: Failed to dup stderr ({})", original_stderr_fd_or_error.error());
                TRY(Core::System::close(dump_fd));
                goto after_stderr_dump_setup;
            }
            int original_stderr_fd = original_stderr_fd_or_error.release_value();

            auto pipe_fds_or_error = Core::System::pipe2(O_CLOEXEC);
            if (pipe_fds_or_error.is_error()) {
                warnln("AudioServer: Failed to create stderr dump pipe ({})", pipe_fds_or_error.error());
                TRY(Core::System::close(original_stderr_fd));
                TRY(Core::System::close(dump_fd));
            } else {
                int read_fd = pipe_fds_or_error.value()[0];
                int write_fd = pipe_fds_or_error.value()[1];

                static RefPtr<Threading::Thread> s_stderr_dump_thread;
                s_stderr_dump_thread = Threading::Thread::construct("AudioServerStderrDump"sv, [read_fd, dump_fd, original_stderr_fd] {
                    SgrSequenceStripper stripper;

                    for (;;) {
                        Array<u8, 8192> buffer;
                        auto nread_or_error = Core::System::read(read_fd, buffer);
                        if (nread_or_error.is_error())
                            break;
                        size_t nread = nread_or_error.value();
                        if (nread == 0)
                            break;

                        (void)Core::System::write(original_stderr_fd, buffer.span().trim(nread));

                        auto filtered = stripper.strip(buffer.span().trim(nread));
                        if (!filtered.is_empty())
                            (void)Core::System::write(dump_fd, filtered);
                    }

                    auto remaining = stripper.flush();
                    if (!remaining.is_empty())
                        (void)Core::System::write(dump_fd, remaining);

                    (void)Core::System::close(read_fd);
                    (void)Core::System::close(dump_fd);
                    (void)Core::System::close(original_stderr_fd);
                    return 0; });

                s_stderr_dump_thread->start();
                s_stderr_dump_thread->detach();

                TRY(Core::System::dup2(write_fd, STDERR_FILENO));
                TRY(Core::System::close(write_fd));
                // Keep read_fd open for the dump thread.
            }
        }
    }

after_stderr_dump_setup:

    bool wait_for_debugger = false;
    StringView mach_server_name;

    Core::ArgsParser args_parser;
    args_parser.add_option(mach_server_name, "Mach server name", "mach-server-name", 0, "mach_server_name");
    args_parser.add_option(wait_for_debugger, "Wait for debugger", "wait-for-debugger");
    args_parser.parse(arguments);

    if (wait_for_debugger)
        Core::Process::wait_for_debugger_and_break();

    Core::EventLoop event_loop;

#if defined(AK_OS_MACOS)
    if (!mach_server_name.is_empty())
        Core::Platform::register_with_mach_server(mach_server_name);
#endif

    auto socket = TRY(Core::take_over_socket_from_system_server());
    auto transport = adopt_own(*new IPC::TransportSocket(move(socket)));
    auto client = AudioServer::AudioServerConnection::construct(move(transport));
    (void)client;

    return event_loop.exec();
}
