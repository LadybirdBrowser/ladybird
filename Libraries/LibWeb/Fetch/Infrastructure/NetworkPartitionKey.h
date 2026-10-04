/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibHTTP/NetworkIsolationKey.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#network-partition-key
// A network partition key is a tuple consisting of a site and null or an implementation-defined value.
// NB: Our implementation-defined value is the frame site and the flags of HTTP::NetworkIsolationKey, as in Chrome. A
//     key whose site is opaque is never shared with anything, so we represent it as no key at all.
using NetworkPartitionKey = HTTP::NetworkIsolationKey;

WEB_API Optional<NetworkPartitionKey> determine_the_network_partition_key(HTML::Environment const& environment);

WEB_API Optional<NetworkPartitionKey> determine_the_network_partition_key(Infrastructure::Request const& request);

}
