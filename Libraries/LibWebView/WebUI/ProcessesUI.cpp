/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Application.h>
#include <LibWebView/ProcessManager.h>
#include <LibWebView/WebUI/ProcessesUI.h>

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

    async_send_message("loadProcessStatistics"sv, process_manager.serialize_json());
}

}
