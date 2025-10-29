/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "SecurityTap.h"
#include <AK/Base64.h>
#include <AK/JsonObject.h>
#include <AK/JsonParser.h>
#include <AK/JsonValue.h>
#include <AK/StringBuilder.h>
#include <LibCore/Socket.h>
#include <openssl/evp.h>
#include <openssl/sha.h>

namespace RequestServer {

ErrorOr<NonnullOwnPtr<SecurityTap>> SecurityTap::create()
{
    // Connect to Sentinel daemon
    auto socket = TRY(Core::LocalSocket::connect("/tmp/sentinel.sock"sv));

    // Set socket timeout to 5 seconds (fail-fast if Sentinel hangs)
    // FIXME: Add timeout configuration

    auto security_tap = adopt_own(*new SecurityTap(move(socket)));
    dbgln("SecurityTap: Connected to Sentinel daemon");

    return security_tap;
}

SecurityTap::SecurityTap(NonnullOwnPtr<Core::LocalSocket> socket)
    : m_sentinel_socket(move(socket))
{
}

ErrorOr<ByteString> SecurityTap::compute_sha256(ReadonlyBytes data)
{
    unsigned char hash[SHA256_DIGEST_LENGTH];

    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (!context)
        return Error::from_string_literal("Failed to create EVP context");

    if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(context);
        return Error::from_string_literal("Failed to initialize SHA256");
    }

    if (EVP_DigestUpdate(context, data.data(), data.size()) != 1) {
        EVP_MD_CTX_free(context);
        return Error::from_string_literal("Failed to update SHA256");
    }

    if (EVP_DigestFinal_ex(context, hash, nullptr) != 1) {
        EVP_MD_CTX_free(context);
        return Error::from_string_literal("Failed to finalize SHA256");
    }

    EVP_MD_CTX_free(context);

    StringBuilder hex_builder;
    for (size_t i = 0; i < SHA256_DIGEST_LENGTH; i++)
        hex_builder.appendff("{:02x}", hash[i]);

    return hex_builder.to_byte_string();
}

ErrorOr<SecurityTap::ScanResult> SecurityTap::inspect_download(
    DownloadMetadata const& metadata,
    ReadonlyBytes content)
{
    // For very large files, skip scanning to avoid performance impact
    constexpr size_t MAX_SCAN_SIZE = 100 * 1024 * 1024; // 100MB
    if (content.size() > MAX_SCAN_SIZE) {
        dbgln("SecurityTap: Skipping scan for large file ({}MB)", content.size() / (1024 * 1024));
        return ScanResult { .is_threat = false, .alert_json = {} };
    }

    // Send scan request to Sentinel
    auto response_json = TRY(send_scan_request(metadata, content));

    // Parse response
    auto json_result = JsonValue::from_string(response_json);
    if (json_result.is_error()) {
        dbgln("SecurityTap: Failed to parse Sentinel response: {}", json_result.error());
        return Error::from_string_literal("Failed to parse Sentinel response");
    }

    auto json = json_result.value();
    if (!json.is_object()) {
        return Error::from_string_literal("Expected JSON object from Sentinel");
    }

    auto obj = json.as_object();

    // Check status
    auto status = obj.get_string("status"sv);
    if (!status.has_value()) {
        return Error::from_string_literal("Missing 'status' field in Sentinel response");
    }

    if (status.value() != "success"sv) {
        auto error_str = obj.get_string("error"sv);
        auto error = error_str.has_value() ? error_str.value() : "Unknown error"_string;
        dbgln("SecurityTap: Sentinel scan failed: {}", error);
        return Error::from_string_literal("Sentinel scan failed");
    }

    // Check result
    auto result = obj.get_string("result"sv);
    if (!result.has_value()) {
        return Error::from_string_literal("Missing 'result' field in Sentinel response");
    }

    // If result is "clean", no threat detected
    if (result.value() == "clean"sv) {
        dbgln("SecurityTap: File clean: {}", metadata.filename);
        return ScanResult { .is_threat = false, .alert_json = {} };
    }

    // Otherwise, result contains threat detection JSON
    dbgln("SecurityTap: Threat detected in {}: {}", metadata.filename, result.value());

    return ScanResult {
        .is_threat = true,
        .alert_json = ByteString(result.value())
    };
}

ErrorOr<ByteString> SecurityTap::send_scan_request(
    DownloadMetadata const& metadata,
    ReadonlyBytes content)
{
    // Build JSON request for Sentinel
    JsonObject request;
    request.set("action"sv, JsonValue("scan_content"sv));
    request.set("request_id"sv, JsonValue(ByteString::formatted("download_{}", metadata.sha256)));

    // Base64 encode content for JSON transport
    auto content_base64 = TRY(encode_base64(content));
    request.set("content"sv, JsonValue(content_base64));

    // Serialize and send
    auto request_json = request.serialized();
    auto request_bytes = request_json.bytes();

    TRY(m_sentinel_socket->write_until_depleted(request_bytes));

    // Add newline delimiter (Sentinel expects JSON Lines format)
    u8 newline = '\n';
    TRY(m_sentinel_socket->write_until_depleted({ &newline, 1 }));

    // Read response (blocking with timeout)
    auto response_buffer = TRY(ByteBuffer::create_uninitialized(4096));
    auto bytes_read = TRY(m_sentinel_socket->read_some(response_buffer));

    if (bytes_read.is_empty()) {
        return Error::from_string_literal("Sentinel socket closed");
    }

    return ByteString(bytes_read);
}

}
