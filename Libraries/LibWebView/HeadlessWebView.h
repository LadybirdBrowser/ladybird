/*
 * Copyright (c) 2024-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCore/Forward.h>
#include <LibGfx/Forward.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/PixelUnits.h>
#include <LibWebView/ViewImplementation.h>

namespace WebView {

class HeadlessWebView : public WebView::ViewImplementation {
public:
    static NonnullOwnPtr<HeadlessWebView> create(Core::AnonymousBuffer theme, Web::DevicePixelSize window_size);
    static NonnullOwnPtr<HeadlessWebView> create_child(HeadlessWebView&, u64 page_index);

protected:
    HeadlessWebView(Core::AnonymousBuffer theme, Web::DevicePixelSize viewport_size);

    void initialize_client(CreateNewClient) override;
    void update_zoom() override;

    virtual Web::DevicePixelSize viewport_size() const override { return m_viewport_size; }
    virtual Gfx::IntPoint to_content_position(Gfx::IntPoint widget_position) const override { return widget_position; }
    virtual Gfx::IntPoint to_widget_position(Gfx::IntPoint content_position) const override { return content_position; }

    Core::AnonymousBuffer m_theme;
    Web::DevicePixelSize m_viewport_size;

    Web::Page::PendingDialog m_pending_dialog { Web::Page::PendingDialog::None };
    Optional<String> m_pending_prompt_text;

    // FIXME: We should implement UI-agnostic platform APIs to interact with the system clipboard.
    Optional<Web::Clipboard::SystemClipboardItem> m_clipboard;

    Vector<NonnullOwnPtr<HeadlessWebView>> m_child_web_views;
};

}
