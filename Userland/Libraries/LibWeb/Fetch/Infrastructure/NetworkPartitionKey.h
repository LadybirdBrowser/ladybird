/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/HTML/Origin.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#network-partition-key
struct NetworkPartitionKey {
    HTML::Origin top_level_origin;
    // FIXME: See https://github.com/whatwg/fetch/issues/1035
    //     This is the document origin in other browsers
    void* second_key = nullptr;

    bool operator==(NetworkPartitionKey const&) const = default;
};

NetworkPartitionKey determine_the_network_partition_key(HTML::Environment const& environment);

Optional<NetworkPartitionKey> determine_the_network_partition_key(Infrastructure::Request const& request);

}

template<>
class AK::Traits<Web::Fetch::Infrastructure::NetworkPartitionKey> : public DefaultTraits<Web::Fetch::Infrastructure::NetworkPartitionKey> {
public:
    static unsigned hash(Web::Fetch::Infrastructure::NetworkPartitionKey const& partition_key)
    {
        return ::AK::Traits<Web::HTML::Origin>::hash(partition_key.top_level_origin);
    }
};
