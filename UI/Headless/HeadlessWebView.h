/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/RefPtr.h>
#include <LibCore/Forward.h>
#include <LibCore/Promise.h>
#include <LibGfx/Forward.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/PixelUnits.h>
#include <LibWebView/ViewImplementation.h>
#include <UI/Headless/Test.h>

namespace Ladybird {

class HeadlessWebView final : public WebView::ViewImplementation {
public:
    static NonnullOwnPtr<HeadlessWebView> create(Core::AnonymousBuffer theme, Web::DevicePixelSize window_size);
    static NonnullOwnPtr<HeadlessWebView> create_child(HeadlessWebView const&, u64 page_index);

    void clear_content_filters();

    NonnullRefPtr<Core::Promise<RefPtr<Gfx::Bitmap>>> take_screenshot();

    TestPromise& test_promise() { return *m_test_promise; }
    void on_test_complete(TestCompletion);

private:
    HeadlessWebView(Core::AnonymousBuffer theme, Web::DevicePixelSize viewport_size);

    void update_zoom() override;
    void initialize_client(CreateNewClient) override;

    virtual Web::DevicePixelSize viewport_size() const override { return m_viewport_size; }
    virtual Gfx::IntPoint to_content_position(Gfx::IntPoint widget_position) const override { return widget_position; }
    virtual Gfx::IntPoint to_widget_position(Gfx::IntPoint content_position) const override { return content_position; }

    virtual void did_receive_screenshot(Badge<WebView::WebContentClient>, Gfx::ShareableBitmap const& screenshot) override;

    Core::AnonymousBuffer m_theme;
    Web::DevicePixelSize m_viewport_size;

    RefPtr<Core::Promise<RefPtr<Gfx::Bitmap>>> m_pending_screenshot;

    NonnullRefPtr<TestPromise> m_test_promise;

    Web::Page::PendingDialog m_pending_dialog { Web::Page::PendingDialog::None };
    Optional<String> m_pending_prompt_text;
};

}
