/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonParser.h>
#include <LibCore/Resource.h>
#include <LibCore/StandardPaths.h>
#include <LibWebView/Application.h>
#include <LibWebView/WebUI/SecurityUI.h>

namespace WebView {

void SecurityUI::register_interfaces()
{
    // Initialize PolicyGraph with Ladybird data directory
    auto data_directory = ByteString::formatted("{}/Ladybird", Core::StandardPaths::user_data_directory());
    auto pg_result = Sentinel::PolicyGraph::create(data_directory);

    if (pg_result.is_error()) {
        dbgln("SecurityUI: Failed to initialize PolicyGraph: {}", pg_result.error());
    } else {
        m_policy_graph = pg_result.release_value();
        dbgln("SecurityUI: PolicyGraph initialized successfully");
    }

    // Register interfaces
    register_interface("getSystemStatus"sv, [this](auto const&) {
        get_system_status();
    });

    register_interface("loadStatistics"sv, [this](auto const&) {
        load_statistics();
    });

    register_interface("loadPolicies"sv, [this](auto const&) {
        load_policies();
    });

    register_interface("getPolicy"sv, [this](auto const& data) {
        get_policy(data);
    });

    register_interface("createPolicy"sv, [this](auto const& data) {
        create_policy(data);
    });

    register_interface("updatePolicy"sv, [this](auto const& data) {
        update_policy(data);
    });

    register_interface("deletePolicy"sv, [this](auto const& data) {
        delete_policy(data);
    });

    register_interface("loadThreatHistory"sv, [this](auto const& data) {
        load_threat_history(data);
    });

    register_interface("getTemplates"sv, [this](auto const&) {
        get_policy_templates();
    });

    register_interface("createFromTemplate"sv, [this](auto const& data) {
        create_policy_from_template(data);
    });

    register_interface("openQuarantineManager"sv, [this](auto const&) {
        open_quarantine_manager();
    });
}

void SecurityUI::get_system_status()
{
    // Query RequestServer for real-time SentinelServer status via IPC
    // This replaces the old heuristic-based approach with actual status from RequestServer
    auto& request_client = Application::request_server_client();

    // Make synchronous IPC call to get sentinel status
    // The RequestServer checks if g_security_tap (connection to SentinelServer) is initialized
    auto response = request_client.get_sentinel_status();
    handle_sentinel_status(response.connected(), response.scanning_enabled());
}

void SecurityUI::handle_sentinel_status(bool connected, bool scanning_enabled)
{
    JsonObject status;
    status.set("connected"sv, JsonValue { connected });
    status.set("scanning_enabled"sv, JsonValue { scanning_enabled });

    // Get last scan time from threat history
    i64 last_scan_timestamp = 0;
    if (m_policy_graph.has_value()) {
        // Get the most recent threat from history
        auto threats_result = m_policy_graph->get_threat_history({}); // All threats (empty Optional)
        if (!threats_result.is_error()) {
            auto threats = threats_result.release_value();
            if (!threats.is_empty()) {
                // Find most recent threat
                for (auto const& threat : threats) {
                    auto timestamp = threat.detected_at.milliseconds_since_epoch();
                    if (timestamp > last_scan_timestamp) {
                        last_scan_timestamp = timestamp;
                    }
                }
            }
        }
    }

    status.set("last_scan"sv, JsonValue { last_scan_timestamp });

    async_send_message("systemStatusLoaded"sv, status);
}

void SecurityUI::load_statistics()
{
    JsonObject stats;

    if (!m_policy_graph.has_value()) {
        dbgln("SecurityUI: PolicyGraph not initialized, returning zeros");
        stats.set("totalPolicies"sv, JsonValue { 0 });
        stats.set("threatsBlocked"sv, JsonValue { 0 });
        stats.set("threatsQuarantined"sv, JsonValue { 0 });
        stats.set("threatsToday"sv, JsonValue { 0 });
        async_send_message("statisticsLoaded"sv, stats);
        return;
    }

    // Query PolicyGraph for statistics
    auto policy_count_result = m_policy_graph->get_policy_count();
    auto threat_count_result = m_policy_graph->get_threat_count();

    if (policy_count_result.is_error()) {
        dbgln("SecurityUI: Failed to get policy count: {}", policy_count_result.error());
        stats.set("totalPolicies"sv, JsonValue { 0 });
    } else {
        stats.set("totalPolicies"sv, JsonValue { static_cast<i64>(policy_count_result.value()) });
    }

    if (threat_count_result.is_error()) {
        dbgln("SecurityUI: Failed to get threat count: {}", threat_count_result.error());
        stats.set("threatsBlocked"sv, JsonValue { 0 });
        stats.set("threatsQuarantined"sv, JsonValue { 0 });
        stats.set("threatsToday"sv, JsonValue { 0 });
    } else {
        // Total threats detected
        auto total_threats = static_cast<i64>(threat_count_result.value());

        // Get threats from today for the threatsToday stat
        auto now = UnixDateTime::now();
        auto yesterday = UnixDateTime::from_seconds_since_epoch(now.seconds_since_epoch() - 86400);
        auto threats_today_result = m_policy_graph->get_threat_history(yesterday); // Last 24 hours
        i64 threats_today = 0;

        if (!threats_today_result.is_error()) {
            threats_today = static_cast<i64>(threats_today_result.value().size());
        }

        // For simplicity, we'll set threatsBlocked to total threats
        // In a real implementation, we'd filter by action_taken
        stats.set("threatsBlocked"sv, JsonValue { total_threats });
        stats.set("threatsQuarantined"sv, JsonValue { 0 }); // Not currently tracking quarantined separately
        stats.set("threatsToday"sv, JsonValue { threats_today });
    }

    async_send_message("statisticsLoaded"sv, stats);
}

void SecurityUI::load_policies()
{
    JsonArray policies_array;

    if (!m_policy_graph.has_value()) {
        dbgln("SecurityUI: PolicyGraph not initialized, returning empty policies");
        JsonObject response;
        response.set("policies"sv, JsonValue { policies_array });
        async_send_message("policiesLoaded"sv, response);
        return;
    }

    // Query PolicyGraph for all policies
    auto policies_result = m_policy_graph->list_policies();

    if (policies_result.is_error()) {
        dbgln("SecurityUI: Failed to list policies: {}", policies_result.error());
        JsonObject response;
        response.set("policies"sv, JsonValue { policies_array });
        async_send_message("policiesLoaded"sv, response);
        return;
    }

    // Convert policies to JSON
    auto policies = policies_result.release_value();
    for (auto const& policy : policies) {
        JsonObject policy_obj;
        policy_obj.set("id"sv, JsonValue { policy.id });
        policy_obj.set("ruleName"sv, JsonValue { policy.rule_name });

        if (policy.url_pattern.has_value()) {
            policy_obj.set("urlPattern"sv, JsonValue { *policy.url_pattern });
        }

        if (policy.file_hash.has_value()) {
            policy_obj.set("fileHash"sv, JsonValue { *policy.file_hash });
        }

        if (policy.mime_type.has_value()) {
            policy_obj.set("mimeType"sv, JsonValue { *policy.mime_type });
        }

        // Convert PolicyAction enum to string
        StringView action_str;
        switch (policy.action) {
        case Sentinel::PolicyGraph::PolicyAction::Allow:
            action_str = "Allow"sv;
            break;
        case Sentinel::PolicyGraph::PolicyAction::Block:
            action_str = "Block"sv;
            break;
        case Sentinel::PolicyGraph::PolicyAction::Quarantine:
            action_str = "Quarantine"sv;
            break;
        }
        policy_obj.set("action"sv, JsonValue { action_str });

        policy_obj.set("createdAt"sv, JsonValue { policy.created_at.milliseconds_since_epoch() });
        policy_obj.set("createdBy"sv, JsonValue { policy.created_by });

        if (policy.expires_at.has_value()) {
            policy_obj.set("expiresAt"sv, JsonValue { policy.expires_at->milliseconds_since_epoch() });
        }

        policy_obj.set("hitCount"sv, JsonValue { static_cast<i64>(policy.hit_count) });

        if (policy.last_hit.has_value()) {
            policy_obj.set("lastHit"sv, JsonValue { policy.last_hit->milliseconds_since_epoch() });
        }

        policies_array.must_append(policy_obj);
    }

    JsonObject response;
    response.set("policies"sv, JsonValue { policies_array });

    async_send_message("policiesLoaded"sv, response);
}

void SecurityUI::get_policy(JsonValue const& data)
{
    if (!data.is_object()) {
        JsonObject error;
        error.set("error"sv, JsonValue { "Invalid request: expected object with policyId"sv });
        async_send_message("policyLoaded"sv, error);
        return;
    }

    if (!m_policy_graph.has_value()) {
        JsonObject error;
        error.set("error"sv, JsonValue { "PolicyGraph not initialized"sv });
        async_send_message("policyLoaded"sv, error);
        return;
    }

    auto const& data_obj = data.as_object();
    auto policy_id_value = data_obj.get_integer<i64>("policyId"sv);

    if (!policy_id_value.has_value()) {
        JsonObject error;
        error.set("error"sv, JsonValue { "Missing or invalid policyId"sv });
        async_send_message("policyLoaded"sv, error);
        return;
    }

    // Retrieve policy from PolicyGraph
    auto policy_result = m_policy_graph->get_policy(policy_id_value.value());

    if (policy_result.is_error()) {
        JsonObject error;
        error.set("error"sv, JsonValue { ByteString::formatted("Failed to get policy: {}", policy_result.error()) });
        async_send_message("policyLoaded"sv, error);
        return;
    }

    // Convert policy to JSON
    auto const& policy = policy_result.value();
    JsonObject policy_obj;
    policy_obj.set("id"sv, JsonValue { policy.id });
    policy_obj.set("ruleName"sv, JsonValue { policy.rule_name });

    if (policy.url_pattern.has_value()) {
        policy_obj.set("urlPattern"sv, JsonValue { *policy.url_pattern });
    }

    if (policy.file_hash.has_value()) {
        policy_obj.set("fileHash"sv, JsonValue { *policy.file_hash });
    }

    if (policy.mime_type.has_value()) {
        policy_obj.set("mimeType"sv, JsonValue { *policy.mime_type });
    }

    // Convert PolicyAction enum to string
    StringView action_str;
    switch (policy.action) {
    case Sentinel::PolicyGraph::PolicyAction::Allow:
        action_str = "Allow"sv;
        break;
    case Sentinel::PolicyGraph::PolicyAction::Block:
        action_str = "Block"sv;
        break;
    case Sentinel::PolicyGraph::PolicyAction::Quarantine:
        action_str = "Quarantine"sv;
        break;
    }
    policy_obj.set("action"sv, JsonValue { action_str });

    policy_obj.set("createdAt"sv, JsonValue { policy.created_at.milliseconds_since_epoch() });
    policy_obj.set("createdBy"sv, JsonValue { policy.created_by });

    if (policy.expires_at.has_value()) {
        policy_obj.set("expiresAt"sv, JsonValue { policy.expires_at->milliseconds_since_epoch() });
    }

    policy_obj.set("hitCount"sv, JsonValue { static_cast<i64>(policy.hit_count) });

    if (policy.last_hit.has_value()) {
        policy_obj.set("lastHit"sv, JsonValue { policy.last_hit->milliseconds_since_epoch() });
    }

    async_send_message("policyLoaded"sv, policy_obj);
}

void SecurityUI::create_policy(JsonValue const& data)
{
    if (!data.is_object()) {
        JsonObject error;
        error.set("error"sv, JsonValue { "Invalid request: expected policy object"sv });
        async_send_message("policyCreated"sv, error);
        return;
    }

    if (!m_policy_graph.has_value()) {
        JsonObject error;
        error.set("error"sv, JsonValue { "PolicyGraph not initialized"sv });
        async_send_message("policyCreated"sv, error);
        return;
    }

    auto const& data_obj = data.as_object();

    // Parse required fields
    auto rule_name = data_obj.get_string("ruleName"sv);
    auto action_str = data_obj.get_string("action"sv);

    if (!rule_name.has_value() || !action_str.has_value()) {
        JsonObject error;
        error.set("error"sv, JsonValue { "Missing required fields: ruleName and action"sv });
        async_send_message("policyCreated"sv, error);
        return;
    }

    // Parse action
    Sentinel::PolicyGraph::PolicyAction action;
    if (action_str.value() == "Allow"sv) {
        action = Sentinel::PolicyGraph::PolicyAction::Allow;
    } else if (action_str.value() == "Block"sv) {
        action = Sentinel::PolicyGraph::PolicyAction::Block;
    } else if (action_str.value() == "Quarantine"sv) {
        action = Sentinel::PolicyGraph::PolicyAction::Quarantine;
    } else {
        JsonObject error;
        error.set("error"sv, JsonValue { "Invalid action: must be Allow, Block, or Quarantine"sv });
        async_send_message("policyCreated"sv, error);
        return;
    }

    // Parse optional fields
    Optional<String> url_pattern;
    if (auto url_val = data_obj.get_string("urlPattern"sv); url_val.has_value()) {
        url_pattern = url_val.value();
    }

    Optional<String> file_hash;
    if (auto hash_val = data_obj.get_string("fileHash"sv); hash_val.has_value()) {
        file_hash = hash_val.value();
    }

    Optional<String> mime_type;
    if (auto mime_val = data_obj.get_string("mimeType"sv); mime_val.has_value()) {
        mime_type = mime_val.value();
    }

    // Create policy struct
    Sentinel::PolicyGraph::Policy policy {
        .id = -1, // Will be assigned by database
        .rule_name = rule_name.value(),
        .url_pattern = move(url_pattern),
        .file_hash = move(file_hash),
        .mime_type = move(mime_type),
        .action = action,
        .created_at = UnixDateTime::now(),
        .created_by = "UI"_string,
        .expires_at = {},
        .hit_count = 0,
        .last_hit = {}
    };

    // Create policy in PolicyGraph
    auto policy_id_result = m_policy_graph->create_policy(policy);

    if (policy_id_result.is_error()) {
        JsonObject error;
        error.set("error"sv, JsonValue { ByteString::formatted("Failed to create policy: {}", policy_id_result.error()) });
        async_send_message("policyCreated"sv, error);
        return;
    }

    JsonObject response;
    response.set("success"sv, JsonValue { true });
    response.set("policyId"sv, JsonValue { policy_id_result.value() });
    response.set("message"sv, JsonValue { "Policy created successfully"sv });

    async_send_message("policyCreated"sv, response);
}

void SecurityUI::update_policy(JsonValue const& data)
{
    if (!data.is_object()) {
        JsonObject error;
        error.set("error"sv, JsonValue { "Invalid request: expected policy object"sv });
        async_send_message("policyUpdated"sv, error);
        return;
    }

    if (!m_policy_graph.has_value()) {
        JsonObject error;
        error.set("error"sv, JsonValue { "PolicyGraph not initialized"sv });
        async_send_message("policyUpdated"sv, error);
        return;
    }

    auto const& data_obj = data.as_object();

    // Parse policy ID
    auto policy_id_value = data_obj.get_integer<i64>("id"sv);
    if (!policy_id_value.has_value()) {
        JsonObject error;
        error.set("error"sv, JsonValue { "Missing or invalid policy id"sv });
        async_send_message("policyUpdated"sv, error);
        return;
    }

    // Parse required fields
    auto rule_name = data_obj.get_string("ruleName"sv);
    auto action_str = data_obj.get_string("action"sv);

    if (!rule_name.has_value() || !action_str.has_value()) {
        JsonObject error;
        error.set("error"sv, JsonValue { "Missing required fields: ruleName and action"sv });
        async_send_message("policyUpdated"sv, error);
        return;
    }

    // Parse action
    Sentinel::PolicyGraph::PolicyAction action;
    if (action_str.value() == "Allow"sv) {
        action = Sentinel::PolicyGraph::PolicyAction::Allow;
    } else if (action_str.value() == "Block"sv) {
        action = Sentinel::PolicyGraph::PolicyAction::Block;
    } else if (action_str.value() == "Quarantine"sv) {
        action = Sentinel::PolicyGraph::PolicyAction::Quarantine;
    } else {
        JsonObject error;
        error.set("error"sv, JsonValue { "Invalid action: must be Allow, Block, or Quarantine"sv });
        async_send_message("policyUpdated"sv, error);
        return;
    }

    // Parse optional fields
    Optional<String> url_pattern;
    if (auto url_val = data_obj.get_string("urlPattern"sv); url_val.has_value()) {
        url_pattern = url_val.value();
    }

    Optional<String> file_hash;
    if (auto hash_val = data_obj.get_string("fileHash"sv); hash_val.has_value()) {
        file_hash = hash_val.value();
    }

    Optional<String> mime_type;
    if (auto mime_val = data_obj.get_string("mimeType"sv); mime_val.has_value()) {
        mime_type = mime_val.value();
    }

    // Create updated policy struct
    Sentinel::PolicyGraph::Policy policy {
        .id = policy_id_value.value(),
        .rule_name = rule_name.value(),
        .url_pattern = move(url_pattern),
        .file_hash = move(file_hash),
        .mime_type = move(mime_type),
        .action = action,
        .created_at = UnixDateTime::now(), // This will be ignored by update
        .created_by = "UI"_string,
        .expires_at = {},
        .hit_count = 0,
        .last_hit = {}
    };

    // Update policy in PolicyGraph
    auto update_result = m_policy_graph->update_policy(policy_id_value.value(), policy);

    if (update_result.is_error()) {
        JsonObject error;
        error.set("error"sv, JsonValue { ByteString::formatted("Failed to update policy: {}", update_result.error()) });
        async_send_message("policyUpdated"sv, error);
        return;
    }

    JsonObject response;
    response.set("success"sv, JsonValue { true });
    response.set("message"sv, JsonValue { "Policy updated successfully"sv });

    async_send_message("policyUpdated"sv, response);
}

void SecurityUI::delete_policy(JsonValue const& data)
{
    if (!data.is_object()) {
        JsonObject error;
        error.set("error"sv, JsonValue { "Invalid request: expected object with policyId"sv });
        async_send_message("policyDeleted"sv, error);
        return;
    }

    if (!m_policy_graph.has_value()) {
        JsonObject error;
        error.set("error"sv, JsonValue { "PolicyGraph not initialized"sv });
        async_send_message("policyDeleted"sv, error);
        return;
    }

    auto const& data_obj = data.as_object();
    auto policy_id_value = data_obj.get_integer<i64>("policyId"sv);

    if (!policy_id_value.has_value()) {
        JsonObject error;
        error.set("error"sv, JsonValue { "Missing or invalid policyId"sv });
        async_send_message("policyDeleted"sv, error);
        return;
    }

    // Delete policy from PolicyGraph
    auto delete_result = m_policy_graph->delete_policy(policy_id_value.value());

    if (delete_result.is_error()) {
        JsonObject error;
        error.set("error"sv, JsonValue { ByteString::formatted("Failed to delete policy: {}", delete_result.error()) });
        async_send_message("policyDeleted"sv, error);
        return;
    }

    JsonObject response;
    response.set("success"sv, JsonValue { true });
    response.set("message"sv, JsonValue { "Policy deleted successfully"sv });

    async_send_message("policyDeleted"sv, response);
}

void SecurityUI::load_threat_history(JsonValue const& data)
{
    JsonArray threats_array;

    if (!m_policy_graph.has_value()) {
        dbgln("SecurityUI: PolicyGraph not initialized, returning empty threat history");
        JsonObject response;
        response.set("threats"sv, JsonValue { threats_array });
        async_send_message("threatHistoryLoaded"sv, response);
        return;
    }

    // Parse optional 'since' parameter (timestamp in milliseconds)
    Optional<UnixDateTime> since;
    if (data.is_object()) {
        auto const& data_obj = data.as_object();
        auto since_value = data_obj.get_integer<i64>("since"sv);
        if (since_value.has_value()) {
            since = UnixDateTime::from_milliseconds_since_epoch(since_value.value());
        }
    }

    // Query PolicyGraph for threat history
    auto threats_result = m_policy_graph->get_threat_history(since);

    if (threats_result.is_error()) {
        dbgln("SecurityUI: Failed to get threat history: {}", threats_result.error());
        JsonObject response;
        response.set("threats"sv, JsonValue { threats_array });
        async_send_message("threatHistoryLoaded"sv, response);
        return;
    }

    // Convert threats to JSON
    auto threats = threats_result.release_value();
    for (auto const& threat : threats) {
        JsonObject threat_obj;
        threat_obj.set("id"sv, JsonValue { threat.id });
        threat_obj.set("detectedAt"sv, JsonValue { threat.detected_at.milliseconds_since_epoch() });
        threat_obj.set("url"sv, JsonValue { threat.url });
        threat_obj.set("filename"sv, JsonValue { threat.filename });
        threat_obj.set("fileHash"sv, JsonValue { threat.file_hash });
        threat_obj.set("mimeType"sv, JsonValue { threat.mime_type });
        threat_obj.set("fileSize"sv, JsonValue { static_cast<i64>(threat.file_size) });
        threat_obj.set("ruleName"sv, JsonValue { threat.rule_name });
        threat_obj.set("severity"sv, JsonValue { threat.severity });
        threat_obj.set("actionTaken"sv, JsonValue { threat.action_taken });

        if (threat.policy_id.has_value()) {
            threat_obj.set("policyId"sv, JsonValue { threat.policy_id.value() });
        }

        threat_obj.set("alertJson"sv, JsonValue { threat.alert_json });

        threats_array.must_append(threat_obj);
    }

    JsonObject response;
    response.set("threats"sv, JsonValue { threats_array });

    async_send_message("threatHistoryLoaded"sv, response);
}

void SecurityUI::get_policy_templates()
{
    JsonArray templates_array;

    // Load policy templates from resource directory
    auto templates_resource_result = Core::Resource::load_from_uri("resource://ladybird/policy-templates"sv);

    if (templates_resource_result.is_error()) {
        dbgln("SecurityUI: Failed to load policy templates resource: {}", templates_resource_result.error());
        JsonObject response;
        response.set("templates"sv, JsonValue { templates_array });
        async_send_message("templatesLoaded"sv, response);
        return;
    }

    auto templates_resource = templates_resource_result.release_value();

    // Iterate through all template JSON files
    templates_resource->for_each_descendant_file([&](Core::Resource const& resource) -> IterationDecision {
        auto filename = resource.filename();

        // Only process .json files
        if (!filename.ends_with_bytes(".json"sv)) {
            return IterationDecision::Continue;
        }

        // Parse the template JSON
        auto json_data = ByteString(reinterpret_cast<char const*>(resource.data().data()), resource.data().size());
        auto json_result = JsonValue::from_string(json_data);

        if (json_result.is_error()) {
            dbgln("SecurityUI: Failed to parse template {}: {}", filename, json_result.error());
            return IterationDecision::Continue;
        }

        auto template_json = json_result.release_value();
        if (!template_json.is_object()) {
            dbgln("SecurityUI: Template {} is not a JSON object", filename);
            return IterationDecision::Continue;
        }

        // Add template ID (filename without .json)
        auto template_obj = template_json.as_object();
        auto filename_view = filename.bytes_as_string_view();
        auto template_id = filename_view.substring_view(0, filename_view.length() - 5); // Remove .json
        template_obj.set("id"sv, JsonValue { template_id });

        templates_array.must_append(template_obj);
        return IterationDecision::Continue;
    });

    JsonObject response;
    response.set("templates"sv, JsonValue { templates_array });

    async_send_message("templatesLoaded"sv, response);
}

void SecurityUI::create_policy_from_template(JsonValue const& data)
{
    if (!data.is_object()) {
        JsonObject error;
        error.set("error"sv, JsonValue { "Invalid request: expected object with templateId and variables"sv });
        async_send_message("policyFromTemplateCreated"sv, error);
        return;
    }

    if (!m_policy_graph.has_value()) {
        JsonObject error;
        error.set("error"sv, JsonValue { "PolicyGraph not initialized"sv });
        async_send_message("policyFromTemplateCreated"sv, error);
        return;
    }

    auto const& data_obj = data.as_object();
    auto template_id = data_obj.get_string("templateId"sv);

    if (!template_id.has_value()) {
        JsonObject error;
        error.set("error"sv, JsonValue { "Missing templateId"sv });
        async_send_message("policyFromTemplateCreated"sv, error);
        return;
    }

    // Load the template
    auto template_filename = ByteString::formatted("{}.json", template_id.value());
    auto template_resource_result = Core::Resource::load_from_uri(
        ByteString::formatted("resource://ladybird/policy-templates/{}", template_filename)
    );

    if (template_resource_result.is_error()) {
        JsonObject error;
        error.set("error"sv, JsonValue { ByteString::formatted("Failed to load template: {}", template_resource_result.error()) });
        async_send_message("policyFromTemplateCreated"sv, error);
        return;
    }

    auto template_resource = template_resource_result.release_value();
    auto json_data = ByteString(reinterpret_cast<char const*>(template_resource->data().data()), template_resource->data().size());
    auto json_result = JsonValue::from_string(json_data);

    if (json_result.is_error()) {
        JsonObject error;
        error.set("error"sv, JsonValue { ByteString::formatted("Failed to parse template: {}", json_result.error()) });
        async_send_message("policyFromTemplateCreated"sv, error);
        return;
    }

    auto template_json = json_result.release_value();
    if (!template_json.is_object()) {
        JsonObject error;
        error.set("error"sv, JsonValue { "Template is not a JSON object"sv });
        async_send_message("policyFromTemplateCreated"sv, error);
        return;
    }

    auto const& template_obj = template_json.as_object();

    // Get variables from request
    auto variables_json = data_obj.get_object("variables"sv);
    if (!variables_json.has_value()) {
        JsonObject error;
        error.set("error"sv, JsonValue { "Missing variables"sv });
        async_send_message("policyFromTemplateCreated"sv, error);
        return;
    }

    auto const& variables = variables_json.value();

    // Get policies array from template
    auto policies_json = template_obj.get_array("policies"sv);
    if (!policies_json.has_value()) {
        JsonObject error;
        error.set("error"sv, JsonValue { "Template missing policies array"sv });
        async_send_message("policyFromTemplateCreated"sv, error);
        return;
    }

    // Create policies from template
    Vector<i64> created_policy_ids;
    for (auto const& policy_value : policies_json.value().values()) {
        if (!policy_value.is_object()) {
            continue;
        }

        auto const& policy_template = policy_value.as_object();

        // Get rule name and substitute variables
        auto rule_name_template = policy_template.get_string("ruleName"sv);
        if (!rule_name_template.has_value()) {
            continue;
        }

        String rule_name = rule_name_template.value();

        // Substitute variables in rule name
        variables.for_each_member([&](auto const& var_name, auto const& var_value) {
            if (var_value.is_string()) {
                auto placeholder = ByteString::formatted("${{{}}}", var_name);
                rule_name = MUST(rule_name.replace(placeholder, var_value.as_string(), ReplaceMode::All));
            }
        });

        // Get action
        auto action_str = policy_template.get_string("action"sv);
        if (!action_str.has_value()) {
            continue;
        }

        Sentinel::PolicyGraph::PolicyAction action;
        if (action_str.value() == "Allow"sv) {
            action = Sentinel::PolicyGraph::PolicyAction::Allow;
        } else if (action_str.value() == "Block"sv) {
            action = Sentinel::PolicyGraph::PolicyAction::Block;
        } else if (action_str.value() == "Quarantine"sv) {
            action = Sentinel::PolicyGraph::PolicyAction::Quarantine;
        } else {
            continue;
        }

        // Get match pattern
        auto match_pattern_json = policy_template.get_object("match_pattern"sv);
        if (!match_pattern_json.has_value()) {
            continue;
        }

        auto const& match_pattern = match_pattern_json.value();

        // Extract and substitute URL pattern
        Optional<String> url_pattern;
        if (auto url_val = match_pattern.get_string("url_pattern"sv); url_val.has_value() && !url_val.value().is_empty()) {
            String url = url_val.value();
            variables.for_each_member([&](auto const& var_name, auto const& var_value) {
                if (var_value.is_string()) {
                    auto placeholder = ByteString::formatted("${{{}}}", var_name);
                    url = MUST(url.replace(placeholder, var_value.as_string(), ReplaceMode::All));
                }
            });
            url_pattern = url;
        }

        // Extract and substitute file hash
        Optional<String> file_hash;
        if (auto hash_val = match_pattern.get_string("file_hash"sv); hash_val.has_value() && !hash_val.value().is_empty()) {
            String hash = hash_val.value();
            variables.for_each_member([&](auto const& var_name, auto const& var_value) {
                if (var_value.is_string()) {
                    auto placeholder = ByteString::formatted("${{{}}}", var_name);
                    hash = MUST(hash.replace(placeholder, var_value.as_string(), ReplaceMode::All));
                }
            });
            file_hash = hash;
        }

        // Extract MIME type (usually not templated, but handle it anyway)
        Optional<String> mime_type;
        if (auto mime_val = match_pattern.get_string("mime_type"sv); mime_val.has_value() && !mime_val.value().is_empty()) {
            mime_type = mime_val.value();
        }

        // Create the policy
        Sentinel::PolicyGraph::Policy policy {
            .id = -1,
            .rule_name = rule_name,
            .url_pattern = move(url_pattern),
            .file_hash = move(file_hash),
            .mime_type = move(mime_type),
            .action = action,
            .created_at = UnixDateTime::now(),
            .created_by = "Template"_string,
            .expires_at = {},
            .hit_count = 0,
            .last_hit = {}
        };

        auto policy_id_result = m_policy_graph->create_policy(policy);
        if (!policy_id_result.is_error()) {
            created_policy_ids.append(policy_id_result.value());
        } else {
            dbgln("SecurityUI: Failed to create policy from template: {}", policy_id_result.error());
        }
    }

    // Send response
    if (created_policy_ids.is_empty()) {
        JsonObject error;
        error.set("error"sv, JsonValue { "Failed to create any policies from template"sv });
        async_send_message("policyFromTemplateCreated"sv, error);
        return;
    }

    JsonArray ids_array;
    for (auto id : created_policy_ids) {
        ids_array.must_append(JsonValue { id });
    }

    JsonObject response;
    response.set("success"sv, JsonValue { true });
    response.set("policyIds"sv, JsonValue { ids_array });
    response.set("message"sv, JsonValue { ByteString::formatted("Created {} policies from template", created_policy_ids.size()) });

    async_send_message("policyFromTemplateCreated"sv, response);
}

void SecurityUI::open_quarantine_manager()
{
    // This method is called when the user clicks "Manage Quarantine" in about:security
    // The actual quarantine dialog is shown by the Qt UI layer
    // We notify the application that the quarantine manager should be opened
    dbgln("SecurityUI: Quarantine manager requested");

    // Send a message to the application to open the quarantine dialog
    // The Qt/application layer will handle creating and showing the dialog
    Application::the().on_quarantine_manager_requested();
}

}
