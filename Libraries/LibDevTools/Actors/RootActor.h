/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibDevTools/Actor.h>

namespace DevTools {

class RootActor final : public Actor {
public:
    static constexpr auto base_name = "root"sv;

    static NonnullRefPtr<RootActor> create(DevToolsServer&, ByteString name);
    virtual ~RootActor() override;

    virtual void handle_message(StringView type, JsonObject const&) override;

    void send_tab_list_changed_message();

private:
    RootActor(DevToolsServer&, ByteString name);

    // https://firefox-source-docs.mozilla.org/devtools/backend/protocol.html#the-request-reply-notify-pattern
    // the root actor sends at most one "tabListChanged" notification after each "listTabs" request.
    bool m_has_sent_tab_list_changed_since_last_list_tabs_request { false };
};

}
