/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibIPC/Forward.h>
#include <LibIPC/HandleBroker.h>

namespace IPC {

enum class HandleBrokerOperation : u8 {
    Park,
    Redeem,
    ForgetChannel,
};

// A handle is named by its value in the requester's process; the Browser copies it out of or into that process.
struct HandleBrokerRequest {
    HandleBrokerOperation operation { HandleBrokerOperation::Park };
    HandleBrokerToken token { 0 };
    HandleBrokerChannel channel { 0 };
    Vector<u64> handle_values;
};

struct HandleBrokerResponse {
    HandleBrokerOperation operation { HandleBrokerOperation::Park };
    // An errno value, or zero.
    i32 error { 0 };
    HandleBrokerToken token { 0 };
    Vector<u64> handle_values;
};

template<>
ErrorOr<void> encode(Encoder&, HandleBrokerRequest const&);

template<>
ErrorOr<HandleBrokerRequest> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, HandleBrokerResponse const&);

template<>
ErrorOr<HandleBrokerResponse> decode(Decoder&);

}
