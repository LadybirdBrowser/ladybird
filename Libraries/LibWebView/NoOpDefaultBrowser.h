/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/DefaultBrowser.h>

namespace WebView {

// The default browser of a platform that has no way to tell or change which browser opens links.
class NoOpDefaultBrowser final : public DefaultBrowser {
public:
    NoOpDefaultBrowser() { did_read_status(DefaultBrowserStatus::Unavailable); }

    virtual void refresh() override { }
    virtual void make_default(Function<void(MakeDefaultBrowserResult const&)> on_complete) override { on_complete({ .failed = true, .system_message = {} }); }
};

}
