/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Vector.h>
#include <LibWebView/DefaultBrowser.h>

namespace Ladybird {

namespace Detail {

struct DefaultBrowserLinuxLink;

}

class DefaultBrowserLinux final : public WebView::DefaultBrowser {
public:
    explicit DefaultBrowserLinux(ByteString desktop_id);
    virtual ~DefaultBrowserLinux() override;

    virtual void refresh() override;
    virtual void make_default(Function<void(WebView::MakeDefaultBrowserResult const&)> on_complete) override;

private:
    enum class MakeDefault : u8 {
        No,
        Yes,
    };

    struct DesktopAnswer {
        WebView::DefaultBrowserStatus status { WebView::DefaultBrowserStatus::Unavailable };
        // What the desktop said when it could not make the browser the default, if it said anything.
        Optional<String> error_message;
    };

    static DesktopAnswer ask_desktop(ByteString const& desktop_id, MakeDefault);
    void ask_desktop_if_idle();
    void desktop_did_answer(MakeDefault, DesktopAnswer const&);

    ByteString m_desktop_id;
    NonnullRefPtr<Detail::DefaultBrowserLinuxLink> m_link;
    Vector<Function<void(WebView::MakeDefaultBrowserResult const&)>> m_pending_requests;
    bool m_asking_desktop { false };
    bool m_refresh_wanted { false };
};

}
