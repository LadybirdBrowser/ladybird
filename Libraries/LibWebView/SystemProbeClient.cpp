/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/File.h>
#include <LibCore/Notifier.h>
#include <LibCore/Process.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#include <LibFileSystem/FileSystem.h>
#include <LibWebCommon/WebView/Utilities.h>
#include <LibWebView/Process.h>
#include <LibWebView/SystemProbeClient.h>

namespace WebView {

static ErrorOr<Core::Process> spawn_probe(Optional<Gfx::GpuDeviceIdentity> const& presentation_device, int output_fd)
{
    Vector<ByteString> arguments;
    if (presentation_device.has_value()) {
        arguments.append("--gpu-device"sv);
        arguments.append(presentation_device->to_byte_string());
    }

    Core::ProcessSpawnOptions options { .name = "SystemProbe"sv };
    options.arguments = arguments;
    options.file_actions.append(Core::FileAction::DupFd { .write_fd = output_fd, .fd = STDOUT_FILENO });

    for (auto const& path : TRY(get_paths_for_helper_process("SystemProbe"sv))) {
        if (!FileSystem::exists(path))
            continue;
        options.executable = path;
        // The probe loads the driver as the Compositor will, so it sees the same environment.
        return Process::spawn_without_transport(ProcessType::Compositor, options);
    }
    return Error::from_string_literal("The SystemProbe helper was not found");
}

NonnullOwnPtr<SystemProbeClient> SystemProbeClient::launch(Optional<Gfx::GpuDeviceIdentity> const& presentation_device, Function<void(PlatformFeatures::SystemFacts const&)> on_finish)
{
    auto client_owner = adopt_own(*new SystemProbeClient);
    auto& client = *client_owner;
    client.m_on_finish = move(on_finish);

    auto pipe = Core::System::pipe2(O_CLOEXEC);
    if (pipe.is_error()) {
        client.fail("Could not create a pipe for its report"sv);
        return client_owner;
    }
    auto output = Core::File::adopt_fd(pipe.value()[0], Core::File::OpenMode::Read);
    if (output.is_error()) {
        (void)Core::System::close(pipe.value()[0]);
        (void)Core::System::close(pipe.value()[1]);
        client.fail("Could not create a pipe for its report"sv);
        return client_owner;
    }
    client.m_output = output.release_value();

    auto process = spawn_probe(presentation_device, pipe.value()[1]);
    (void)Core::System::close(pipe.value()[1]);
    if (process.is_error()) {
        client.fail("It could not be launched"sv);
        return client_owner;
    }

    client.m_pid = process.value().pid();
    client.m_notifier = Core::Notifier::construct(client.m_output->fd(), Core::Notifier::Type::Read);
    client.m_notifier->on_activation = [&client] {
        client.read_available_output();
    };
    client.m_timeout_timer = Core::Timer::create_single_shot(timeout, [&client] {
        client.fail("It did not report in time"sv);
    });
    client.m_timeout_timer->start();
    return client_owner;
}

SystemProbeClient::~SystemProbeClient()
{
    if (!has_finished())
        terminate_probe();
}

void SystemProbeClient::read_available_output()
{
    Array<u8, 4 * KiB> buffer;
    auto result = m_output->read_some(buffer);
    if (result.is_error()) {
        if (result.error().is_errno() && (result.error().code() == EINTR || result.error().code() == EAGAIN))
            return;
        fail("Its report could not be read"sv);
        return;
    }
    auto bytes_read = result.value().size();
    if (bytes_read == 0) {
        finish_with_report();
        return;
    }
    if (m_report.size() + bytes_read > maximum_report_size) {
        fail("Its report was too large"sv);
        return;
    }
    m_report.append(result.value());
}

void SystemProbeClient::finish_with_report()
{
    auto facts = PlatformFeatures::SystemFacts::from_json(StringView { m_report.bytes() });
    if (facts.is_error()) {
        // A probe that died before reporting closed its output just the same.
        fail(m_report.is_empty() ? "It exited without a report"sv : "Its report could not be understood"sv);
        return;
    }
    finish(facts.release_value());
}

void SystemProbeClient::fail(StringView reason)
{
    terminate_probe();
    PlatformFeatures::SystemFacts facts;
    facts.probe_failure = MUST(String::from_utf8(reason));
    finish(move(facts));
}

void SystemProbeClient::finish(PlatformFeatures::SystemFacts facts)
{
    if (has_finished())
        return;
    m_facts = move(facts);
    stop_reading();

    if (m_on_finish)
        m_on_finish(*m_facts);
}

void SystemProbeClient::terminate_probe()
{
    if (m_pid > 0)
        (void)Core::Process::terminate_process(m_pid, Core::Process::TerminationMode::Forceful);
    stop_reading();
}

void SystemProbeClient::stop_reading()
{
    if (m_notifier) {
        m_notifier->close();
        m_notifier = nullptr;
    }
    if (m_timeout_timer) {
        m_timeout_timer->stop();
        m_timeout_timer = nullptr;
    }
    m_output = nullptr;
    // The ProcessMonitor reaps the probe along with every other child.
    m_pid = -1;
}

}
