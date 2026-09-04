/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/CanonicalBrowsingContext.h>
#include <LibWebView/CanonicalBrowsingContextGroup.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/WebContentClient.h>

namespace WebView {

// https://html.spec.whatwg.org/multipage/document-sequences.html#creating-a-new-browsing-context
NonnullRefPtr<CanonicalBrowsingContext> CanonicalBrowsingContext::create_a_new_browsing_context_and_document(CanonicalBrowsingContextGroup& group, URL::Origin const& document_origin, Optional<WebContentClient&> document_process)
{
    // 1. Let browsingContext be a new browsing context.
    auto browsing_context = adopt_ref(*new CanonicalBrowsingContext);

    // 6. Let sandboxFlags be the result of determining the creation sandboxing flags given browsingContext and embedder.
    // 7. Let origin be the result of determining the origin given about:blank, sandboxFlags, and creatorOrigin.
    // NB: The process creating the document determines origin, and replicates it as the document's origin.

    // 9. Let agent be the result of obtaining a similar-origin window agent given origin, group, and false.
    auto agent = group.obtain_similar_origin_window_agent(document_origin, false);

    // NB: Steps 10 to 24 run in the WebContent process creating the document, so that process hosts agent.
    if (document_process.has_value())
        agent->set_hosting_process_if_unset(*document_process);

    // 25. Return browsingContext and document.
    return browsing_context;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#creating-a-new-top-level-browsing-context
// https://html.spec.whatwg.org/multipage/document-sequences.html#creating-a-new-browsing-context-group-and-document
NonnullRefPtr<CanonicalBrowsingContext> CanonicalBrowsingContext::create_a_new_top_level_browsing_context_and_document(URL::Origin const& document_origin, Optional<WebContentClient&> document_process)
{
    // NB: A group is kept alive by the browsing contexts in its browsing context set, so creating a new browsing context
    //     group and document is folded in here, where the browsing context holding the group is returned.

    // 1. Let group be a new browsing context group.
    // 2. Append group to the user agent's browsing context group set.
    auto group = CanonicalBrowsingContextGroup::create();

    // 3. Let browsingContext and document be the result of creating a new browsing context and document with null, null, and group.
    auto browsing_context = create_a_new_browsing_context_and_document(*group, document_origin, document_process);

    // 4. Append browsingContext to group.
    group->append(*browsing_context);

    // 5. Return group and document.
    // 2. Return group's browsing context set[0] and document.
    return browsing_context;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#creating-a-new-auxiliary-browsing-context
NonnullRefPtr<CanonicalBrowsingContext> CanonicalBrowsingContext::create_a_new_auxiliary_browsing_context_and_document(CanonicalNavigable& opener, URL::Origin const& document_origin, Optional<WebContentClient&> document_process)
{
    // 1. Let openerTopLevelBrowsingContext be opener's top-level traversable's active browsing context.
    auto& opener_top_level_browsing_context = opener.top_level_traversable().active_browsing_context();

    // 2. Let group be openerTopLevelBrowsingContext's group.
    auto group = opener_top_level_browsing_context.group();

    // 3. Assert: group is non-null, as navigating invokes this directly.
    VERIFY(group);

    // 4. Let browsingContext and document be the result of creating a new browsing context and document with opener's active document, null, and group.
    auto browsing_context = create_a_new_browsing_context_and_document(*group, document_origin, document_process);

    // FIXME: 5. Set browsingContext's is auxiliary to true.

    // 6. Append browsingContext to group.
    group->append(*browsing_context);

    // FIXME: 7. Set browsingContext's opener browsing context to opener.
    // FIXME: 8. Set browsingContext's virtual browsing context group ID to openerTopLevelBrowsingContext's virtual browsing context group ID.
    // FIXME: 9. Set browsingContext's opener origin at creation to opener's active document's origin.

    // 10. Return browsingContext and document.
    return browsing_context;
}

CanonicalBrowsingContext::~CanonicalBrowsingContext()
{
    if (m_group)
        m_group->remove(*this);
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
