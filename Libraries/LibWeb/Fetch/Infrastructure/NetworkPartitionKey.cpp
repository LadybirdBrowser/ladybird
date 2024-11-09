/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Fetch/Infrastructure/NetworkPartitionKey.h>
#include <LibWeb/Fetch/Request.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#determine-the-network-partition-key
NetworkPartitionKey determine_the_network_partition_key(HTML::Environment const& environment)
{
    // 1. Let topLevelOrigin be environment’s top-level origin.
    auto top_level_origin = environment.top_level_origin;

    // FIXME: 2. If topLevelOrigin is null, then set topLevelOrigin to environment’s top-level creation URL’s origin
    // This field is supposed to be nullable

    // 3. Assert: topLevelOrigin is an origin.

    // FIXME: 4. Let topLevelSite be the result of obtaining a site, given topLevelOrigin.

    // 5. Let secondKey be null or an implementation-defined value.
    void* second_key = nullptr;

    // 6. Return (topLevelSite, secondKey).
    return { top_level_origin, second_key };
}

// https://fetch.spec.whatwg.org/#request-determine-the-network-partition-key
Optional<NetworkPartitionKey> determine_the_network_partition_key(Infrastructure::Request const& request)
{
    // 1. If request’s reserved client is non-null, then return the result of determining the network partition key given request’s reserved client.
    if (auto reserved_client = request.reserved_client())
        return determine_the_network_partition_key(*reserved_client);

    // 2. If request’s client is non-null, then return the result of determining the network partition key given request’s client.
    if (auto client = request.client())
        return determine_the_network_partition_key(*client);

    return {};
}

}
