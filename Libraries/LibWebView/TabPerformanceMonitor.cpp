/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <LibRequests/RequestControlClient.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWebView/ProcessManager.h>
#include <LibWebView/RequestServerManager.h>
#include <LibWebView/TabPerformanceMonitor.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>

namespace WebView {

static TabPerformanceMonitor* s_monitor;

TabPerformanceMonitor& TabPerformanceMonitor::the()
{
    if (!s_monitor)
        s_monitor = new TabPerformanceMonitor;
    return *s_monitor;
}

void TabPerformanceMonitor::configure(Requests::RequestControlClient& requests)
{
    if (!s_monitor)
        return;
    requests.async_set_performance_monitor_enabled(s_monitor->m_enabled);
    if (!s_monitor->m_enabled) {
        requests.on_network_usage = nullptr;
        return;
    }
    requests.on_network_usage = [](Vector<Requests::NetworkUsage> usage, u64 interval_microseconds) {
        if (!s_monitor || !s_monitor->m_enabled)
            return;
        auto now = MonotonicTime::now();
        for (auto const& entry : usage) {
            Optional<u64> owner;
            WebContentClient::for_each_client([&](WebContentClient& client) {
                if (client.pid() != entry.process_id)
                    return IterationDecision::Continue;
                // NB: What a worker on a thread of the process uses comes with no page.
                if (auto* page = client.page(entry.page_id)) {
                    auto const& view = page->view();
                    owner = view.owner_view_id().value_or(view.view_id());
                } else {
                    owner = client.exclusive_performance_owner();
                }
                return IterationDecision::Break;
            });
            if (owner.has_value())
                s_monitor->m_tabs.ensure(*owner).add_network_bytes(now, interval_microseconds, entry.download_bytes, entry.upload_bytes);
        }
    };
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
    if (!m_enabled) {
        m_timer = nullptr;
        m_tabs.clear();
    }
    Application::request_server_manager().for_each_instance([](RequestServerInstance& instance) {
        configure(instance.control_client());
        return IterationDecision::Continue;
    });
    if (!m_enabled)
        return;
    m_timer = Core::Timer::create_repeating(AK::Duration::from_milliseconds(500), [this] { sample(); });
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
    HashMap<u64, HashTable<u64>> tabs_sharing_processes;
    Application::process_manager().for_each_process([&](Process& process) {
        if (process.type() != ProcessType::WebContent)
            return;
        auto client = process.client<WebContentClient>();
        if (!client)
            return;
        auto owners = client->performance_owners();
        if (owners.is_empty())
            return;
        auto usage = Application::process_manager().resource_usage(process.pid());
        if (!usage.has_value())
            return;
        // NB: A process that several tabs share counts in full toward each of them. Each tab then reports how
        //     many other tabs share its processes, so the UI can show that the numbers overlap.
        for (auto owner : owners) {
            processes.ensure(owner).set(process.pid(), *usage);
            for (auto other_owner : owners) {
                if (other_owner != owner)
                    tabs_sharing_processes.ensure(owner).set(other_owner);
            }
        }
    });
    auto now = MonotonicTime::now();
    ViewImplementation::for_each_view([&](ViewImplementation& view) {
        if (view.owner_view_id().has_value())
            return IterationDecision::Continue;
        Optional<pid_t> top_level_process;
        if (auto page = view.traversable().display_page())
            top_level_process = page->client().pid();
        auto stats = m_tabs.ensure(view.view_id()).sample(now, processes.ensure(view.view_id()), top_level_process);
        if (auto sharing = tabs_sharing_processes.get(view.view_id()); sharing.has_value())
            stats.other_tabs_sharing_processes = sharing->size();
        if (view.on_performance_stats)
            view.on_performance_stats(stats);
        return IterationDecision::Continue;
    });
}

}
