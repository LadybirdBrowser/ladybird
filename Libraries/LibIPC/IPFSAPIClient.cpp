/*
 * Copyright (c) 2025, the Ladybird Browser Contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Socket.h>
#include <LibIPC/IPFSAPIClient.h>

namespace IPC {

ErrorOr<void> IPFSAPIClient::check_api_available(ByteString host, u16 port)
{
    // Try to connect to IPFS API endpoint
    auto socket_result = Core::TCPSocket::connect(host, port);

    if (socket_result.is_error()) {
        dbgln("IPFSAPIClient: Cannot connect to IPFS API at {}:{} - {}",
            host, port, socket_result.error());
        return Error::from_string_literal("Cannot connect to IPFS daemon API. Is IPFS running?");
    }

    dbgln("IPFSAPIClient: IPFS daemon API is available at {}:{}", host, port);
    return {};
}

ErrorOr<ByteString> IPFSAPIClient::send_api_request(ByteString const& endpoint, ByteString const& method)
{
    // HTTP request to IPFS API
    // Example: POST http://127.0.0.1:5001/api/v0/pin/add?arg=QmHash

    // For MVP, we'll use simple TCP socket + HTTP
    // TODO: Use libcurl or LibHTTP for proper HTTP client

    auto socket_result = Core::TCPSocket::connect("127.0.0.1"sv, 5001);
    if (socket_result.is_error())
        return Error::from_string_literal("Cannot connect to IPFS API");

    auto socket = socket_result.release_value();

    // Build HTTP request
    StringBuilder request_builder;
    request_builder.appendff("{} {} HTTP/1.1\r\n", method, endpoint);
    request_builder.append("Host: 127.0.0.1:5001\r\n"sv);
    request_builder.append("Connection: close\r\n"sv);
    request_builder.append("\r\n"sv);

    auto request = request_builder.to_byte_string();
    TRY(socket->write_until_depleted(request.bytes()));

    // Read response
    auto response = TRY(socket->read_until_eof());

    // Parse response (simple approach - just check for 200 OK)
    auto response_string = ByteString::copy(response);
    if (!response_string.contains("200 OK"sv))
        return Error::from_string_literal("IPFS API request failed");

    return response_string;
}

ErrorOr<void> IPFSAPIClient::pin_add(ByteString const& cid)
{
    dbgln("IPFSAPIClient: Pinning CID: {}", cid);

    // POST /api/v0/pin/add?arg=QmHash
    auto endpoint = ByteString::formatted("/api/v0/pin/add?arg={}", cid);
    TRY(send_api_request(endpoint));

    dbgln("IPFSAPIClient: Successfully pinned CID: {}", cid);
    return {};
}

ErrorOr<void> IPFSAPIClient::pin_remove(ByteString const& cid)
{
    dbgln("IPFSAPIClient: Unpinning CID: {}", cid);

    // POST /api/v0/pin/rm?arg=QmHash
    auto endpoint = ByteString::formatted("/api/v0/pin/rm?arg={}", cid);
    TRY(send_api_request(endpoint));

    dbgln("IPFSAPIClient: Successfully unpinned CID: {}", cid);
    return {};
}

ErrorOr<Vector<ByteString>> IPFSAPIClient::pin_list()
{
    dbgln("IPFSAPIClient: Listing pinned CIDs");

    // POST /api/v0/pin/ls
    auto response = TRY(send_api_request("/api/v0/pin/ls"sv));

    // Parse JSON response to extract CIDs
    // TODO: Proper JSON parsing
    // For MVP, return empty list
    Vector<ByteString> pins;

    dbgln("IPFSAPIClient: Found {} pinned CIDs", pins.size());
    return pins;
}

}
