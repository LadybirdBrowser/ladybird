/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/CanonicalBrowsingContext.h>
#include <LibWebView/CanonicalBrowsingContextGroup.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalWindow.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>

namespace WebView {

NonnullRefPtr<CanonicalDocument> CanonicalDocument::create(URL::URL creation_url, URL::Origin origin, NonnullRefPtr<CanonicalBrowsingContext> browsing_context, NonnullRefPtr<CanonicalWindow> relevant_global_object, IsInitialAboutBlank is_initial_about_blank)
{
    auto document = adopt_ref(*new CanonicalDocument(move(creation_url), move(origin), move(browsing_context), move(relevant_global_object), is_initial_about_blank));

    // https://html.spec.whatwg.org/multipage/document-lifecycle.html#initialise-the-document-object
    // 10. Set window's associated Document to document.
    // AD-HOC: Make a window reused from an initial about:blank keep the about:blank as its associated Document until
    //         this document is made active, since the navigation creating this document can be abandoned before this
    //         document is made active. That leaves the about:blank active, and the origin of the about:blank's window's
    //         relevant settings object is then the origin of the window's associated Document. Blink, WebKit, and Gecko
    //         all hand a reused window over to the new document only when the new document commits: WebKit in
    //         DocumentWriter::begin() (takeDOMWindowFrom()), Gecko in nsGlobalWindowOuter::SetNewDocument(), and Blink
    //         in DocumentLoader::CommitNavigation() (ShouldReuseDOMWindow()).
    if (!document->m_relevant_global_object->has_associated_document())
        document->m_relevant_global_object->set_associated_document({}, document);
    return document;
}

CanonicalDocument::CanonicalDocument(URL::URL creation_url, URL::Origin origin, NonnullRefPtr<CanonicalBrowsingContext> browsing_context, NonnullRefPtr<CanonicalWindow> relevant_global_object, IsInitialAboutBlank is_initial_about_blank)
    : m_creation_url(move(creation_url))
    , m_origin(move(origin))
    , m_browsing_context(move(browsing_context))
    , m_relevant_global_object(move(relevant_global_object))
    , m_is_initial_about_blank(is_initial_about_blank)
{
}

CanonicalDocument::~CanonicalDocument() = default;

Optional<URL::Origin> CanonicalDocument::top_level_origin() const
{
    auto& top_level_browsing_context = m_browsing_context->top_level_browsing_context();

    // NB: A top-level document is not yet its browsing context's active document when it is placed in a process.
    if (&top_level_browsing_context == m_browsing_context.ptr())
        return m_origin;
    if (top_level_browsing_context.has_been_discarded())
        return {};
    return top_level_browsing_context.active_document()->origin();
}

void CanonicalDocument::set_host(RefPtr<WebContentPage> host)
{
    m_host = move(host);
    if (m_host) {
        m_relevant_global_object->agent().set_hosting_process_if_unset(m_host->client());
        m_host->client().request_server_site_bindings().bind_sites_of(*this);
    }
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

    // AD-HOC: Make a window reused from an initial about:blank take this document as its associated Document only now
    //         that this document is made active. See create().
    window.set_associated_document({}, *this);
}

}
