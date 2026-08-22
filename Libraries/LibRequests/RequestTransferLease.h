/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashFunctions.h>
#include <AK/Traits.h>
#include <AK/Types.h>

namespace Requests {

struct RequestTransferLeaseKey {
    int source_client_id { -1 };
    u64 source_request_id { 0 };

    bool operator==(RequestTransferLeaseKey const&) const = default;
};

}

namespace AK {

template<>
struct Traits<Requests::RequestTransferLeaseKey> : public DefaultTraits<Requests::RequestTransferLeaseKey> {
    static unsigned hash(Requests::RequestTransferLeaseKey const& key)
    {
        return pair_int_hash(Traits<int>::hash(key.source_client_id), Traits<u64>::hash(key.source_request_id));
    }
};

}
