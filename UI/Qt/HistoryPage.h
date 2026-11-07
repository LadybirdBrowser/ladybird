/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <UI/Qt/WebUIPage.h>

namespace Ladybird {

class HistoryPage final : public WebUIPage {
public:
    explicit HistoryPage(u64 web_ui_id);

    virtual void received_message(String const& name, JsonValue const& data) override;

private:
    void load_history_entries(JsonValue const&);
    void remove_history_entry(JsonValue const&);
    void forget_history_site(JsonValue const&);
};

}
