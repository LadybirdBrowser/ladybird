/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <LibDevTools/Actor.h>
#include <LibWeb/Forward.h>

namespace DevTools {

class StyleSheetsActor final : public Actor {
public:
    static constexpr auto base_name = "style-sheets"sv;

    static NonnullRefPtr<StyleSheetsActor> create(DevToolsServer&, String name, WeakPtr<TabActor>);
    virtual ~StyleSheetsActor() override;

    void set_style_sheets(Vector<Web::CSS::StyleSheetIdentifier>);

private:
    StyleSheetsActor(DevToolsServer&, String name, WeakPtr<TabActor>);

    virtual void handle_message(Message const&) override;

    void style_sheet_source_received(Web::CSS::StyleSheetIdentifier const&, String source);

    WeakPtr<TabActor> m_tab;

    Vector<Web::CSS::StyleSheetIdentifier> m_style_sheets;
    HashMap<size_t, Message> m_pending_style_sheet_source_requests;
};

}
