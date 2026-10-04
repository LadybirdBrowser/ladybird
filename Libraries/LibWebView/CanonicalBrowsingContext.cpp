/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebCommon/HTML/BrowsingContext.h>
#include <LibWebCommon/HTML/ReplicatedNavigableState.h>
#include <LibWebView/CanonicalBrowsingContext.h>
#include <LibWebView/CanonicalBrowsingContextGroup.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/CanonicalWindow.h>
#include <LibWebView/WebContentClient.h>

namespace WebView {

// https://html.spec.whatwg.org/multipage/browsers.html#determining-the-creation-sandboxing-flags
Web::HTML::SandboxingFlagSet determine_the_creation_sandboxing_flags(CanonicalBrowsingContext const& browsing_context, Optional<Web::HTML::ReplicatedContainerState const&> embedder)
{
    // To determine the creation sandboxing flags for a browsing context browsing context, given null or an element
    // embedder, return the union of the flags that are present in the following sandboxing flag sets:

    // - If embedder is null, then: the flags set on browsing context's popup sandboxing flag set.
    if (!embedder.has_value())
        return browsing_context.popup_sandboxing_flag_set();

    // - If embedder is an element, then: the flags set on embedder's iframe sandboxing flag set.
    // - If embedder is an element, then: the flags set on embedder's node document's active sandboxing flag set.
    return embedder->iframe_sandboxing_flag_set | embedder->document_active_sandboxing_flag_set;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#creating-a-new-browsing-context
CanonicalBrowsingContext::BrowsingContextAndDocument CanonicalBrowsingContext::create_a_new_browsing_context_and_document(CanonicalDocument const* creator, Optional<Web::HTML::ReplicatedContainerState const&> embedder, CanonicalBrowsingContextGroup& group, Web::HTML::CrossProcessId document_id, Optional<Web::HTML::EnvironmentId> environment_id, Optional<URL::Origin> given_origin)
{
    // 1. Let browsingContext be a new browsing context.
    auto browsing_context = adopt_ref(*new CanonicalBrowsingContext);
    if (embedder.has_value())
        browsing_context->m_top_level_browsing_context = creator->browsing_context().top_level_browsing_context();

    // 3. Let creatorOrigin be null.
    Optional<URL::Origin> creator_origin;

    // 5. If creator is non-null:
    if (creator) {
        // 1. Set creatorOrigin to creator's origin.
        creator_origin = creator->origin();

        // 3. Set browsingContext's virtual browsing context group ID to creator's browsing context's top-level browsing context's virtual browsing context group ID.
        browsing_context->m_virtual_browsing_context_group_id = creator->browsing_context().top_level_browsing_context().virtual_browsing_context_group_id();
    }

    // 6. Let sandboxFlags be the result of determining the creation sandboxing flags given browsingContext and embedder.
    auto sandbox_flags = determine_the_creation_sandboxing_flags(browsing_context, embedder);

    // 7. Let origin be the result of determining the origin given about:blank, sandboxFlags, and creatorOrigin.
    // NB: An origin determined earlier, by the UI process for a traversable's first document or by a process creating
    //     the document before the UI process hears of it, is taken when determining the origin could have given it:
    //     creatorOrigin, or a new opaque origin that no document holds.
    auto about_blank = URL::about_blank();
    auto is_a_new_opaque_origin = has_flag(sandbox_flags, Web::HTML::SandboxingFlagSet::SandboxedOrigin) || !creator_origin.has_value();
    auto origin = Web::HTML::determine_the_origin(about_blank, sandbox_flags, move(creator_origin));
    if (given_origin.has_value()) {
        auto could_be_determined = is_a_new_opaque_origin
            ? given_origin->is_opaque() && !CanonicalTraversable::is_origin_held_by_a_document(*given_origin)
            : given_origin->is_same_origin(origin);
        if (could_be_determined)
            origin = given_origin.release_value();
    }

    // 9. Let agent be the result of obtaining a similar-origin window agent given origin, group, and false.
    auto agent = group.obtain_similar_origin_window_agent(origin, false);

    // 10. Let realm execution context be the result of creating a new realm given agent and the following customizations:
    //     - For the global object, create a new Window object.
    //     - For the global this binding, use browsingContext's WindowProxy object.
    // NB: The realm is in the process creating the document, which hosts agent once it holds the document.
    auto window = CanonicalWindow::create(agent);

    // 13. Set up a window environment settings object with about:blank, realm execution context, null,
    //     topLevelCreationURL, and topLevelOrigin.
    window->set_up_a_window_environment_settings_object(move(environment_id));

    // 15. Let document be a new Document, with:
    //     origin: origin
    //     browsing context: browsingContext
    //     active sandboxing flag set: sandboxFlags
    //     is initial about:blank: true
    auto document = CanonicalDocument::create(document_id, URL::about_blank(), origin, browsing_context, window, sandbox_flags, CanonicalDocument::IsInitialAboutBlank::Yes);

    // 19. If creator is non-null:
    //     3. If creator's origin is same origin with creator's relevant settings object's top-level origin, then set
    //        document's opener policy to creator's browsing context's top-level browsing context's active document's
    //        opener policy.
    // NB: creator is fully active, so its relevant settings object's top-level origin is its top-level browsing
    //     context's active document's origin.
    if (creator) {
        auto top_level_document = creator->browsing_context().top_level_browsing_context().active_document();
        if (creator->origin().is_same_origin(top_level_document->origin()))
            document->set_opener_policy(top_level_document->opener_policy());
    }

    // 23. Make active document.
    document->make_active();

    // 24. Completely finish loading document.
    document->set_completely_loaded();

    // 25. Return browsingContext and document.
    return { browsing_context, document };
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#creating-a-new-top-level-browsing-context
// https://html.spec.whatwg.org/multipage/document-sequences.html#creating-a-new-browsing-context-group-and-document
CanonicalBrowsingContext::BrowsingContextAndDocument CanonicalBrowsingContext::create_a_new_top_level_browsing_context_and_document(Web::HTML::CrossProcessId document_id, Optional<URL::Origin> given_origin)
{
    // NB: A group is kept alive by the browsing contexts in its browsing context set, so creating a new browsing context
    //     group and document is folded in here, where the browsing context holding the group is returned.

    // 1. Let group be a new browsing context group.
    auto group = CanonicalBrowsingContextGroup::create();

    // 2. Append group to the user agent's browsing context group set.
    CanonicalBrowsingContextGroup::append_to_user_agent_browsing_context_group_set(*group);

    // 3. Let browsingContext and document be the result of creating a new browsing context and document with null, null, and group.
    auto browsing_context_and_document = create_a_new_browsing_context_and_document(nullptr, {}, *group, document_id, {}, move(given_origin));

    // 4. Append browsingContext to group.
    group->append(*browsing_context_and_document.browsing_context);

    // 5. Return group and document.
    // 2. Return group's browsing context set[0] and document.
    return browsing_context_and_document;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#creating-a-new-auxiliary-browsing-context
CanonicalBrowsingContext::BrowsingContextAndDocument CanonicalBrowsingContext::create_a_new_auxiliary_browsing_context_and_document(CanonicalNavigable& opener, Web::HTML::CrossProcessId document_id)
{
    // 1. Let openerTopLevelBrowsingContext be opener's top-level traversable's active browsing context.
    auto& opener_top_level_browsing_context = opener.top_level_traversable().active_browsing_context();

    // 2. Let group be openerTopLevelBrowsingContext's group.
    auto group = opener_top_level_browsing_context.group();

    // 3. Assert: group is non-null, as navigating invokes this directly.
    VERIFY(group);

    // 4. Let browsingContext and document be the result of creating a new browsing context and document with opener's active document, null, and group.
    auto browsing_context_and_document = create_a_new_browsing_context_and_document(&opener.active_document(), {}, *group, document_id, {});

    // 5. Set browsingContext's is auxiliary to true.
    browsing_context_and_document.browsing_context->m_is_auxiliary = true;

    // 6. Append browsingContext to group.
    group->append(*browsing_context_and_document.browsing_context);

    // 7. Set browsingContext's opener browsing context to opener.
    browsing_context_and_document.browsing_context->m_opener_browsing_context = opener.active_browsing_context();

    // 8. Set browsingContext's virtual browsing context group ID to openerTopLevelBrowsingContext's virtual browsing context group ID.
    browsing_context_and_document.browsing_context->m_virtual_browsing_context_group_id = opener_top_level_browsing_context.virtual_browsing_context_group_id();

    // 9. Set browsingContext's opener origin at creation to opener's active document's origin.
    browsing_context_and_document.browsing_context->m_opener_origin_at_creation = opener.active_document().origin();

    // 10. Return browsingContext and document.
    return browsing_context_and_document;
}

CanonicalBrowsingContext::~CanonicalBrowsingContext()
{
    remove();
}

void CanonicalBrowsingContext::remove()
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

CanonicalWindow& CanonicalBrowsingContext::active_window() const
{
    VERIFY(m_window_proxy_window);
    return *m_window_proxy_window.ptr();
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

RefPtr<CanonicalBrowsingContext> CanonicalBrowsingContext::opener_browsing_context() const
{
    if (!m_opener_browsing_context || m_opener_browsing_context->has_been_discarded())
        return nullptr;
    return m_opener_browsing_context;
}

void CanonicalBrowsingContext::set_opener_browsing_context(RefPtr<CanonicalBrowsingContext> opener)
{
    m_opener_browsing_context = opener;
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
