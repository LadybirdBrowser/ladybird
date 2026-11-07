/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonValue.h>
#include <AK/String.h>

namespace Ladybird {

// One open about:history. The page reaches this through the Hub, which does not know what the page is about.
class HistoryPage {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit HistoryPage(u64 web_ui_id);

    void received_message(String const& name, JsonValue const& data);

private:
    void load_history_entries(JsonValue const&);
    void remove_history_entry(JsonValue const&);
    void forget_history_site(JsonValue const&);

    u64 m_web_ui_id { 0 };
};

}
