/*
 * Copyright (c) 2026-present, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Crypto/Crypto.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/HTML/RemoteNavigable.h>
#include <LibWeb/HTML/SandboxingFlagSet.h>
#include <LibWeb/HTML/SourceSnapshotParams.h>
#include <LibWeb/HTML/UserNavigationInvolvement.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::HTML {

Navigable::Navigable(GC::Ref<Page> page)
    : m_page(page)
{
}

Navigable::~Navigable() = default;

void Navigable::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_parent);
    visitor.visit(m_container);
    visitor.visit(m_page);
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#nav-container
GC::Ptr<NavigableContainer> Navigable::container() const
{
    // The container of a navigable navigable is the navigable container whose nested navigable is navigable, or null if there is no such element.
    return m_container;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#nav-container-document
GC::Ptr<DOM::Document> Navigable::container_document() const
{
    auto container = this->container();

    // 1. If navigable's container is null, then return null.
    if (!container)
        return nullptr;

    // 2. Return navigable's container's node document.
    return container->document();
}

bool Navigable::is_ancestor_of(Navigable const& other) const
{
    for (auto ancestor = other.parent(); ancestor; ancestor = ancestor->parent()) {
        if (ancestor.ptr() == this)
            return true;
    }
    return false;
}

// The currently focused area of the walk from this navigable down, as far as this page holds it. Other processes can
// host the documents on the way to the tab's focused navigable. The walk then resumes at the nearest navigable above the
// focused navigable that this page hosts, or ends at the nearest container this page holds.
GC::Ptr<DOM::Node> Navigable::currently_focused_area_shown_by_focused_navigable()
{
    if (!page().client().has_focus())
        return nullptr;

    auto focused_navigable = page().focused_navigable();
    if (!focused_navigable || (focused_navigable.ptr() != this && !is_ancestor_of(*focused_navigable)))
        return nullptr;

    for (auto navigable = focused_navigable; navigable; navigable = navigable->parent()) {
        if (auto* local_navigable = as_if<LocalNavigable>(*navigable))
            return local_navigable->currently_focused_area();
        if (navigable.ptr() == this)
            return nullptr;
        if (auto container = navigable->container())
            return container;
    }
    return nullptr;
}

// AD-HOC: Destroying a child navigable detaches its subtree from the traversable, although the documents in that
//         subtree keep their navigable until the destruction's unload steps complete. The spec destroys each of those
//         documents from a queued task, so the node navigable of a removed iframe's documents stays non-null — leaving
//         them reporting their child navigables long after the removal. Other engines detach the whole subtree at once.
bool Navigable::is_in_a_destroyed_subtree() const
{
    for (auto navigable = GC::Ptr<Navigable const> { this }; navigable; navigable = navigable->parent()) {
        if (navigable->has_been_destroyed())
            return true;
    }
    return false;
}

GC::Ptr<Navigable> Navigable::find(CrossProcessId id)
{
    // AD-HOC: Step 3 of destroy a child navigable, after which the navigable is no longer a child, is deferred until
    //         its document has unloaded. A destroyed navigable is not found, although
    //         Document::document_tree_child_navigables() still includes it until then.
    if (has_been_destroyed())
        return nullptr;
    if (this->id() == id)
        return this;

    for (auto* container : NavigableContainer::all_instances()) {
        auto child = container->content_navigable();
        if (!child || !active_document_is(container->document()))
            continue;
        if (auto navigable = child->find(id))
            return navigable;
    }
    return nullptr;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#nav-traversable
GC::Ref<Navigable> Navigable::traversable_navigable()
{
    // 1. Let navigable be inputNavigable.
    GC::Ref<Navigable> navigable = *this;

    // 2. While navigable is not a traversable navigable, set navigable to navigable's parent.
    while (!navigable->is_traversable())
        navigable = *navigable->parent();

    // 3. Return navigable.
    return navigable;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#nav-top
GC::Ref<Navigable> Navigable::top_level_traversable()
{
    // 1. Let navigable be inputNavigable.
    GC::Ref<Navigable> navigable = *this;

    // 2. While navigable's parent is not null, set navigable to navigable's parent.
    while (navigable->parent())
        navigable = *navigable->parent();

    // 3. Return navigable.
    return navigable;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#navigate
WebIDL::ExceptionOr<void> Navigable::navigate(NavigateParams params)
{
    auto source_document = params.source_document;

    // 1. Let cspNavigationType be "form-submission" if formDataEntryList is non-null; otherwise "other".
    auto csp_navigation_type = params.form_data_entry_list.has_value()
        ? ContentSecurityPolicy::Directives::Directive::NavigationType::FormSubmission
        : ContentSecurityPolicy::Directives::Directive::NavigationType::Other;

    // 2. Let sourceSnapshotParams be the result of snapshotting source snapshot params given sourceDocument.
    auto source_snapshot_params = snapshot_source_snapshot_params(source_document);

    // 3. Let initiatorOriginSnapshot be a new opaque origin.
    auto initiator_origin_snapshot = URL::Origin::create_opaque();

    // 4. Let initiatorBaseURLSnapshot be about:blank.
    auto initiator_base_url_snapshot = URL::about_blank();

    // 5. If sourceDocument is null:
    if (!source_document) {
        // 1. Assert: userInvolvement is "browser UI".
        VERIFY(params.user_involvement == UserNavigationInvolvement::BrowserUI);

        // 2. If url's scheme is "javascript", then set initiatorOriginSnapshot to navigable's active document's origin.
        if (params.url.scheme() == "javascript"sv) {
            auto origin = active_document_origin();
            if (!origin.has_value())
                return {};
            initiator_origin_snapshot = origin.release_value();
        }
    }
    // 6. Otherwise:
    else {
        // 1. Assert: userInvolvement is not "browser UI".
        VERIFY(params.user_involvement != UserNavigationInvolvement::BrowserUI);

        // 2. If sourceDocument's node navigable is not allowed by sandboxing to navigate navigable given
        //    sourceSnapshotParams:
        if (!source_document->navigable()->allowed_by_sandboxing_to_navigate(*this, source_snapshot_params)) {
            // 1. If exceptionsEnabled is true, then throw a "SecurityError" DOMException.
            if (params.exceptions_enabled)
                return WebIDL::SecurityError::create("Source document's node navigable is not allowed to navigate"_utf16);

            // 2. Return.
            return {};
        }

        // 3. Set initiatorOriginSnapshot to sourceDocument's origin.
        initiator_origin_snapshot = source_document->origin();

        // 4. Set initiatorBaseURLSnapshot to sourceDocument's document base URL.
        initiator_base_url_snapshot = source_document->base_url();
    }

    // AD-HOC: Nothing the browser process can see names this URL's blob URL entry until the navigation commits, so
    //         ask it to keep the entry until then.
    if (auto const& blob_url_entry = params.url.blob_url_entry(); blob_url_entry.has_value() && source_document) {
        if (auto const* blob = blob_url_entry->object.get_pointer<URL::BlobURLEntry::Blob>())
            source_document->page().client().page_did_retain_blob_url_token(id(), blob->token);
    }

    // 7. Let navigationId be the result of generating a random UUID.
    // NB: Generating the ID is the responsibility of whichever process requested the navigation. A load
    //     requested by the UI process carries the ID the UI generated when it recorded the navigation.
    params.navigation_id = params.navigation_id.value_or_lazy_evaluated([] {
        auto uuid = Crypto::generate_random_uuid();
        return Utf16String::from_ascii_without_validation(uuid.bytes());
    });

    // 8. If the surrounding agent is equal to navigable's active document's relevant agent, then continue these
    //    steps. Otherwise, queue a global task on the navigation and traversal task source given navigable's active
    //    window to continue these steps.
    // NB: Steps 1 to 7 took everything the remaining steps need from sourceDocument, so they carry no reference to it.
    return continue_navigation_in_active_document_agent({
        .url = move(params.url),
        .document_resource = move(params.document_resource),
        .response = params.response,
        .history_handling = params.history_handling,
        .navigation_api_state = move(params.navigation_api_state),
        .form_data_entry_list = move(params.form_data_entry_list),
        .referrer_policy = params.referrer_policy,
        .user_involvement = params.user_involvement,
        .navigation_id = params.navigation_id.release_value(),
        .source_element = params.source_element,
        .initial_insertion = params.initial_insertion,
        .api_method_tracker = params.api_method_tracker,
        .csp_navigation_type = csp_navigation_type,
        .source_snapshot_params = source_snapshot_params,
        .initiator_origin_snapshot = move(initiator_origin_snapshot),
        .initiator_base_url_snapshot = move(initiator_base_url_snapshot),
    });
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#allowed-to-navigate
bool Navigable::allowed_by_sandboxing_to_navigate(Navigable const& target, SourceSnapshotParams const& source_snapshot_params) const
{
    auto& source = *this;

    // A navigable source is allowed by sandboxing to navigate a second navigable target,
    // given a source snapshot params sourceSnapshotParams, if the following steps return true:

    // 1. If source is target, then return true.
    if (&source == &target)
        return true;

    // 2. If source is an ancestor of target, then return true.
    if (source.is_ancestor_of(target))
        return true;

    // 3. If target is an ancestor of source, then:
    if (target.is_ancestor_of(source)) {

        // 1. If target is not a top-level traversable, then return true.
        if (!target.is_top_level_traversable())
            return true;

        // 2. If sourceSnapshotParams's has transient activation is true, and sourceSnapshotParams's sandboxing flags's
        //    sandboxed top-level navigation with user activation browsing context flag is set, then return false.
        if (source_snapshot_params.has_transient_activation && has_flag(source_snapshot_params.sandboxing_flags, SandboxingFlagSet::SandboxedTopLevelNavigationWithUserActivation))
            return false;

        // 3. If sourceSnapshotParams's has transient activation is false, and sourceSnapshotParams's sandboxing flags's
        //    sandboxed top-level navigation without user activation browsing context flag is set, then return false.
        if (!source_snapshot_params.has_transient_activation && has_flag(source_snapshot_params.sandboxing_flags, SandboxingFlagSet::SandboxedTopLevelNavigationWithoutUserActivation))
            return false;

        // 4. Return true.
        return true;
    }

    // 4. If target is a top-level traversable:
    if (target.is_top_level_traversable()) {
        // FIXME: 1. If source is the one permitted sandboxed navigator of target, then return true.

        // 2. If sourceSnapshotParams's sandboxing flags's sandboxed navigation browsing context flag is set, then return false.
        if (has_flag(source_snapshot_params.sandboxing_flags, SandboxingFlagSet::SandboxedNavigation))
            return false;

        // 3. Return true.
        return true;
    }

    // 5. If sourceSnapshotParams's sandboxing flags's sandboxed navigation browsing context flag is set, then return false.
    // 6. Return true.
    return !has_flag(source_snapshot_params.sandboxing_flags, SandboxingFlagSet::SandboxedNavigation);
}

GC::Ptr<Navigable> navigable_with_id_in_any_page(Page const& preferred_page, CrossProcessId id)
{
    // A local navigable is the navigable itself, so scanning those first stands one in another page ahead of any
    // proxy of it.
    GC::Ptr<Navigable> match;
    auto consider = [&](Navigable& navigable) {
        if (navigable.id() != id || navigable.has_been_destroyed())
            return false;
        if (&navigable.page() == &preferred_page) {
            match = navigable;
            return true;
        }
        if (!match)
            match = navigable;
        return false;
    };

    for (auto& navigable : all_local_navigables()) {
        if (consider(navigable))
            return match;
    }
    for (auto& navigable : all_remote_navigables()) {
        if (consider(navigable))
            return match;
    }
    return match;
}

}
