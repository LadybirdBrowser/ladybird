/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/Vector.h>
#include <LibJS/Runtime/PropertyKey.h>
#include <LibURL/Origin.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/CrossOrigin/AbstractOperations.h>
#include <LibWeb/HTML/CrossOrigin/Reporting.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/origin.html#coop-check-access-report
void check_if_access_between_two_browsing_contexts_should_be_reported(
    BrowsingContext const& accessor,
    BrowsingContext const* accessed,
    JS::PropertyKey const& property_key,
    EnvironmentSettingsObject const& environment)
{
    // FIXME: Spec bug: https://github.com/whatwg/html/issues/10192
    if (!accessed)
        return;

    // 1. If propertyKey is not a cross-origin accessible window property name, then return.
    if (!is_cross_origin_accessible_window_property_name(property_key))
        return;

    // 2. Assert: accessor's active document and accessed's active document are both fully active.
    VERIFY(accessor.active_document()->is_fully_active());
    VERIFY(accessed->active_document()->is_fully_active());

    // 3. Let accessorTopDocument be accessor's top-level browsing context's active document.
    auto* accessor_top_document = accessor.top_level_browsing_context()->active_document();

    // 4. Let accessorInclusiveAncestorOrigins be the list obtained by taking the origin of the active document of each of accessor's active document's inclusive ancestor navigables.
    Vector<URL::Origin> accessor_inclusive_ancestor_origins = {};
    auto accessor_inclusive_ancestors = accessor.active_document()->ancestor_navigables();
    accessor_inclusive_ancestor_origins.ensure_capacity(accessor_inclusive_ancestors.size());
    for (auto const& ancestor : accessor_inclusive_ancestors) {
        VERIFY(ancestor != nullptr);
        VERIFY(ancestor->active_document() != nullptr);
        accessor_inclusive_ancestor_origins.append(ancestor->active_document()->origin());
    }

    // 5. Let accessedTopDocument be accessed's top-level browsing context's active document.
    VERIFY(accessed->top_level_browsing_context() != nullptr);
    auto* accessed_top_document = accessed->top_level_browsing_context()->active_document();

    // 6. Let accessedInclusiveAncestorOrigins be the list obtained by taking the origin of the active document of each of accessed's active document's inclusive ancestor navigables.
    Vector<URL::Origin> accessed_inclusive_ancestor_origins = {};
    auto accessed_inclusive_ancestors = accessed->active_document()->ancestor_navigables();
    accessed_inclusive_ancestor_origins.ensure_capacity(accessed_inclusive_ancestors.size());
    for (auto const& ancestor : accessed_inclusive_ancestors) {
        VERIFY(ancestor != nullptr);
        VERIFY(ancestor->active_document() != nullptr);
        accessed_inclusive_ancestor_origins.append(ancestor->active_document()->origin());
    }

    // 7. If any of accessorInclusiveAncestorOrigins are not same origin with accessorTopDocument's origin, or if any of accessedInclusiveAncestorOrigins are not same origin with accessedTopDocument's origin, then return.
    for (auto const& origin : accessor_inclusive_ancestor_origins)
        if (!origin.is_same_origin(accessor_top_document->origin()))
            return;
    for (auto const& origin : accessed_inclusive_ancestor_origins)
        if (!origin.is_same_origin(accessed_top_document->origin()))
            return;

    // 8. If accessor's top-level browsing context's virtual browsing context group ID is accessed's top-level browsing context's virtual browsing context group ID, then return.
    if (accessor.top_level_browsing_context()->virtual_browsing_context_group_id() == accessed->top_level_browsing_context()->virtual_browsing_context_group_id())
        return;

    // 9. Let accessorAccessedRelationship be a new accessor-accessed relationship with value none.
    auto accessor_accessed_relationship = AccessorAccessedRelationship::None;

    // 10. If accessed's top-level browsing context's opener browsing context is accessor or is an ancestor of accessor, then set accessorAccessedRelationship to accessor is opener.
    if (accessor.is_ancestor_of(*accessed->top_level_browsing_context()->opener_browsing_context()))
        accessor_accessed_relationship = AccessorAccessedRelationship::AccessorIsOpener;

    // 11. If accessor's top-level browsing context's opener browsing context is accessed or is an ancestor of accessed, then set accessorAccessedRelationship to accessor is openee.
    if (accessed->is_ancestor_of(*accessor.top_level_browsing_context()->opener_browsing_context()))
        accessor_accessed_relationship = AccessorAccessedRelationship::AccessorIsOpener;

    // 12. Queue violation reports for accesses, given accessorAccessedRelationship, accessorTopDocument's opener policy, accessedTopDocument's opener policy, accessor's active document's URL, accessed's active document's URL, accessor's top-level browsing context's initial URL, accessed's top-level browsing context's initial URL, accessor's active document's origin, accessed's active document's origin, accessor's top-level browsing context's opener origin at creation, accessed's top-level browsing context's opener origin at creation, accessorTopDocument's referrer, accessedTopDocument's referrer, propertyKey, and environment.
    (void)environment;
    (void)accessor_accessed_relationship;
}

}
