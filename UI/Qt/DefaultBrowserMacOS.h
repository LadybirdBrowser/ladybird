/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Vector.h>
#include <LibWebView/DefaultBrowser.h>

namespace Ladybird {

namespace Detail {

struct DefaultBrowserMacOSLink;

}

class DefaultBrowserMacOS final : public WebView::DefaultBrowser {
public:
    DefaultBrowserMacOS();
    virtual ~DefaultBrowserMacOS() override;

    virtual void refresh() override;
    virtual void make_default(Function<void(WebView::MakeDefaultBrowserResult const&)> on_complete) override;

private:
    void system_did_answer(WebView::MakeDefaultBrowserResult const&);

    NonnullRefPtr<Detail::DefaultBrowserMacOSLink> m_link;
    Vector<Function<void(WebView::MakeDefaultBrowserResult const&)>> m_pending_requests;
};

}
