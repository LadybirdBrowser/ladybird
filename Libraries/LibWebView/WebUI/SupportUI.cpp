/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/NeverDestroyed.h>
#include <LibWebView/Application.h>
#include <LibWebView/WebUI/SupportUI.h>

namespace WebView {

static NeverDestroyed<HashTable<SupportUI*>> s_open_pages;

SupportUI::~SupportUI()
{
    s_open_pages->remove(this);
}

void SupportUI::register_interfaces()
{
    s_open_pages->set(this);
    register_interface("loadPlatformFeatures"sv, [this](auto const&) {
        load_platform_features();
    });
}

void SupportUI::platform_features_changed()
{
    for (auto* page : *s_open_pages) {
        if (page->has_client())
            page->load_platform_features();
    }
}

void SupportUI::load_platform_features()
{
    async_send_message("loadPlatformFeatures"sv, Application::the().platform_features_report());
}

}
