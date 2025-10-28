/*
 * Copyright (c) 2025, the Ladybird Browser Contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/Vector.h>

namespace IPC {

// IPFS API client for operations not supported by HTTP gateway
// Communicates with local IPFS daemon via HTTP API (default: http://127.0.0.1:5001)
class IPFSAPIClient {
public:
    // Pin operations
    static ErrorOr<void> pin_add(ByteString const& cid);
    static ErrorOr<void> pin_remove(ByteString const& cid);
    static ErrorOr<Vector<ByteString>> pin_list();

    // Check if IPFS API is available
    static ErrorOr<void> check_api_available(ByteString host = "127.0.0.1"sv, u16 port = 5001);

private:
    static ErrorOr<ByteString> send_api_request(ByteString const& endpoint, ByteString const& method = "POST"sv);
};

}
