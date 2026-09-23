/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#window
class WEBVIEW_API CanonicalWindow final : public RefCounted<CanonicalWindow> {
public:
    static NonnullRefPtr<CanonicalWindow> create(NonnullRefPtr<CanonicalSimilarOriginWindowAgent>);

    ~CanonicalWindow();

    // NB: The agent of the Window's realm.
    CanonicalSimilarOriginWindowAgent& agent() const { return m_agent; }

private:
    explicit CanonicalWindow(NonnullRefPtr<CanonicalSimilarOriginWindowAgent>);

    NonnullRefPtr<CanonicalSimilarOriginWindowAgent> m_agent;
};

}
