/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibJS/Bytecode/Executable.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>
#include <LibWebView/Debugger.h>
#include <WebContent/ConnectionFromClient.h>
#include <WebContent/DevToolsDebugger.h>
#include <WebContent/PageClient.h>
#include <WebContent/PageHost.h>

namespace WebContent {

DevToolsDebugger::DevToolsDebugger(ConnectionFromClient& client)
    : m_client(client)
{
}

DevToolsDebugger::~DevToolsDebugger()
{
    auto& vm = Web::Bindings::main_thread_vm();
    if (auto* debugger = vm.debugger(); debugger && !debugger->is_paused())
        vm.disable_debugging();
}

void DevToolsDebugger::attach(PageClient& page)
{
    if (m_attached_page_ids.set(page.id()) != AK::HashSetResult::InsertedNewEntry)
        return;

    auto& vm = Web::Bindings::main_thread_vm();
    vm.enable_debugging();
    if (!m_pause_callback_is_installed) {
        vm.debugger()->set_pause_callback([this](auto const& pause) {
            handle_pause(pause);
        });
        m_pause_callback_is_installed = true;
    }
}

void DevToolsDebugger::configure(PageClient& page, WebView::DebuggerConfiguration configuration)
{
    m_configurations.set(page.id(), configuration);
}

void DevToolsDebugger::detach(PageClient& page)
{
    if (!m_attached_page_ids.remove(page.id()))
        return;

    m_configurations.remove(page.id());
    if (m_paused_page_id == page.id())
        m_resume_requested = true;

    disable_if_unused();
}

void DevToolsDebugger::interrupt(PageClient& page)
{
    if (!m_attached_page_ids.contains(page.id()))
        return;

    auto& vm = Web::Bindings::main_thread_vm();
    if (auto* debugger = vm.debugger())
        debugger->request_pause_on_next_bytecode_execution();
}

void DevToolsDebugger::resume(PageClient& page)
{
    if (m_paused_page_id == page.id())
        m_resume_requested = true;
}

PageClient* DevToolsDebugger::paused_page_client() const
{
    auto& vm = Web::Bindings::main_thread_vm();
    if (!vm.current_realm())
        return nullptr;

    auto* window = Web::Bindings::window_from_global_object(vm.current_realm()->global_object());
    if (!window)
        return nullptr;

    for (auto page_id : m_attached_page_ids) {
        auto page = m_client.page_host().page(page_id);
        if (page.has_value() && &page->page() == &window->page())
            return &*page;
    }
    return nullptr;
}

void DevToolsDebugger::handle_pause(JS::Debugger::PauseInfo const& pause)
{
    auto* page = paused_page_client();
    if (!page) {
        Web::Bindings::main_thread_vm().debugger()->continue_execution();
        return;
    }

    auto configuration = m_configurations.get(page->id()).value_or(WebView::DebuggerConfiguration {});
    if ((pause.reason == JS::Debugger::PauseReason::DebuggerStatement && !configuration.should_pause_on_debugger_statement)
        || (pause.reason == JS::Debugger::PauseReason::Breakpoint && configuration.skip_breakpoints)) {
        Web::Bindings::main_thread_vm().debugger()->continue_execution();
        return;
    }

    auto source = page->devtools_source_description(*pause.executable->source_code);
    if (!source.has_value()) {
        Web::Bindings::main_thread_vm().debugger()->continue_execution();
        return;
    }

    auto line = source->source_start_line;
    auto column = source->source_start_column;
    if (pause.source_range.has_value()) {
        line = pause.source_range->start.line;
        column = pause.source_range->start.column;
    }
    WebView::DebuggerPause debugger_pause {
        .reason = [&] {
            switch (pause.reason) {
            case JS::Debugger::PauseReason::Breakpoint:
                return WebView::DebuggerPauseReason::Breakpoint;
            case JS::Debugger::PauseReason::DebuggerStatement:
                return WebView::DebuggerPauseReason::DebuggerStatement;
            case JS::Debugger::PauseReason::Entry:
                return WebView::DebuggerPauseReason::Entry;
            }
            VERIFY_NOT_REACHED();
        }(),
        .frames = {},
    };
    debugger_pause.frames.append(WebView::DebuggerFrame {
        .id = 1,
        .display_name = pause.executable->name.to_utf16_string(),
        .location = {
            .source = source.release_value(),
            .line = line,
            .column = column,
        },
    });

    m_paused_page_id = page->id();
    m_resume_requested = false;
    m_is_handling_pause = true;

    auto event_loop_pause = Web::HTML::main_thread_event_loop().pause();
    m_client.async_did_pause_debugger(page->id(), move(debugger_pause));
    Core::EventLoop::current().spin_until([this] {
        return m_resume_requested;
    });

    m_is_handling_pause = false;
    m_paused_page_id = {};
    Web::Bindings::main_thread_vm().debugger()->continue_execution();
    schedule_disable_if_unused();
}

void DevToolsDebugger::disable_if_unused()
{
    if (!m_attached_page_ids.is_empty() || m_is_handling_pause)
        return;

    auto& vm = Web::Bindings::main_thread_vm();
    if (vm.debugging_enabled()) {
        vm.disable_debugging();
        m_pause_callback_is_installed = false;
    }
}

void DevToolsDebugger::schedule_disable_if_unused()
{
    Core::deferred_invoke([weak_this = make_weak_ptr<DevToolsDebugger>()] {
        if (weak_this)
            weak_this->disable_if_unused();
    });
}

}
