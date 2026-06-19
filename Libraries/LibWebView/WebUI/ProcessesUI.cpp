/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Application.h>
#include <LibWebView/ProcessManager.h>
#include <LibWebView/SiteIsolationManager.h>
#include <LibWebView/WebUI/ProcessesUI.h>

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/String.h>

namespace WebView {

void ProcessesUI::register_interfaces()
{
    register_interface("updateProcessStatistics"sv, [this](auto const&) {
        update_process_statistics();
    });
}

void ProcessesUI::update_process_statistics()
{
    auto& process_manager = Application::process_manager();
    process_manager.update_all_process_statistics();

    auto process_embedders = SiteIsolationManager::the().remote_frame_process_embedders();
    auto serialize_process_statistics = [&] {
        JsonArray serialized;

        process_manager.for_each_process_statistics([&](auto& process, auto const& statistics) {
            auto type = process_name_from_type(process.type());
            auto const& title = process.title();

            auto process_name = title.has_value()
                ? MUST(String::formatted("{} - {}", type, *title))
                : MUST(String::from_utf8(type.bytes()));

            JsonObject object;
            object.set("name"sv, move(process_name));
            object.set("pid"sv, statistics.pid);
            object.set("cpu"sv, statistics.cpu_percent);
            object.set("memory"sv, statistics.memory_usage_bytes);
            if (auto embedder_pid = process_embedders.get(statistics.pid); embedder_pid.has_value())
                object.set("embedderPID"sv, *embedder_pid);
            serialized.must_append(move(object));
        });

        return serialized;
    };

    async_send_message("loadProcessStatistics"sv, serialize_process_statistics());
}

}
