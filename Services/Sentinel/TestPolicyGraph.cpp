/*
 * Copyright (c) 2025, Ladybird contributors
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "PolicyGraph.h"
#include <AK/StringView.h>
#include <stdio.h>

using namespace Sentinel;

static void test_create_policy(PolicyGraph& pg)
{
    printf("\n=== Test: Create Policy ===\n");

    PolicyGraph::Policy policy {
        .rule_name = "EICAR_Test_File"_string,
        .url_pattern = {},  // No URL filter
        .file_hash = "abc123"_string,
        .mime_type = {},    // No MIME type filter
        .action = PolicyGraph::PolicyAction::Block,
        .created_at = UnixDateTime::now(),
        .created_by = "test_user"_string,
        .expires_at = {},   // No expiration
        .last_hit = {},     // No hits yet
    };

    auto result = pg.create_policy(policy);
    if (result.is_error()) {
        printf("❌ FAILED: Could not create policy: %s\n", result.error().string_literal().characters_without_null_termination());
        return;
    }

    auto policy_id = result.release_value();
    printf("✅ PASSED: Created policy with ID %ld\n", policy_id);
}

static void test_list_policies(PolicyGraph& pg)
{
    printf("\n=== Test: List Policies ===\n");

    auto result = pg.list_policies();
    if (result.is_error()) {
        printf("❌ FAILED: Could not list policies\n");
        return;
    }

    auto policies = result.release_value();
    printf("✅ PASSED: Found %zu policies\n", policies.size());

    for (auto const& policy : policies) {
        printf("  - ID: %ld, Rule: %s, Action: %s, Hits: %ld\n",
               policy.id,
               policy.rule_name.bytes_as_string_view().characters_without_null_termination(),
               policy.action == PolicyGraph::PolicyAction::Allow ? "Allow" :
               policy.action == PolicyGraph::PolicyAction::Block ? "Block" : "Quarantine",
               policy.hit_count);
    }
}

static void test_match_policy_by_hash(PolicyGraph& pg)
{
    printf("\n=== Test: Match Policy by Hash ===\n");

    // Create a policy for specific hash
    PolicyGraph::Policy policy {
        .rule_name = "Test_Rule"_string,
        .url_pattern = {},  // No URL filter
        .file_hash = "hash123456"_string,
        .mime_type = {},    // No MIME type filter
        .action = PolicyGraph::PolicyAction::Quarantine,
        .created_at = UnixDateTime::now(),
        .created_by = "test"_string,
        .expires_at = {},   // No expiration
        .last_hit = {},     // No hits yet
    };

    auto create_result = pg.create_policy(policy);
    if (create_result.is_error()) {
        printf("❌ FAILED: Could not create test policy\n");
        return;
    }

    // Try to match by hash
    PolicyGraph::ThreatMetadata threat {
        .url = "http://example.com/file.exe"_string,
        .filename = "file.exe"_string,
        .file_hash = "hash123456"_string,
        .mime_type = "application/x-msdos-program"_string,
        .file_size = 1024,
        .rule_name = "Test_Rule"_string,
        .severity = "high"_string,
    };

    auto match_result = pg.match_policy(threat);
    if (match_result.is_error()) {
        printf("❌ FAILED: Could not match policy\n");
        return;
    }

    auto matched = match_result.release_value();
    if (matched.has_value()) {
        printf("✅ PASSED: Matched policy ID %ld by hash\n", matched->id);
        printf("  Action: %s, Hit count: %ld\n",
               matched->action == PolicyGraph::PolicyAction::Quarantine ? "Quarantine" : "Other",
               matched->hit_count);
    } else {
        printf("❌ FAILED: No policy matched\n");
    }
}

static void test_match_policy_by_url(PolicyGraph& pg)
{
    printf("\n=== Test: Match Policy by URL Pattern ===\n");

    // Create a policy with URL pattern
    PolicyGraph::Policy policy {
        .rule_name = "Malicious_Domain"_string,
        .url_pattern = "%malicious.com%"_string,  // SQL LIKE pattern
        .file_hash = {},  // No hash filter
        .mime_type = {},  // No MIME type filter
        .action = PolicyGraph::PolicyAction::Block,
        .created_at = UnixDateTime::now(),
        .created_by = "test"_string,
        .expires_at = {},  // No expiration
        .last_hit = {},    // No hits yet
    };

    auto create_result = pg.create_policy(policy);
    if (create_result.is_error()) {
        printf("❌ FAILED: Could not create URL pattern policy\n");
        return;
    }

    // Try to match by URL
    PolicyGraph::ThreatMetadata threat {
        .url = "http://evil.malicious.com/payload.exe"_string,
        .filename = "payload.exe"_string,
        .file_hash = "different_hash"_string,
        .mime_type = "application/octet-stream"_string,
        .file_size = 2048,
        .rule_name = "Malicious_Domain"_string,
        .severity = "critical"_string,
    };

    auto match_result = pg.match_policy(threat);
    if (match_result.is_error()) {
        printf("❌ FAILED: Could not match by URL\n");
        return;
    }

    auto matched = match_result.release_value();
    if (matched.has_value()) {
        printf("✅ PASSED: Matched policy ID %ld by URL pattern\n", matched->id);
        printf("  Pattern: %s, Action: Block\n",
               matched->url_pattern.has_value() ? matched->url_pattern->bytes_as_string_view().characters_without_null_termination() : "none");
    } else {
        printf("❌ FAILED: URL pattern did not match\n");
    }
}

static void test_match_policy_by_rule(PolicyGraph& pg)
{
    printf("\n=== Test: Match Policy by Rule Name ===\n");

    // Create a policy for specific rule (no hash or URL)
    PolicyGraph::Policy policy {
        .rule_name = "Windows_PE_Suspicious"_string,
        .url_pattern = {},  // No URL filter
        .file_hash = {},    // No hash filter
        .mime_type = {},    // No MIME type filter
        .action = PolicyGraph::PolicyAction::Quarantine,
        .created_at = UnixDateTime::now(),
        .created_by = "test"_string,
        .expires_at = {},  // No expiration
        .last_hit = {},    // No hits yet
    };

    auto create_result = pg.create_policy(policy);
    if (create_result.is_error()) {
        printf("❌ FAILED: Could not create rule-based policy\n");
        return;
    }

    // Try to match by rule name
    PolicyGraph::ThreatMetadata threat {
        .url = "http://anywhere.com/program.exe"_string,
        .filename = "program.exe"_string,
        .file_hash = "yet_another_hash"_string,
        .mime_type = "application/x-msdownload"_string,
        .file_size = 4096,
        .rule_name = "Windows_PE_Suspicious"_string,
        .severity = "medium"_string,
    };

    auto match_result = pg.match_policy(threat);
    if (match_result.is_error()) {
        printf("❌ FAILED: Could not match by rule name\n");
        return;
    }

    auto matched = match_result.release_value();
    if (matched.has_value()) {
        printf("✅ PASSED: Matched policy ID %ld by rule name\n", matched->id);
        printf("  Rule: %s, Action: Quarantine\n",
               matched->rule_name.bytes_as_string_view().characters_without_null_termination());
    } else {
        printf("❌ FAILED: Rule name did not match\n");
    }
}

static void test_record_threat(PolicyGraph& pg)
{
    printf("\n=== Test: Record Threat History ===\n");

    PolicyGraph::ThreatMetadata threat {
        .url = "http://test.com/threat.exe"_string,
        .filename = "threat.exe"_string,
        .file_hash = "threat_hash_123"_string,
        .mime_type = "application/x-msdos-program"_string,
        .file_size = 8192,
        .rule_name = "Test_Threat"_string,
        .severity = "high"_string,
    };

    auto result = pg.record_threat(threat, "blocked"_string, {}, "{\"test\":\"data\"}"_string);
    if (result.is_error()) {
        printf("❌ FAILED: Could not record threat\n");
        return;
    }

    printf("✅ PASSED: Recorded threat to history\n");
}

static void test_get_threat_history(PolicyGraph& pg)
{
    printf("\n=== Test: Get Threat History ===\n");

    auto result = pg.get_threat_history({});
    if (result.is_error()) {
        printf("❌ FAILED: Could not get threat history\n");
        return;
    }

    auto threats = result.release_value();
    printf("✅ PASSED: Retrieved %zu threat records\n", threats.size());

    for (auto const& threat : threats) {
        printf("  - %s from %s: %s (action: %s)\n",
               threat.filename.bytes_as_string_view().characters_without_null_termination(),
               threat.url.bytes_as_string_view().characters_without_null_termination(),
               threat.rule_name.bytes_as_string_view().characters_without_null_termination(),
               threat.action_taken.bytes_as_string_view().characters_without_null_termination());
    }
}

static void test_policy_statistics(PolicyGraph& pg)
{
    printf("\n=== Test: Policy Statistics ===\n");

    auto policy_count = pg.get_policy_count();
    auto threat_count = pg.get_threat_count();

    if (policy_count.is_error() || threat_count.is_error()) {
        printf("❌ FAILED: Could not get statistics\n");
        return;
    }

    printf("✅ PASSED: Statistics retrieved\n");
    printf("  Total policies: %lu\n", policy_count.release_value());
    printf("  Total threats: %lu\n", threat_count.release_value());
}

int main()
{
    printf("====================================\n");
    printf("  PolicyGraph Integration Tests\n");
    printf("====================================\n");

    // Create PolicyGraph with test database
    auto db_path = "/tmp/sentinel_test"sv;
    auto pg_result = PolicyGraph::create(db_path);

    if (pg_result.is_error()) {
        printf("\n❌ FATAL: Could not create PolicyGraph: %s\n",
               pg_result.error().string_literal().characters_without_null_termination());
        return 1;
    }

    auto pg = pg_result.release_value();
    printf("✅ PolicyGraph initialized at %s\n", db_path.characters_without_null_termination());

    // Run all tests
    test_create_policy(pg);
    test_list_policies(pg);
    test_match_policy_by_hash(pg);
    test_match_policy_by_url(pg);
    test_match_policy_by_rule(pg);
    test_record_threat(pg);
    test_get_threat_history(pg);
    test_policy_statistics(pg);

    printf("\n====================================\n");
    printf("  All Tests Complete!\n");
    printf("====================================\n");
    printf("\nDatabase location: %s/policy_graph.db\n", db_path.characters_without_null_termination());

    return 0;
}
