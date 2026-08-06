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
    m_attached_page_ids.remove(page.id());
    m_configurations.remove(page.id());
    remove_breakpoints_for_page(page.id());
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

ErrorOr<void> DevToolsDebugger::set_breakpoint(PageClient& page, WebView::DebuggerBreakpointLocation location)
{
    if (auto registrations = m_breakpoints.find(page.id()); registrations != m_breakpoints.end()) {
        if (registrations->value.find_if([&](auto const& registration) { return registration.location.represents_same_breakpoint_as(location); }) != registrations->value.end())
            return {};
    }

    auto& vm = Web::Bindings::main_thread_vm();
    vm.enable_debugging();
    ErrorOr<JS::BreakpointID> id = [&]() -> ErrorOr<JS::BreakpointID> {
        if (!location.source_id.has_value())
            return vm.debugger()->add_breakpoint(location.filename, location.line, location.column);
        auto source_code = page.devtools_source_code(*location.source_id);
        if (!source_code.has_value())
            return Error::from_string_literal("Unable to locate debugger source");
        return vm.debugger()->add_breakpoint(source_code.release_value(), location.line, location.column);
    }();
    if (id.is_error()) {
        disable_if_unused();
        return id.release_error();
    }
    m_breakpoints.ensure(page.id()).append({ move(location), id.release_value() });
    return {};
}

ErrorOr<void> DevToolsDebugger::remove_breakpoint(PageClient& page, WebView::DebuggerBreakpointLocation const& location)
{
    auto registrations = m_breakpoints.find(page.id());
    if (registrations == m_breakpoints.end())
        return Error::from_string_literal("Unable to locate debugger breakpoint");

    auto registration = registrations->value.find_if([&](auto const& candidate) {
        return candidate.location.represents_same_breakpoint_as(location);
    });
    if (registration == registrations->value.end())
        return Error::from_string_literal("Unable to locate debugger breakpoint");

    auto breakpoint_id = registration->id;
    registrations->value.remove(registration.index());
    if (registrations->value.is_empty())
        m_breakpoints.remove(registrations);

    bool breakpoint_is_still_used = false;
    for (auto const& page_breakpoints : m_breakpoints) {
        if (page_breakpoints.value.find_if([&](auto const& registration) { return registration.id == breakpoint_id; }) != page_breakpoints.value.end()) {
            breakpoint_is_still_used = true;
            break;
        }
    }

    auto& vm = Web::Bindings::main_thread_vm();
    if (!breakpoint_is_still_used && vm.debugger())
        vm.debugger()->remove_breakpoint(breakpoint_id);
    disable_if_unused();
    return {};
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
    if (!m_attached_page_ids.is_empty() || !m_breakpoints.is_empty() || m_is_handling_pause)
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

void DevToolsDebugger::remove_breakpoints_for_page(u64 page_id)
{
    auto registrations = m_breakpoints.take(page_id);
    if (!registrations.has_value())
        return;

    auto& vm = Web::Bindings::main_thread_vm();
    for (auto const& registration : *registrations) {
        bool breakpoint_is_still_used = false;
        for (auto const& page_breakpoints : m_breakpoints) {
            if (page_breakpoints.value.find_if([&](auto const& candidate) { return candidate.id == registration.id; }) != page_breakpoints.value.end()) {
                breakpoint_is_still_used = true;
                break;
            }
        }
        if (!breakpoint_is_still_used && vm.debugger())
            vm.debugger()->remove_breakpoint(registration.id);
    }
}

}
