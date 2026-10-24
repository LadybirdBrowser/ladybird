/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/DistinctNumeric.h>
#include <LibIPC/SenderClaim.h>

namespace Web::StorageAPI {

// Identifies one storage area: the map a storage bottle for one storage key keeps, in the process that owns it. The
// UI process hands out area IDs when a WebContent process connects to an area and tracks which connection each one
// went to, so an area ID in a message from a helper process is a claim about the sender. The distinct type keeps that
// claim from being mistaken for an ordinary number.
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, StorageAreaId);

// An area ID in a message is checked against the receiver's own record of which areas the sender was connected to.
template<typename Stub>
bool verify_sender_claim(Stub& stub, StorageAreaId const& area_id)
{
    if constexpr (requires {
                      { stub.may_act_for_storage_area(area_id) } -> SameAs<bool>;
                  })
        return stub.may_act_for_storage_area(area_id);
    else
        return true;
}

}

namespace IPC {

template<>
class SenderClaimReceiver<Web::StorageAPI::StorageAreaId> {
public:
    // True when the connection a message came from was given the storage area the message names. A receiver that
    // hands out no area IDs is not the party the claim is made to, so it accepts every area ID.
    virtual bool may_act_for_storage_area(Web::StorageAPI::StorageAreaId) const { return true; }

protected:
    ~SenderClaimReceiver() = default;
};

}
