/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Function.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/Time.h>
#include <LibCore/Forward.h>
#include <LibGfx/GpuDeviceIdentity.h>
#include <LibPlatformFeatures/SystemFacts.h>
#include <LibWebView/Export.h>

namespace WebView {

// Runs the SystemProbe helper once and reads the facts it reports. A probe that crashes, hangs or reports something
// unreadable yields facts that say so, rather than an error.
class WEBVIEW_API SystemProbeClient {
    AK_MAKE_NONCOPYABLE(SystemProbeClient);
    AK_MAKE_NONMOVABLE(SystemProbeClient);

public:
    AK_ALLOC_WITH_KMALLOC;

    static constexpr AK::Duration timeout = AK::Duration::from_seconds(4);
    static constexpr size_t maximum_report_size = 64 * KiB;

    // Destroying the client before the probe reports stops the probe without calling on_finish.
    static NonnullOwnPtr<SystemProbeClient> launch(Optional<Gfx::GpuDeviceIdentity> const& presentation_device, Function<void(PlatformFeatures::SystemFacts const&)> on_finish);

    ~SystemProbeClient();

    bool has_finished() const { return m_facts.has_value(); }

private:
    SystemProbeClient() = default;

    void read_available_output();
    void finish_with_report();
    void finish(PlatformFeatures::SystemFacts);
    void terminate_probe();
    void stop_reading();
    void fail(StringView reason);

    pid_t m_pid { -1 };
    OwnPtr<Core::File> m_output;
    RefPtr<Core::Notifier> m_notifier;
    RefPtr<Core::Timer> m_timeout_timer;
    ByteBuffer m_report;
    Optional<PlatformFeatures::SystemFacts> m_facts;
    // Called once, from launch() or from the event loop.
    Function<void(PlatformFeatures::SystemFacts const&)> m_on_finish;
};

}
