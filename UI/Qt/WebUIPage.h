/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/JsonValue.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/String.h>
#include <LibWebView/Forward.h>

namespace Ladybird {

// One open page of the browser's own, such as about:history. The page reaches this through the Hub, which does not
// know what the page is about.
class WebUIPage {
public:
    AK_ALLOC_WITH_KMALLOC;

    virtual ~WebUIPage() = default;

    virtual void received_message(String const& name, JsonValue const& data) = 0;

protected:
    explicit WebUIPage(u64 web_ui_id)
        : m_web_ui_id(web_ui_id)
    {
    }

    u64 web_ui_id() const { return m_web_ui_id; }
    void send_message(String name, JsonValue data) const;

    // Whether the user clicked in the page just now. This uses up the click, so only one action follows from it.
    bool consume_user_activation() const;

private:
    u64 m_web_ui_id { 0 };
};

// The pages of the browser's own that the UI answers, and the ones of them that are open.
class WebUIPages {
public:
    AK_ALLOC_WITH_KMALLOC;

    using Factory = Function<NonnullOwnPtr<WebUIPage>(u64 web_ui_id, WebView::IsPrivate)>;

    // Makes about:<host> a page of the UI, which opens with a page made by the factory.
    void register_page(String host, String title, Factory);

    void page_opened(u64 web_ui_id, String const& host, WebView::IsPrivate);
    void page_received_message(u64 web_ui_id, String const& name, JsonValue const& data);
    void page_closed(u64 web_ui_id);

private:
    HashMap<String, Factory> m_factories;
    HashMap<u64, NonnullOwnPtr<WebUIPage>> m_pages;
};

}
