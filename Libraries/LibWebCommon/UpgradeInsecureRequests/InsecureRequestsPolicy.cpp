/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/UpgradeInsecureRequests/InsecureRequestsPolicy.h>

namespace Web::UpgradeInsecureRequests {

void UpgradeInsecureNavigationsSet::insert(HostAndPort tuple)
{
    if (!contains(tuple))
        m_tuples.append(move(tuple));
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::UpgradeInsecureRequests::UpgradeInsecureNavigationsSet const& set)
{
    TRY(encoder.encode_size(set.tuples().size()));
    for (auto const& tuple : set.tuples()) {
        TRY(encoder.encode(tuple.host));
        TRY(encoder.encode(tuple.port));
    }
    return {};
}

template<>
ErrorOr<Web::UpgradeInsecureRequests::UpgradeInsecureNavigationsSet> decode(Decoder& decoder)
{
    Web::UpgradeInsecureRequests::UpgradeInsecureNavigationsSet set;
    auto size = TRY(decoder.decode_size());
    for (size_t i = 0; i < size; ++i) {
        auto host = TRY(decoder.decode<URL::Host>());
        auto port = TRY(decoder.decode<Optional<u16>>());
        set.insert({ move(host), port });
    }
    return set;
}

}
