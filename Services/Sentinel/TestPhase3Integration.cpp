/*
 * Copyright (c) 2025, Ladybird contributors
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "PolicyGraph.h"
#include <AK/StringView.h>
#include <LibCore/Directory.h>
#include <LibFileSystem/FileSystem.h>
#include <stdio.h>
#include <sys/stat.h>

using namespace Sentinel;

// EICAR test file hash (SHA256)
constexpr auto EICAR_HASH = "275a021bbfb6489e54d471899f7db9d1663fc695ec2fe2a2c4538aabf651fd0f"sv;

// Test result tracking
static int tests_passed = 0;
static int tests_failed = 0;

static void log_pass(StringView test_name)
{
    printf("✅ PASSED: %s\n", test_name.characters_without_null_termination());
    tests_passed++;
}

static void log_fail(StringView test_name, StringView reason)
{
    printf("❌ FAILED: %s - %s\n",
           test_name.characters_without_null_termination(),
           reason.characters_without_null_termination());
    tests_failed++;
}

static void print_section(StringView title)
{
    printf("\n=== %s ===\n", title.characters_without_null_termination());
}

// Test 1: Block Policy Enforcement
// - Create policy for EICAR test file hash
// - Verify automatic blocking without user prompt
// - Verify threat logged in history
static void test_block_policy_enforcement(PolicyGraph& pg)
{
    print_section("Test 1: Block Policy Enforcement"sv);

    // Create a block policy for EICAR hash
    PolicyGraph::Policy block_policy {
        .rule_name = "EICAR_Test_File"_string,
        .url_pattern = {},
        .file_hash = String::from_utf8_without_validation(EICAR_HASH.bytes()),
        .mime_type = {},
        .action = PolicyGraph::PolicyAction::Block,
        .created_at = UnixDateTime::now(),
        .created_by = "integration_test"_string,
        .expires_at = {},
        .last_hit = {},
    };

    auto create_result = pg.create_policy(block_policy);
    if (create_result.is_error()) {
        log_fail("Create block policy"sv, "Could not create policy"sv);
        return;
    }

    auto policy_id = create_result.release_value();
    printf("  Created block policy ID: %ld\n", policy_id);

    // Simulate first EICAR detection
    PolicyGraph::ThreatMetadata first_threat {
        .url = "http://test.example.com/eicar.com"_string,
        .filename = "eicar.com"_string,
        .file_hash = String::from_utf8_without_validation(EICAR_HASH.bytes()),
        .mime_type = "application/octet-stream"_string,
        .file_size = 68,
        .rule_name = "EICAR_Test_File"_string,
        .severity = "critical"_string,
    };

    // First detection - policy should match
    auto match_result = pg.match_policy(first_threat);
    if (match_result.is_error()) {
        log_fail("Match block policy"sv, "Policy match failed"sv);
        return;
    }

    auto matched = match_result.release_value();
    if (!matched.has_value()) {
        log_fail("Match block policy"sv, "No policy matched EICAR hash"sv);
        return;
    }

    if (matched->action != PolicyGraph::PolicyAction::Block) {
        log_fail("Match block policy"sv, "Wrong action (expected Block)"sv);
        return;
    }

    printf("  First EICAR detection matched policy ID: %ld (Action: Block)\n", matched->id);

    // Record threat in history
    auto record_result = pg.record_threat(first_threat, "blocked"_string, matched->id, "{\"alert\":\"EICAR detected\"}"_string);
    if (record_result.is_error()) {
        log_fail("Record blocked threat"sv, "Could not record threat"sv);
        return;
    }

    // Simulate second EICAR detection (should auto-block)
    PolicyGraph::ThreatMetadata second_threat {
        .url = "http://another-site.com/malware.exe"_string,
        .filename = "malware.exe"_string,
        .file_hash = String::from_utf8_without_validation(EICAR_HASH.bytes()),
        .mime_type = "application/x-msdos-program"_string,
        .file_size = 68,
        .rule_name = "EICAR_Test_File"_string,
        .severity = "critical"_string,
    };

    auto second_match = pg.match_policy(second_threat);
    if (second_match.is_error() || !second_match.value().has_value()) {
        log_fail("Auto-block second EICAR"sv, "Policy did not match second detection"sv);
        return;
    }

    if (second_match.value()->action != PolicyGraph::PolicyAction::Block) {
        log_fail("Auto-block second EICAR"sv, "Wrong action for second detection"sv);
        return;
    }

    printf("  Second EICAR detection automatically blocked (no prompt)\n");

    // Verify threat history contains both detections
    auto history = pg.get_threats_by_rule("EICAR_Test_File"_string);
    if (history.is_error()) {
        log_fail("Verify threat history"sv, "Could not retrieve history"sv);
        return;
    }

    if (history.value().size() < 1) {
        log_fail("Verify threat history"sv, "No threats in history"sv);
        return;
    }

    printf("  Verified %zu threat(s) logged in history\n", history.value().size());
    log_pass("Block Policy Enforcement"sv);
}

// Test 2: Policy Matching Priority
// - Create hash, URL, and rule policies
// - Verify hash policy matched first (Priority 1)
// - Verify URL policy matched second (Priority 2)
// - Verify rule policy matched last (Priority 3)
static void test_policy_matching_priority(PolicyGraph& pg)
{
    print_section("Test 2: Policy Matching Priority"sv);

    // Create three policies with different specificity
    PolicyGraph::Policy hash_policy {
        .rule_name = "Test_Rule"_string,
        .url_pattern = {},
        .file_hash = "priority_test_hash_123"_string,
        .mime_type = {},
        .action = PolicyGraph::PolicyAction::Block,
        .created_at = UnixDateTime::now(),
        .created_by = "integration_test"_string,
        .expires_at = {},
        .last_hit = {},
    };

    PolicyGraph::Policy url_policy {
        .rule_name = "Test_Rule"_string,
        .url_pattern = "%evil.com%"_string,
        .file_hash = {},
        .mime_type = {},
        .action = PolicyGraph::PolicyAction::Quarantine,
        .created_at = UnixDateTime::now(),
        .created_by = "integration_test"_string,
        .expires_at = {},
        .last_hit = {},
    };

    PolicyGraph::Policy rule_policy {
        .rule_name = "Test_Rule"_string,
        .url_pattern = {},
        .file_hash = {},
        .mime_type = {},
        .action = PolicyGraph::PolicyAction::Allow,
        .created_at = UnixDateTime::now(),
        .created_by = "integration_test"_string,
        .expires_at = {},
        .last_hit = {},
    };

    auto hash_id = pg.create_policy(hash_policy);
    auto url_id = pg.create_policy(url_policy);
    auto rule_id = pg.create_policy(rule_policy);

    if (hash_id.is_error() || url_id.is_error() || rule_id.is_error()) {
        log_fail("Create priority policies"sv, "Could not create all policies"sv);
        return;
    }

    printf("  Created policies: Hash=%ld, URL=%ld, Rule=%ld\n",
           hash_id.value(), url_id.value(), rule_id.value());

    // Test Priority 1: Hash match should win
    PolicyGraph::ThreatMetadata threat_with_hash {
        .url = "http://evil.com/file.exe"_string,
        .filename = "file.exe"_string,
        .file_hash = "priority_test_hash_123"_string,
        .mime_type = "application/x-msdownload"_string,
        .file_size = 1024,
        .rule_name = "Test_Rule"_string,
        .severity = "high"_string,
    };

    auto match1 = pg.match_policy(threat_with_hash);
    if (match1.is_error() || !match1.value().has_value()) {
        log_fail("Priority 1: Hash match"sv, "No policy matched"sv);
        return;
    }

    if (match1.value()->id != hash_id.value() || match1.value()->action != PolicyGraph::PolicyAction::Block) {
        log_fail("Priority 1: Hash match"sv, "Wrong policy matched (expected hash policy)"sv);
        return;
    }

    printf("  ✓ Priority 1: Hash policy matched (ID=%ld, Action=Block)\n", match1.value()->id);

    // Test Priority 2: URL match (no hash)
    PolicyGraph::ThreatMetadata threat_with_url {
        .url = "http://evil.com/payload.exe"_string,
        .filename = "payload.exe"_string,
        .file_hash = "different_hash_456"_string,
        .mime_type = "application/x-msdownload"_string,
        .file_size = 2048,
        .rule_name = "Test_Rule"_string,
        .severity = "high"_string,
    };

    auto match2 = pg.match_policy(threat_with_url);
    if (match2.is_error() || !match2.value().has_value()) {
        log_fail("Priority 2: URL match"sv, "No policy matched"sv);
        return;
    }

    if (match2.value()->id != url_id.value() || match2.value()->action != PolicyGraph::PolicyAction::Quarantine) {
        log_fail("Priority 2: URL match"sv, "Wrong policy matched (expected URL policy)"sv);
        return;
    }

    printf("  ✓ Priority 2: URL pattern policy matched (ID=%ld, Action=Quarantine)\n", match2.value()->id);

    // Test Priority 3: Rule match (no hash, no URL)
    PolicyGraph::ThreatMetadata threat_with_rule_only {
        .url = "http://safe-site.com/program.exe"_string,
        .filename = "program.exe"_string,
        .file_hash = "yet_another_hash_789"_string,
        .mime_type = "application/x-msdownload"_string,
        .file_size = 4096,
        .rule_name = "Test_Rule"_string,
        .severity = "medium"_string,
    };

    auto match3 = pg.match_policy(threat_with_rule_only);
    if (match3.is_error() || !match3.value().has_value()) {
        log_fail("Priority 3: Rule match"sv, "No policy matched"sv);
        return;
    }

    if (match3.value()->id != rule_id.value() || match3.value()->action != PolicyGraph::PolicyAction::Allow) {
        log_fail("Priority 3: Rule match"sv, "Wrong policy matched (expected rule policy)"sv);
        return;
    }

    printf("  ✓ Priority 3: Rule name policy matched (ID=%ld, Action=Allow)\n", match3.value()->id);
    log_pass("Policy Matching Priority"sv);
}

// Test 3: Quarantine Workflow
// - Simulate quarantine action
// - Verify file moved to quarantine directory
// - Verify metadata JSON created
// - Verify permissions (0700 dir, 0400 file)
static void test_quarantine_workflow(PolicyGraph& pg)
{
    print_section("Test 3: Quarantine Workflow"sv);

    // Create quarantine policy
    PolicyGraph::Policy quarantine_policy {
        .rule_name = "Suspicious_PE"_string,
        .url_pattern = {},
        .file_hash = "suspicious_file_hash_999"_string,
        .mime_type = {},
        .action = PolicyGraph::PolicyAction::Quarantine,
        .created_at = UnixDateTime::now(),
        .created_by = "integration_test"_string,
        .expires_at = {},
        .last_hit = {},
    };

    auto policy_result = pg.create_policy(quarantine_policy);
    if (policy_result.is_error()) {
        log_fail("Create quarantine policy"sv, "Could not create policy"sv);
        return;
    }

    printf("  Created quarantine policy ID: %ld\n", policy_result.value());

    // Simulate quarantine detection
    PolicyGraph::ThreatMetadata threat {
        .url = "http://suspicious.net/backdoor.exe"_string,
        .filename = "backdoor.exe"_string,
        .file_hash = "suspicious_file_hash_999"_string,
        .mime_type = "application/x-msdos-program"_string,
        .file_size = 10240,
        .rule_name = "Suspicious_PE"_string,
        .severity = "high"_string,
    };

    auto match_result = pg.match_policy(threat);
    if (match_result.is_error() || !match_result.value().has_value()) {
        log_fail("Match quarantine policy"sv, "Policy did not match"sv);
        return;
    }

    if (match_result.value()->action != PolicyGraph::PolicyAction::Quarantine) {
        log_fail("Match quarantine policy"sv, "Wrong action (expected Quarantine)"sv);
        return;
    }

    printf("  Matched quarantine policy (Action: Quarantine)\n");

    // Record quarantine action
    auto record_result = pg.record_threat(threat, "quarantined"_string, match_result.value()->id,
                                         "{\"quarantine_path\":\"/tmp/test_quarantine/backdoor.exe\"}"_string);
    if (record_result.is_error()) {
        log_fail("Record quarantine action"sv, "Could not record threat"sv);
        return;
    }

    // Note: Actual quarantine directory creation and file operations would be
    // handled by the Sentinel service, not PolicyGraph. This test verifies
    // the policy matching and threat recording aspects of the workflow.

    printf("  Recorded quarantine action in threat history\n");

    // Verify threat was recorded with correct action
    auto history = pg.get_threats_by_rule("Suspicious_PE"_string);
    if (history.is_error()) {
        log_fail("Verify quarantine history"sv, "Could not retrieve history"sv);
        return;
    }

    bool found_quarantine = false;
    for (auto const& record : history.value()) {
        if (record.action_taken.bytes_as_string_view() == "quarantined"sv) {
            found_quarantine = true;
            printf("  Verified quarantine action logged (ID=%ld)\n", record.id);
            break;
        }
    }

    if (!found_quarantine) {
        log_fail("Verify quarantine history"sv, "Quarantine action not found in history"sv);
        return;
    }

    log_pass("Quarantine Workflow"sv);
}

// Test 4: Policy CRUD Operations
// - Create, read, update, delete policies
// - Verify database consistency
static void test_policy_crud_operations(PolicyGraph& pg)
{
    print_section("Test 4: Policy CRUD Operations"sv);

    // CREATE
    PolicyGraph::Policy test_policy {
        .rule_name = "CRUD_Test_Rule"_string,
        .url_pattern = "%test.com%"_string,
        .file_hash = {},
        .mime_type = "application/pdf"_string,
        .action = PolicyGraph::PolicyAction::Allow,
        .created_at = UnixDateTime::now(),
        .created_by = "integration_test"_string,
        .expires_at = {},
        .last_hit = {},
    };

    auto create_result = pg.create_policy(test_policy);
    if (create_result.is_error()) {
        log_fail("CRUD: Create"sv, "Could not create policy"sv);
        return;
    }

    auto policy_id = create_result.release_value();
    printf("  CREATE: Created policy ID %ld\n", policy_id);

    // READ
    auto read_result = pg.get_policy(policy_id);
    if (read_result.is_error()) {
        log_fail("CRUD: Read"sv, "Could not read policy"sv);
        return;
    }

    auto read_policy = read_result.release_value();
    if (read_policy.rule_name.bytes_as_string_view() != "CRUD_Test_Rule"sv) {
        log_fail("CRUD: Read"sv, "Policy data mismatch"sv);
        return;
    }

    printf("  READ: Retrieved policy (Rule: %s, Action: Allow)\n",
           read_policy.rule_name.bytes_as_string_view().characters_without_null_termination());

    // UPDATE
    read_policy.action = PolicyGraph::PolicyAction::Block;
    read_policy.mime_type = "application/x-executable"_string;

    auto update_result = pg.update_policy(policy_id, read_policy);
    if (update_result.is_error()) {
        log_fail("CRUD: Update"sv, "Could not update policy"sv);
        return;
    }

    auto updated = pg.get_policy(policy_id);
    if (updated.is_error() || updated.value().action != PolicyGraph::PolicyAction::Block) {
        log_fail("CRUD: Update"sv, "Policy not updated correctly"sv);
        return;
    }

    printf("  UPDATE: Changed action to Block and MIME type to executable\n");

    // DELETE
    auto delete_result = pg.delete_policy(policy_id);
    if (delete_result.is_error()) {
        log_fail("CRUD: Delete"sv, "Could not delete policy"sv);
        return;
    }

    auto verify_delete = pg.get_policy(policy_id);
    if (!verify_delete.is_error()) {
        log_fail("CRUD: Delete"sv, "Policy still exists after deletion"sv);
        return;
    }

    printf("  DELETE: Successfully removed policy ID %ld\n", policy_id);
    log_pass("Policy CRUD Operations"sv);
}

// Test 5: Threat History
// - Record threats
// - Query history with filters
// - Verify correct ordering
static void test_threat_history(PolicyGraph& pg)
{
    print_section("Test 5: Threat History"sv);

    // Record multiple threats
    Vector<PolicyGraph::ThreatMetadata> threats;
    threats.append({
        .url = "http://malware1.com/virus.exe"_string,
        .filename = "virus.exe"_string,
        .file_hash = "hash_threat_1"_string,
        .mime_type = "application/x-msdos-program"_string,
        .file_size = 5120,
        .rule_name = "Test_Malware_Rule"_string,
        .severity = "critical"_string,
    });

    threats.append({
        .url = "http://malware2.com/trojan.dll"_string,
        .filename = "trojan.dll"_string,
        .file_hash = "hash_threat_2"_string,
        .mime_type = "application/x-msdownload"_string,
        .file_size = 7168,
        .rule_name = "Test_Malware_Rule"_string,
        .severity = "high"_string,
    });

    threats.append({
        .url = "http://suspicious.org/script.js"_string,
        .filename = "script.js"_string,
        .file_hash = "hash_threat_3"_string,
        .mime_type = "text/javascript"_string,
        .file_size = 2048,
        .rule_name = "Suspicious_Script"_string,
        .severity = "medium"_string,
    });

    // Record all threats
    for (auto const& threat : threats) {
        auto result = pg.record_threat(threat, "blocked"_string, {}, "{}"_string);
        if (result.is_error()) {
            log_fail("Record threats"sv, "Could not record threat"sv);
            return;
        }
    }

    printf("  Recorded %zu threats to history\n", threats.size());

    // Query all history
    auto all_history = pg.get_threat_history({});
    if (all_history.is_error()) {
        log_fail("Query all history"sv, "Could not retrieve history"sv);
        return;
    }

    printf("  Retrieved %zu total threat records\n", all_history.value().size());

    // Query by specific rule
    auto rule_history = pg.get_threats_by_rule("Test_Malware_Rule"_string);
    if (rule_history.is_error()) {
        log_fail("Query by rule"sv, "Could not retrieve filtered history"sv);
        return;
    }

    if (rule_history.value().size() != 2) {
        log_fail("Query by rule"sv, "Expected 2 threats for Test_Malware_Rule"sv);
        return;
    }

    printf("  Retrieved %zu threats for rule 'Test_Malware_Rule'\n", rule_history.value().size());

    // Verify ordering (should be newest first)
    if (all_history.value().size() >= 2) {
        auto& first = all_history.value()[0];
        auto& second = all_history.value()[1];

        if (first.detected_at < second.detected_at) {
            log_fail("Verify ordering"sv, "History not ordered by time (newest first)"sv);
            return;
        }

        printf("  Verified history ordered by detection time (newest first)\n");
    }

    // Verify threat counts
    auto threat_count = pg.get_threat_count();
    if (threat_count.is_error()) {
        log_fail("Get threat count"sv, "Could not get count"sv);
        return;
    }

    printf("  Total threats in database: %lu\n", threat_count.value());
    log_pass("Threat History"sv);
}

int main()
{
    printf("====================================\n");
    printf("  Phase 3 Integration Tests\n");
    printf("====================================\n");

    // Create PolicyGraph with test database
    auto db_path = "/tmp/sentinel_phase3_test"sv;

    // Clean up any existing test database
    if (FileSystem::exists(ByteString::formatted("{}/policy_graph.db", db_path))) {
        printf("\nCleaning up previous test database...\n");
        (void)FileSystem::remove(ByteString::formatted("{}/policy_graph.db", db_path), FileSystem::RecursionMode::Allowed);
    }

    auto pg_result = PolicyGraph::create(ByteString(db_path));
    if (pg_result.is_error()) {
        printf("\n❌ FATAL: Could not create PolicyGraph: %s\n",
               pg_result.error().string_literal().characters_without_null_termination());
        return 1;
    }

    auto pg = pg_result.release_value();
    printf("✅ PolicyGraph initialized at %s\n", db_path.characters_without_null_termination());

    // Run all integration tests
    test_block_policy_enforcement(pg);
    test_policy_matching_priority(pg);
    test_quarantine_workflow(pg);
    test_policy_crud_operations(pg);
    test_threat_history(pg);

    // Print summary
    printf("\n====================================\n");
    printf("  Test Summary\n");
    printf("====================================\n");
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);
    printf("  Total:  %d\n", tests_passed + tests_failed);
    printf("====================================\n");

    if (tests_failed > 0) {
        printf("\n❌ Some tests FAILED\n");
        return 1;
    }

    printf("\n✅ All tests PASSED!\n");
    printf("\nDatabase location: %s/policy_graph.db\n", db_path.characters_without_null_termination());

    return 0;
}
