/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibIPC/Forward.h>
#include <LibURL/Host.h>
#include <LibWebCommon/Export.h>

namespace Web::UpgradeInsecureRequests {

// https://w3c.github.io/webappsec-upgrade-insecure-requests/#insecure-requests-policy
enum class InsecureRequestsPolicy : u8 {
    DoNotUpgrade,
    Upgrade,
};

// https://w3c.github.io/webappsec-upgrade-insecure-requests/#upgrade-insecure-navigations-set
// A (host, port) tuple, which the upgrade insecure navigations set contains.
struct HostAndPort {
    URL::Host host;
    Optional<u16> port;

    bool operator==(HostAndPort const&) const = default;
};

// https://w3c.github.io/webappsec-upgrade-insecure-requests/#upgrade-insecure-navigations-set
class WEBCOMMON_API UpgradeInsecureNavigationsSet {
public:
    bool contains(HostAndPort const& tuple) const { return m_tuples.contains_slow(tuple); }
    void insert(HostAndPort);

    Vector<HostAndPort> const& tuples() const { return m_tuples; }

private:
    Vector<HostAndPort> m_tuples;
};

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::UpgradeInsecureRequests::UpgradeInsecureNavigationsSet const&);

template<>
WEBCOMMON_API ErrorOr<Web::UpgradeInsecureRequests::UpgradeInsecureNavigationsSet> decode(Decoder&);

}
