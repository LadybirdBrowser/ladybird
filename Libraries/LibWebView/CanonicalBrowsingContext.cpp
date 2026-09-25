/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/CanonicalBrowsingContext.h>
#include <LibWebView/CanonicalBrowsingContextGroup.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/CanonicalWindow.h>
#include <LibWebView/WebContentClient.h>

namespace WebView {

// https://html.spec.whatwg.org/multipage/document-sequences.html#creating-a-new-browsing-context
CanonicalBrowsingContext::BrowsingContextAndDocument CanonicalBrowsingContext::create_a_new_browsing_context_and_document(CanonicalBrowsingContextGroup& group, Optional<CanonicalBrowsingContext&> embedder_browsing_context, URL::Origin const& document_origin)
{
    // 1. Let browsingContext be a new browsing context.
    auto browsing_context = adopt_ref(*new CanonicalBrowsingContext);
    if (embedder_browsing_context.has_value())
        browsing_context->m_top_level_browsing_context = embedder_browsing_context->top_level_browsing_context();

    // 6. Let sandboxFlags be the result of determining the creation sandboxing flags given browsingContext and embedder.
    // 7. Let origin be the result of determining the origin given about:blank, sandboxFlags, and creatorOrigin.
    // NB: The process creating the document determines origin, and replicates it as the document's origin.
    auto const& origin = document_origin;

    // 9. Let agent be the result of obtaining a similar-origin window agent given origin, group, and false.
    auto agent = group.obtain_similar_origin_window_agent(origin, false);

    // 10. Let realm execution context be the result of creating a new realm given agent and the following customizations:
    //     - For the global object, create a new Window object.
    //     - For the global this binding, use browsingContext's WindowProxy object.
    // NB: The realm is in the process creating the document, which hosts agent once it holds the document.
    auto window = CanonicalWindow::create(agent);

    // 15. Let document be a new Document, with:
    //     origin: origin
    //     browsing context: browsingContext
    //     is initial about:blank: true
    auto document = CanonicalDocument::create(origin, browsing_context, window, CanonicalDocument::IsInitialAboutBlank::Yes);

    // 23. Make active document.
    document->make_active();

    // 25. Return browsingContext and document.
    return { browsing_context, document };
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#creating-a-new-top-level-browsing-context
// https://html.spec.whatwg.org/multipage/document-sequences.html#creating-a-new-browsing-context-group-and-document
CanonicalBrowsingContext::BrowsingContextAndDocument CanonicalBrowsingContext::create_a_new_top_level_browsing_context_and_document(URL::Origin const& document_origin)
{
    // NB: A group is kept alive by the browsing contexts in its browsing context set, so creating a new browsing context
    //     group and document is folded in here, where the browsing context holding the group is returned.

    // 1. Let group be a new browsing context group.
    // 2. Append group to the user agent's browsing context group set.
    auto group = CanonicalBrowsingContextGroup::create();

    // 3. Let browsingContext and document be the result of creating a new browsing context and document with null, null, and group.
    auto browsing_context_and_document = create_a_new_browsing_context_and_document(*group, {}, document_origin);

    // 4. Append browsingContext to group.
    group->append(*browsing_context_and_document.browsing_context);

    // 5. Return group and document.
    // 2. Return group's browsing context set[0] and document.
    return browsing_context_and_document;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#creating-a-new-auxiliary-browsing-context
CanonicalBrowsingContext::BrowsingContextAndDocument CanonicalBrowsingContext::create_a_new_auxiliary_browsing_context_and_document(CanonicalNavigable& opener, URL::Origin const& document_origin)
{
    // 1. Let openerTopLevelBrowsingContext be opener's top-level traversable's active browsing context.
    auto& opener_top_level_browsing_context = opener.top_level_traversable().active_browsing_context();

    // 2. Let group be openerTopLevelBrowsingContext's group.
    auto group = opener_top_level_browsing_context.group();

    // 3. Assert: group is non-null, as navigating invokes this directly.
    VERIFY(group);

    // 4. Let browsingContext and document be the result of creating a new browsing context and document with opener's active document, null, and group.
    auto browsing_context_and_document = create_a_new_browsing_context_and_document(*group, {}, document_origin);

    // FIXME: 5. Set browsingContext's is auxiliary to true.

    // 6. Append browsingContext to group.
    group->append(*browsing_context_and_document.browsing_context);

    // FIXME: 7. Set browsingContext's opener browsing context to opener.
    // FIXME: 8. Set browsingContext's virtual browsing context group ID to openerTopLevelBrowsingContext's virtual browsing context group ID.
    // FIXME: 9. Set browsingContext's opener origin at creation to opener's active document's origin.

    // 10. Return browsingContext and document.
    return browsing_context_and_document;
}

CanonicalBrowsingContext::~CanonicalBrowsingContext()
{
    if (m_group)
        m_group->remove(*this);
}

NonnullRefPtr<CanonicalDocument> CanonicalBrowsingContext::active_document() const
{
    return *m_active_document.strong_ref();
}

void CanonicalBrowsingContext::set_active_document(Badge<CanonicalDocument>, CanonicalDocument& document)
{
    m_active_document = document;
}

void CanonicalBrowsingContext::set_active_window(Badge<CanonicalDocument>, CanonicalWindow& window)
{
    m_window_proxy_window = window;
}

CanonicalBrowsingContext& CanonicalBrowsingContext::top_level_browsing_context()
{
    if (m_top_level_browsing_context)
        return *m_top_level_browsing_context;
    return *this;
}

RefPtr<CanonicalBrowsingContextGroup> CanonicalBrowsingContext::group() const
{
    return m_group;
}

void CanonicalBrowsingContext::set_group(Badge<CanonicalBrowsingContextGroup>, CanonicalBrowsingContextGroup* group)
{
    m_group = group;
}

}
