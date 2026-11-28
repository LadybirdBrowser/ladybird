/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibHTTP/NetworkIsolationKey.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Infrastructure/ClientContextSnapshot.h>
#include <LibWebCommon/HTML/Scripting/SerializedEnvironment.h>

namespace Web::Fetch::Infrastructure {

class Request;

// https://fetch.spec.whatwg.org/#network-partition-key
// A network partition key is a tuple consisting of a site and null or an implementation-defined value.
// NB: Our implementation-defined value is the frame site and the flags of HTTP::NetworkIsolationKey, as in Chrome. A
//     key whose site is opaque is never shared with anything, so we represent it as no key at all.
using NetworkPartitionKey = HTTP::NetworkIsolationKey;

// The settings object context is given when the environment is a settings object.
WEB_API Optional<NetworkPartitionKey> determine_the_network_partition_key(HTML::SerializedEnvironment const&, Optional<SettingsObjectContext> const&);

WEB_API Optional<NetworkPartitionKey> determine_the_network_partition_key(Infrastructure::Request const& request);

}
