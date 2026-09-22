/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/DistinctNumeric.h>
#include <LibIPC/SenderClaim.h>

namespace Compositing {

// Identifies a page within one WebContent process. The UI process hands out page IDs and tracks which
// connection each one belongs to, so a page ID in a message from a helper process is a claim about the
// sender. The distinct type keeps that claim from being mistaken for an ordinary number.
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, PageId);

// A page ID in a message is checked against the receiver's own record of which pages the sender may act
// for.
template<typename Stub>
bool verify_sender_claim(Stub& stub, PageId const& page_id)
{
    if constexpr (requires {
                      { stub.may_act_for_page(page_id) } -> SameAs<bool>;
                  })
        return stub.may_act_for_page(page_id);
    else
        return true;
}

}

namespace IPC {

template<>
class SenderClaimReceiver<Compositing::PageId> {
public:
    // True when the connection a message came from was given the page the message names. A receiver
    // that hands out no page IDs is not the party the claim is made to, so it accepts every page ID.
    virtual bool may_act_for_page(Compositing::PageId) const { return true; }

protected:
    ~SenderClaimReceiver() = default;
};

}
