/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefPtr.h>
#include <LibWeb/Page/PageId.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

struct WEBVIEW_API WebContentPage {
    RefPtr<WebContentClient> client;
    Web::PageId id { 0 };

    bool is_open() const;

    bool operator==(WebContentPage const& other) const { return client.ptr() == other.client.ptr() && id == other.id; }
};

}
