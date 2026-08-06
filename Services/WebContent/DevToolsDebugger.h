/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/Weakable.h>
#include <LibGC/Root.h>
#include <LibJS/Debugger.h>
#include <LibWebView/Debugger.h>
#include <WebContent/Forward.h>

namespace WebContent {

class DevToolsDebugger : public Weakable<DevToolsDebugger> {
    AK_MAKE_NONCOPYABLE(DevToolsDebugger);
    AK_MAKE_NONMOVABLE(DevToolsDebugger);

public:
    explicit DevToolsDebugger(ConnectionFromClient&);
    ~DevToolsDebugger();

    void attach(PageClient&);
    void configure(PageClient&, WebView::DebuggerConfiguration);
    void detach(PageClient&);
    void interrupt(PageClient&);
    void resume(PageClient&, WebView::DebuggerResumeMode);
    void update_blackboxing(PageClient&, Utf16String, Vector<WebView::DebuggerBlackboxRange>, WebView::DebuggerBlackboxingOperation);
    ErrorOr<void> set_breakpoint(PageClient&, WebView::DebuggerBreakpointLocation, WebView::DebuggerBreakpointOptions);
    ErrorOr<void> remove_breakpoint(PageClient&, WebView::DebuggerBreakpointLocation const&);
    Vector<WebView::DebuggerEnvironment> environments_for_frame(PageClient&, u64 frame_id);
    ErrorOr<WebView::DebuggerEvaluationResult> evaluate_in_frame(PageClient&, u64 frame_id, Utf16View source_text);
    ErrorOr<WebView::DebuggerObjectProperties> properties_for_object(PageClient&, u64 object_id);

private:
    void handle_pause(JS::Debugger::PauseInfo const&);
    void emit_logpoint(PageClient&, JS::Debugger::PauseInfo const&, JS::ExecutionContext&, WebView::DebuggerBreakpointOptions const&);
    void update_exception_pause_mode();
    bool pause_is_blackboxed(PageClient&, JS::Debugger::PauseInfo const&) const;
    PageClient* paused_page_client() const;
    void disable_if_unused();
    void schedule_disable_if_unused();
    void remove_breakpoints_for_page(u64 page_id);
    WebView::DebuggerValue serialize_value(JS::Value);

    struct BreakpointRegistration {
        WebView::DebuggerBreakpointLocation location;
        WebView::DebuggerBreakpointOptions options;
        JS::BreakpointID id { 0 };
    };

    struct BlackboxedSource {
        Utf16String url;
        WebView::DebuggerBlackboxState state;
    };

    ConnectionFromClient& m_client;
    HashTable<u64> m_attached_page_ids;
    HashMap<u64, WebView::DebuggerConfiguration> m_configurations;
    HashMap<u64, Vector<BreakpointRegistration>> m_breakpoints;
    HashMap<u64, Vector<BlackboxedSource>> m_blackboxed_sources;
    HashMap<u64, JS::ExecutionContext*> m_paused_frames;
    HashMap<u64, GC::Root<JS::Object>> m_paused_objects;
    HashMap<GC::Ptr<JS::Object>, u64> m_paused_object_ids;
    u64 m_next_environment_id { 1 };
    u64 m_next_frame_id { 1 };
    u64 m_next_object_id { 1 };
    Optional<u64> m_paused_page_id;
    WebView::DebuggerResumeMode m_resume_mode { WebView::DebuggerResumeMode::Continue };
    bool m_resume_requested { false };
    bool m_is_handling_pause { false };
    bool m_pause_callback_is_installed { false };
};

}
