/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/CanonicalBrowsingContext.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalWindow.h>
#include <LibWebView/WebContentPage.h>

namespace WebView {

NonnullRefPtr<CanonicalDocument> CanonicalDocument::create(URL::Origin origin, NonnullRefPtr<CanonicalBrowsingContext> browsing_context, NonnullRefPtr<CanonicalWindow> relevant_global_object, IsInitialAboutBlank is_initial_about_blank)
{
    return adopt_ref(*new CanonicalDocument(move(origin), move(browsing_context), move(relevant_global_object), is_initial_about_blank));
}

CanonicalDocument::CanonicalDocument(URL::Origin origin, NonnullRefPtr<CanonicalBrowsingContext> browsing_context, NonnullRefPtr<CanonicalWindow> relevant_global_object, IsInitialAboutBlank is_initial_about_blank)
    : m_origin(move(origin))
    , m_browsing_context(move(browsing_context))
    , m_relevant_global_object(move(relevant_global_object))
    , m_is_initial_about_blank(is_initial_about_blank)
{
}

CanonicalDocument::~CanonicalDocument() = default;

void CanonicalDocument::set_host(RefPtr<WebContentPage> host)
{
    m_host = move(host);
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#make-active
void CanonicalDocument::make_active()
{
    // 1. Let window be document's relevant global object.
    auto& window = relevant_global_object();

    // 2. Set document's browsing context's active document to document.
    m_browsing_context->set_active_document({}, *this);

    // 3. Set document's browsing context's WindowProxy's [[Window]] internal slot value to window.
    m_browsing_context->set_active_window({}, window);

    // 4. Set window's relevant settings object's execution ready flag.
    // NB: The process hosting the document sets it.
}

}
