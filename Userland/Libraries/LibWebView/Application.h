/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCore/EventLoop.h>
#include <LibWebView/Process.h>
#include <LibWebView/ProcessManager.h>

#ifdef __swift__
#    include <swift/bridging>
#else
#    define SWIFT_IMMORTAL_REFERENCE
#endif

namespace WebView {

class Application {
    AK_MAKE_NONCOPYABLE(Application);

public:
    Application(int argc, char** argv);
    virtual ~Application();

    int exec();

    static Application& the() { return *s_the; }

    Core::EventLoop& event_loop() { return m_event_loop; }

    void add_child_process(Process&&);

    // FIXME: Should these methods be part of Application, instead of deferring to ProcessManager?
#if defined(AK_OS_MACH)
    void set_process_mach_port(pid_t, Core::MachPort&&);
#endif
    Optional<Process&> find_process(pid_t);

    // FIXME: Should we just expose the ProcessManager via a getter?
    void update_process_statistics();
    String generate_process_statistics_html();

protected:
    virtual void process_did_exit(Process&&);

private:
    static Application* s_the;

    Core::EventLoop m_event_loop;
    ProcessManager m_process_manager;
    bool m_in_shutdown { false };
} SWIFT_IMMORTAL_REFERENCE;

}
