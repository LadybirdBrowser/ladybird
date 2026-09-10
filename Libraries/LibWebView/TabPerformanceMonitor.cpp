/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <LibRequests/RequestClient.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWebView/ProcessManager.h>
#include <LibWebView/TabPerformanceMonitor.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WorkerProcessManager.h>

namespace WebView {

static TabPerformanceMonitor* s_monitor;

TabPerformanceMonitor& TabPerformanceMonitor::the()
{
    if (!s_monitor)
        s_monitor = new TabPerformanceMonitor;
    return *s_monitor;
}

void TabPerformanceMonitor::request_server_did_restart()
{
    if (s_monitor && s_monitor->m_enabled)
        s_monitor->config_variable_changed(ConfigVariableID::ShowTabPerformanceMonitor);
}

void TabPerformanceMonitor::forget_view(u64 view_id)
{
    if (s_monitor)
        s_monitor->m_tabs.remove(view_id);
}

TabPerformanceMonitor::TabPerformanceMonitor()
{
    config_variable_changed(ConfigVariableID::ShowTabPerformanceMonitor);
}

void TabPerformanceMonitor::config_variable_changed(ConfigVariableID id)
{
    if (id != ConfigVariableID::ShowTabPerformanceMonitor)
        return;
    m_enabled = Application::settings().config_variable_as_bool(id);
    auto& requests = Application::request_server_client();
    requests.async_set_performance_monitor_enabled(m_enabled);
    if (!m_enabled) {
        m_timer = nullptr;
        m_tabs.clear();
        requests.on_network_usage = nullptr;
        return;
    }
    requests.on_network_usage = [this](Vector<Requests::NetworkUsage> usage, u64 interval_microseconds) {
        if (!m_enabled)
            return;
        auto now = MonotonicTime::now();
        for (auto const& entry : usage) {
            Optional<u64> owner;
            WebContentClient::for_each_client([&](WebContentClient& client) {
                if (client.pid() != entry.process_id)
                    return IterationDecision::Continue;
                if (auto* navigable = client.navigable_for_page(entry.page_id)) {
                    if (auto view = ViewImplementation::find_view_for_traversable(navigable->top_level_traversable()); view.has_value())
                        owner = view->view_id();
                }
                return IterationDecision::Break;
            });
            if (!owner.has_value())
                owner = WorkerProcessManager::the().exclusive_performance_owner(entry.process_id);
            if (owner.has_value())
                m_tabs.ensure(*owner).add_network_bytes(now, interval_microseconds, entry.download_bytes, entry.upload_bytes);
        }
    };
    m_timer = Core::Timer::create_repeating(500, [this] { sample(); });
    m_timer->start();
    sample();
}

void TabPerformanceMonitor::did_present(u64 view_id)
{
    if (s_monitor && s_monitor->m_enabled)
        s_monitor->m_tabs.ensure(view_id).did_present(MonotonicTime::now());
}

void TabPerformanceMonitor::sample()
{
    HashMap<u64, HashMap<pid_t, Core::Platform::ProcessResourceUsage>> processes;
    Application::process_manager().for_each_process([&](Process& process) {
        Optional<u64> owner;
        if (process.type() == ProcessType::WebContent) {
            if (auto client = process.client<WebContentClient>())
                owner = client->exclusive_performance_owner();
        } else if (process.type() == ProcessType::WebWorker) {
            owner = WorkerProcessManager::the().exclusive_performance_owner(process.pid());
        }
        if (!owner.has_value())
            return;
        if (auto usage = Application::process_manager().resource_usage(process.pid()); usage.has_value())
            processes.ensure(*owner).set(process.pid(), *usage);
    });
    auto now = MonotonicTime::now();
    ViewImplementation::for_each_view([&](ViewImplementation& view) {
        auto stats = m_tabs.ensure(view.view_id()).sample(now, processes.ensure(view.view_id()));
        if (view.on_performance_stats)
            view.on_performance_stats(stats);
        return IterationDecision::Continue;
    });
}

}
