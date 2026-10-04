/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Site.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/NetworkPartitionKey.h>
#include <LibWeb/Fetch/Request.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Scripting/Environments.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#determine-the-network-partition-key
Optional<NetworkPartitionKey> determine_the_network_partition_key(HTML::Environment const& environment)
{
    // 1. Let topLevelOrigin be environment’s top-level origin.
    auto top_level_origin = environment.top_level_origin;

    // 2. If topLevelOrigin is null, then set topLevelOrigin to environment’s top-level creation URL’s origin
    if (!top_level_origin.has_value())
        top_level_origin = environment.top_level_creation_url->origin();

    // 3. Assert: topLevelOrigin is an origin.

    // 4. Let topLevelSite be the result of obtaining a site, given topLevelOrigin.
    auto top_level_site = URL::Site::serialize_for_partitioning(*top_level_origin);
    if (!top_level_site.has_value())
        return {};

    // 5. Let secondKey be null or an implementation-defined value.
    // NB: The frame site is that of the environment's document or worker, or, for an environment that is not yet a
    //     settings object, that of the document it is being created for.
    auto const* settings_object = as_if<HTML::EnvironmentSettingsObject>(environment);
    auto frame_origin = settings_object ? settings_object->origin() : environment.creation_url.origin();

    // 6. Return (topLevelSite, secondKey).
    return NetworkPartitionKey {
        .top_level_site = top_level_site.release_value(),
        .frame_site = URL::Site::serialize_for_partitioning(frame_origin),
        .is_subframe_document = false,
        .is_cross_site_main_frame_navigation = false,
        .has_cross_site_ancestor = settings_object && settings_object->has_cross_site_ancestor(),
    };
}

// https://fetch.spec.whatwg.org/#request-determine-the-network-partition-key
Optional<NetworkPartitionKey> determine_the_network_partition_key(Infrastructure::Request const& request)
{
    auto current_url_origin = request.current_url().origin();

    // AD-HOC: A top-level navigation request belongs to the top-level document it creates, whose site is that of the
    //         request's current URL even after a cross-site redirect. Like Chrome, we flag one that another site
    //         initiated. Unlike Chrome, we also flag one that no document initiated, such as one from the address bar,
    //         as the process of a document of another site may be the one making it.
    if (request.mode() == Request::Mode::Navigate && request.destination() == Request::Destination::Document) {
        auto site = URL::Site::serialize_for_partitioning(current_url_origin);
        if (!site.has_value())
            return {};

        auto const& initiator_origin = request.top_level_navigation_initiator_origin();
        auto is_cross_site = !initiator_origin.has_value() || !initiator_origin->is_same_site(current_url_origin);

        return NetworkPartitionKey {
            .top_level_site = *site,
            .frame_site = move(site),
            .is_subframe_document = false,
            .is_cross_site_main_frame_navigation = is_cross_site,
        };
    }

    Optional<NetworkPartitionKey> key;

    // 1. If request’s reserved client is non-null, then return the result of determining the network partition key given request’s reserved client.
    if (auto reserved_client = request.reserved_client())
        key = determine_the_network_partition_key(*reserved_client);

    // 2. If request’s client is non-null, then return the result of determining the network partition key given request’s client.
    else if (auto client = request.client())
        key = determine_the_network_partition_key(*client);

    // 3. Return null.
    if (!key.has_value())
        return {};

    // AD-HOC: A navigation request of a child navigable belongs to the document it creates, whose site is that of the
    //         request's current URL. Like Chrome, we flag it as a subframe document. That document has a cross-site
    //         ancestor if its navigable's parent's active document has one, or is of another site.
    if (request.mode() == Request::Mode::Navigate) {
        key->frame_site = URL::Site::serialize_for_partitioning(current_url_origin);
        key->is_subframe_document = true;
        key->has_cross_site_ancestor = [&] {
            auto reserved_client = request.reserved_client();
            if (!reserved_client || !reserved_client->target_browsing_context)
                return false;
            auto const* document = reserved_client->target_browsing_context->active_document();
            auto navigable = document ? document->navigable() : nullptr;
            auto parent = navigable ? navigable->parent() : nullptr;
            if (!parent)
                return false;
            auto parent_origin = parent->active_document_origin();
            return parent->active_document_has_cross_site_ancestor() || !parent_origin.has_value() || !parent_origin->is_same_site(current_url_origin);
        }();
    }

    // AD-HOC: A request a worker makes for its own script has the worker's environment as its reserved client, which is
    //         not yet a settings object. The worker shares its creator's ancestors.
    else if (request.reserved_client() && !is<HTML::EnvironmentSettingsObject>(*request.reserved_client())) {
        if (auto client = request.client())
            key->has_cross_site_ancestor = client->has_cross_site_ancestor();
    }

    return key;
}

}
