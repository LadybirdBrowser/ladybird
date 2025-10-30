/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "SentinelServer.h"
#include <AK/Base64.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonParser.h>
#include <AK/JsonValue.h>
#include <LibCore/File.h>
#include <yara.h>

// Undefine YARA macros that conflict with AK classes
#ifdef get_string
#    undef get_string
#endif
#ifdef set
#    undef set
#endif

namespace Sentinel {

static YR_RULES* s_yara_rules = nullptr;

static ErrorOr<void> initialize_yara()
{
    dbgln("Sentinel: Initializing YARA");

    int result = yr_initialize();
    if (result != ERROR_SUCCESS)
        return Error::from_string_literal("Failed to initialize YARA");

    YR_COMPILER* compiler = nullptr;
    result = yr_compiler_create(&compiler);
    if (result != ERROR_SUCCESS) {
        yr_finalize();
        return Error::from_string_literal("Failed to create YARA compiler");
    }

    // Load default rules from Services/Sentinel/rules/default.yar
    ByteString rules_path = "/home/rbsmith4/ladybird/Services/Sentinel/rules/default.yar"sv;
    auto rules_file = Core::File::open(rules_path, Core::File::OpenMode::Read);

    if (rules_file.is_error()) {
        dbgln("Sentinel: Failed to open YARA rules at {}: {}", rules_path, rules_file.error());
        yr_compiler_destroy(compiler);
        yr_finalize();
        return Error::from_string_literal("Failed to open YARA rules file");
    }

    auto rules_content = rules_file.value()->read_until_eof();
    if (rules_content.is_error()) {
        dbgln("Sentinel: Failed to read YARA rules: {}", rules_content.error());
        yr_compiler_destroy(compiler);
        yr_finalize();
        return Error::from_string_literal("Failed to read YARA rules file");
    }

    // Add rules to compiler
    result = yr_compiler_add_string(compiler,
        reinterpret_cast<char const*>(rules_content.value().data()),
        nullptr);

    if (result != 0) {
        dbgln("Sentinel: Failed to compile YARA rules");
        yr_compiler_destroy(compiler);
        yr_finalize();
        return Error::from_string_literal("Failed to compile YARA rules");
    }

    // Get compiled rules
    result = yr_compiler_get_rules(compiler, &s_yara_rules);
    if (result != ERROR_SUCCESS) {
        yr_compiler_destroy(compiler);
        yr_finalize();
        return Error::from_string_literal("Failed to get compiled YARA rules");
    }

    yr_compiler_destroy(compiler);

    dbgln("Sentinel: YARA initialized successfully");
    return {};
}

ErrorOr<NonnullOwnPtr<SentinelServer>> SentinelServer::create()
{
    // Initialize YARA rules
    TRY(initialize_yara());

    auto server = Core::LocalServer::construct();
    if (!server->listen("/tmp/sentinel.sock"sv))
        return Error::from_string_literal("Failed to listen on /tmp/sentinel.sock");

    auto sentinel_server = adopt_own(*new SentinelServer(move(server)));
    return sentinel_server;
}

SentinelServer::SentinelServer(NonnullRefPtr<Core::LocalServer> server)
    : m_server(move(server))
{
    m_server->on_accept = [this](NonnullOwnPtr<Core::LocalSocket> client_socket) {
        handle_client(move(client_socket));
    };
}

void SentinelServer::handle_client(NonnullOwnPtr<Core::LocalSocket> socket)
{
    dbgln("Sentinel: Client connected");

    socket->on_ready_to_read = [this, sock = socket.ptr()]() {
        auto buffer_result = ByteBuffer::create_uninitialized(4096);
        if (buffer_result.is_error()) {
            dbgln("Sentinel: Failed to allocate buffer");
            return;
        }

        auto buffer = buffer_result.release_value();
        auto read_result = sock->read_some(buffer);

        if (read_result.is_error()) {
            dbgln("Sentinel: Read error: {}", read_result.error());
            return;
        }

        auto bytes_read = read_result.value();
        if (bytes_read.is_empty()) {
            dbgln("Sentinel: Client disconnected");
            return;
        }

        String message = MUST(String::from_utf8(StringView(reinterpret_cast<char const*>(bytes_read.data()), bytes_read.size())));
        auto process_result = process_message(*sock, message);

        if (process_result.is_error()) {
            dbgln("Sentinel: Failed to process message: {}", process_result.error());
        }
    };

    m_clients.append(move(socket));
}

ErrorOr<void> SentinelServer::process_message(Core::LocalSocket& socket, String const& message)
{
    // Parse JSON message
    auto json_result = JsonValue::from_string(message);
    if (json_result.is_error())
        return Error::from_string_literal("Invalid JSON");

    auto json = json_result.value();
    if (!json.is_object())
        return Error::from_string_literal("Expected JSON object");

    auto obj = json.as_object();
    auto action = obj.get_string("action"sv);
    if (!action.has_value())
        return Error::from_string_literal("Missing 'action' field");

    JsonObject response;
    auto request_id = obj.get_string("request_id"sv);
    response.set("request_id"sv, request_id.has_value() ? JsonValue(request_id.value()) : JsonValue("unknown"sv));

    if (action.value() == "scan_file"sv) {
        auto file_path = obj.get_string("file_path"sv);
        if (!file_path.has_value()) {
            response.set("status"sv, "error"sv);
            response.set("error"sv, "Missing 'file_path' field"sv);
        } else {
            // Convert String to ByteString for scan_file
            auto result = scan_file(ByteString(file_path.value().bytes_as_string_view()));
            if (result.is_error()) {
                response.set("status"sv, "error"sv);
                response.set("error"sv, result.error().string_literal());
            } else {
                response.set("status"sv, "success"sv);
                response.set("result"sv, MUST(String::from_utf8(StringView(result.value()))));
            }
        }
    } else if (action.value() == "scan_content"sv) {
        auto content = obj.get_string("content"sv);
        if (!content.has_value()) {
            response.set("status"sv, "error"sv);
            response.set("error"sv, "Missing 'content' field"sv);
        } else {
            // Decode base64 content before scanning
            auto decoded_result = decode_base64(content.value().bytes_as_string_view());
            if (decoded_result.is_error()) {
                response.set("status"sv, "error"sv);
                response.set("error"sv, "Failed to decode base64 content"sv);
            } else {
                auto result = scan_content(decoded_result.value().bytes());
                if (result.is_error()) {
                    response.set("status"sv, "error"sv);
                    response.set("error"sv, result.error().string_literal());
                } else {
                    response.set("status"sv, "success"sv);
                    response.set("result"sv, MUST(String::from_utf8(StringView(result.value()))));
                }
            }
        }
    } else {
        response.set("status"sv, "error"sv);
        response.set("error"sv, "Unknown action"sv);
    }

    auto response_str = response.serialized();
    TRY(socket.write_until_depleted(response_str.bytes()));

    return {};
}

ErrorOr<ByteString> SentinelServer::scan_file(ByteString const& file_path)
{
    // Read file content
    auto file = TRY(Core::File::open(file_path, Core::File::OpenMode::Read));
    auto content = TRY(file->read_until_eof());

    return scan_content(content.bytes());
}

struct YaraMatchData {
    Vector<ByteString> rule_names;
    Vector<JsonObject> rule_details;
};

static int yara_callback([[maybe_unused]] YR_SCAN_CONTEXT* context, int message, void* message_data, void* user_data)
{
    if (message == CALLBACK_MSG_RULE_MATCHING) {
        auto* rule = static_cast<YR_RULE*>(message_data);
        auto* match_data = static_cast<YaraMatchData*>(user_data);

        match_data->rule_names.append(ByteString(rule->identifier));

        // Extract rule metadata
        JsonObject rule_obj;
        rule_obj.set("rule_name"sv, JsonValue(ByteString(rule->identifier)));

        // Get metadata
        YR_META* meta = rule->metas;
        while (!META_IS_LAST_IN_RULE(meta)) {
            if (meta->type == META_TYPE_STRING) {
                if (strcmp(meta->identifier, "description") == 0)
                    rule_obj.set("description"sv, JsonValue(ByteString(meta->string)));
                else if (strcmp(meta->identifier, "severity") == 0)
                    rule_obj.set("severity"sv, JsonValue(ByteString(meta->string)));
                else if (strcmp(meta->identifier, "author") == 0)
                    rule_obj.set("author"sv, JsonValue(ByteString(meta->string)));
            }
            meta++;
        }
        // Handle the last metadata entry
        if (meta->type == META_TYPE_STRING) {
            if (strcmp(meta->identifier, "description") == 0)
                rule_obj.set("description"sv, JsonValue(ByteString(meta->string)));
            else if (strcmp(meta->identifier, "severity") == 0)
                rule_obj.set("severity"sv, JsonValue(ByteString(meta->string)));
            else if (strcmp(meta->identifier, "author") == 0)
                rule_obj.set("author"sv, JsonValue(ByteString(meta->string)));
        }

        match_data->rule_details.append(move(rule_obj));
    }
    return CALLBACK_CONTINUE;
}

ErrorOr<ByteString> SentinelServer::scan_content(ReadonlyBytes content)
{
    if (!s_yara_rules)
        return Error::from_string_literal("YARA rules not initialized");

    // For large files (> 10MB), use streaming scan to reduce memory pressure
    constexpr size_t STREAMING_THRESHOLD = 10 * 1024 * 1024; // 10MB
    constexpr size_t CHUNK_SIZE = 1 * 1024 * 1024; // 1MB chunks

    if (content.size() > STREAMING_THRESHOLD) {
        dbgln("SentinelServer: Using streaming scan for large file ({}MB)", content.size() / (1024 * 1024));

        // Scan in chunks with overlap to catch patterns spanning chunk boundaries
        constexpr size_t OVERLAP_SIZE = 4096; // 4KB overlap
        YaraMatchData match_data;

        size_t offset = 0;
        while (offset < content.size()) {
            size_t chunk_size = min(CHUNK_SIZE, content.size() - offset);
            auto chunk = content.slice(offset, chunk_size);

            int result = yr_rules_scan_mem(
                s_yara_rules,
                reinterpret_cast<uint8_t const*>(chunk.data()),
                chunk.size(),
                0,
                yara_callback,
                &match_data,
                0);

            if (result != ERROR_SUCCESS)
                return Error::from_string_literal("YARA scan failed");

            // If we found matches, we can stop early
            if (!match_data.rule_names.is_empty())
                break;

            // Move to next chunk with overlap
            if (offset + chunk_size >= content.size())
                break;

            offset += chunk_size - OVERLAP_SIZE;
        }

        if (match_data.rule_names.is_empty())
            return ByteString("clean"sv);

        // Format matches as detailed JSON response
        JsonObject result_obj;
        result_obj.set("threat_detected"sv, true);

        JsonArray matched_rules_array;
        for (auto const& rule_detail : match_data.rule_details) {
            matched_rules_array.must_append(rule_detail);
        }

        result_obj.set("matched_rules"sv, move(matched_rules_array));
        result_obj.set("match_count"sv, static_cast<i64>(match_data.rule_names.size()));

        auto json_string = result_obj.serialized();
        return ByteString(json_string.bytes_as_string_view());
    }

    // For smaller files, scan entire content at once (original behavior)
    YaraMatchData match_data;
    int result = yr_rules_scan_mem(
        s_yara_rules,
        reinterpret_cast<uint8_t const*>(content.data()),
        content.size(),
        0,
        yara_callback,
        &match_data,
        0);

    if (result != ERROR_SUCCESS)
        return Error::from_string_literal("YARA scan failed");

    if (match_data.rule_names.is_empty())
        return ByteString("clean"sv);

    // Format matches as detailed JSON response
    JsonObject result_obj;
    result_obj.set("threat_detected"sv, true);

    JsonArray matched_rules_array;
    for (auto const& rule_detail : match_data.rule_details) {
        matched_rules_array.must_append(rule_detail);
    }

    result_obj.set("matched_rules"sv, move(matched_rules_array));
    result_obj.set("match_count"sv, static_cast<i64>(match_data.rule_names.size()));

    auto json_string = result_obj.serialized();
    return ByteString(json_string.bytes_as_string_view());
}

}
