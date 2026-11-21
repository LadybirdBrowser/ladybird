/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibWebView/WebUI/AboutUI.h>

namespace WebView {

void AboutUI::register_interfaces()
{
    register_interface("loadPages"sv, [this](auto const&) {
        load_pages();
    });
}

void AboutUI::load_pages()
{
    auto pages = WebUI::pages();

    JsonArray page_list;
    page_list.ensure_capacity(pages.size());

    for (auto const& page : pages) {
        JsonObject entry;
        entry.set("host"sv, page.host);
        entry.set("title"sv, page.title);
        page_list.must_append(move(entry));
    }

    async_send_message("loadPages"sv, move(page_list));
}

}
