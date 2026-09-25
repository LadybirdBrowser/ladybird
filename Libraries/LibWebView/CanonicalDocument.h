/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/Weakable.h>
#include <LibURL/Origin.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

// https://dom.spec.whatwg.org/#concept-document
class WEBVIEW_API CanonicalDocument final
    : public RefCounted<CanonicalDocument>
    , public Weakable<CanonicalDocument> {
public:
    enum class IsInitialAboutBlank : bool {
        No,
        Yes,
    };

    static NonnullRefPtr<CanonicalDocument> create(URL::Origin, NonnullRefPtr<CanonicalBrowsingContext>, NonnullRefPtr<CanonicalWindow>, IsInitialAboutBlank);

    ~CanonicalDocument();

    // https://dom.spec.whatwg.org/#concept-document-origin
    URL::Origin const& origin() const { return m_origin; }

    // https://html.spec.whatwg.org/multipage/document-sequences.html#concept-document-bc
    CanonicalBrowsingContext& browsing_context() const { return m_browsing_context; }

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-relevant-global
    CanonicalWindow& relevant_global_object() const { return m_relevant_global_object; }

    // https://html.spec.whatwg.org/multipage/dom.html#is-initial-about:blank
    bool is_initial_about_blank() const { return m_is_initial_about_blank == IsInitialAboutBlank::Yes; }

    // https://html.spec.whatwg.org/multipage/dom.html#completely-loaded
    bool is_completely_loaded() const { return m_completely_loaded; }
    void set_completely_loaded() { m_completely_loaded = true; }

    RefPtr<WebContentPage> const& host() const { return m_host; }
    void set_host(RefPtr<WebContentPage>);

    void make_active();

private:
    CanonicalDocument(URL::Origin, NonnullRefPtr<CanonicalBrowsingContext>, NonnullRefPtr<CanonicalWindow>, IsInitialAboutBlank);

    URL::Origin m_origin;
    NonnullRefPtr<CanonicalBrowsingContext> m_browsing_context;
    NonnullRefPtr<CanonicalWindow> m_relevant_global_object;
    IsInitialAboutBlank m_is_initial_about_blank { IsInitialAboutBlank::No };
    bool m_completely_loaded { false };
    RefPtr<WebContentPage> m_host;
};

}
