/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <AK/Weakable.h>
#include <LibJS/Debugger.h>
#include <WebContent/Forward.h>

namespace WebContent {

class DevToolsDebugger : public Weakable<DevToolsDebugger> {
    AK_MAKE_NONCOPYABLE(DevToolsDebugger);
    AK_MAKE_NONMOVABLE(DevToolsDebugger);

public:
    explicit DevToolsDebugger(ConnectionFromClient&);
    ~DevToolsDebugger();

    void attach(PageClient&);
    void detach(PageClient&);
    void interrupt(PageClient&);
    void resume(PageClient&);

private:
    void handle_pause(JS::Debugger::PauseInfo const&);
    PageClient* paused_page_client() const;
    void disable_if_unused();
    void schedule_disable_if_unused();

    ConnectionFromClient& m_client;
    HashTable<u64> m_attached_page_ids;
    Optional<u64> m_paused_page_id;
    bool m_resume_requested { false };
    bool m_is_handling_pause { false };
    bool m_pause_callback_is_installed { false };
};

}
