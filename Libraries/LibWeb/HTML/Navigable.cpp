/*
 * Copyright (c) 2026-present, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Crypto/Crypto.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/SandboxingFlagSet.h>
#include <LibWeb/HTML/SourceSnapshotParams.h>
#include <LibWeb/HTML/UserNavigationInvolvement.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::HTML {

Navigable::~Navigable() = default;

void Navigable::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_parent);
}

bool Navigable::is_ancestor_of(Navigable const& other) const
{
    for (auto ancestor = other.parent(); ancestor; ancestor = ancestor->parent()) {
        if (ancestor.ptr() == this)
            return true;
    }
    return false;
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
    return continue_navigation_in_active_document_agent(
        move(params),
        csp_navigation_type,
        source_snapshot_params,
        move(initiator_origin_snapshot),
        move(initiator_base_url_snapshot));
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

}
