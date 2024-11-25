/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ContentSecurityPolicy/Directives/Names.h>
#include <LibWeb/ContentSecurityPolicy/PolicyList.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/NavigationParams.h>
#include <LibWeb/HTML/Scripting/Environments.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(NavigationParams);
GC_DEFINE_ALLOCATOR(NonFetchSchemeNavigationParams);

void NavigationParams::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(navigable);
    visitor.visit(request);
    visitor.visit(response);
    visitor.visit(fetch_controller);
    visitor.visit(reserved_environment);
    visitor.visit(policy_container);
}

void NonFetchSchemeNavigationParams::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(navigable);
}

// https://html.spec.whatwg.org/multipage/document-lifecycle.html#check-a-navigation-response's-adherence-to-x-frame-options
bool check_a_navigation_responses_adherence_to_x_frame_options(GC::Ptr<Fetch::Infrastructure::Response> response, Navigable* navigable, GC::Ref<ContentSecurityPolicy::PolicyList const> csp_list, URL::Origin destination_origin)
{
    // 1. If navigable is not a child navigable, then return true.
    if (!navigable->parent()) {
        return true;
    }

    // 2. For each policy of cspList:
    for (auto const policy : csp_list->policies()) {
        // 1. If policy's disposition is not "enforce", then continue.
        if (policy->disposition() != ContentSecurityPolicy::Policy::Disposition::Enforce)
            continue;

        // 2. If policy's directive set contains a frame-ancestors directive, then return true.
        auto maybe_frame_ancestors = policy->directives().find_if([](auto const& directive) {
            return directive->name() == ContentSecurityPolicy::Directives::Names::FrameAncestors;
        });

        if (!maybe_frame_ancestors.is_end())
            return true;
    }

    // 3. Let rawXFrameOptions be the result of getting, decoding, and splitting `X-Frame-Options` from response's header list.
    auto raw_x_frame_options = response->header_list()->get_decode_and_split("X-Frame-Options"sv.bytes());

    // 4. Let xFrameOptions be a new set.
    auto x_frame_options = AK::OrderedHashTable<String>();

    // 5. For each value of rawXFrameOptions, append value, converted to ASCII lowercase, to xFrameOptions.
    if (raw_x_frame_options.has_value()) {
        for (auto const& value : raw_x_frame_options.value()) {
            x_frame_options.set(value.to_ascii_lowercase());
        }
    }

    // 6. If xFrameOptions's size is greater than 1, and xFrameOptions contains any of "deny", "allowall", or "sameorigin", then return false.
    if (x_frame_options.size() > 1 && (x_frame_options.contains("deny"sv) || x_frame_options.contains("allowall"sv) || x_frame_options.contains("sameorigin"sv))) {
        return false;
    }

    // 7. If xFrameOptions's size is greater than 1, then return true.
    if (x_frame_options.size() > 1) {
        return true;
    }

    // 8. If xFrameOptions[0] is "deny", then return false.
    auto first_x_frame_option = x_frame_options.begin();
    if (!x_frame_options.is_empty() && *first_x_frame_option == "deny"sv) {
        return false;
    }

    // 9. If xFrameOptions[0] is "sameorigin", then:
    if (!x_frame_options.is_empty() && *first_x_frame_option == "sameorigin"sv) {
        // 1. Let containerDocument be navigable's container document.
        auto container_document = navigable->container_document();

        // 2. While containerDocument is not null:
        while (container_document) {
            // 1. If containerDocument's origin is not same origin with destinationOrigin, then return false.
            if (!container_document->origin().is_same_origin(destination_origin)) {
                return false;
            }

            // 2. Set containerDocument to containerDocument's container document.
            container_document = container_document->container_document();
        }
    }

    // 10. Return true.
    return true;
}

}
