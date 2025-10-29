/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "PolicyGraph.h"
#include <AK/StringBuilder.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>

namespace Sentinel {

ErrorOr<PolicyGraph> PolicyGraph::create(ByteString const& db_directory)
{
    // Ensure directory exists
    if (!FileSystem::exists(db_directory))
        TRY(Core::System::mkdir(db_directory, 0755));

    // Create/open database
    auto database = TRY(Database::Database::create(db_directory, "policy_graph"sv));

    // Create policies table
    auto create_policies_table = TRY(database->prepare_statement(R"#(
        CREATE TABLE IF NOT EXISTS policies (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            rule_name TEXT NOT NULL,
            url_pattern TEXT,
            file_hash TEXT,
            mime_type TEXT,
            action TEXT NOT NULL,
            created_at INTEGER NOT NULL,
            created_by TEXT NOT NULL,
            expires_at INTEGER,
            hit_count INTEGER DEFAULT 0,
            last_hit INTEGER
        );
    )#"sv));
    database->execute_statement(create_policies_table, {});

    // Create indexes on policies table
    auto create_rule_name_index = TRY(database->prepare_statement(
        "CREATE INDEX IF NOT EXISTS idx_policies_rule_name ON policies(rule_name);"sv));
    database->execute_statement(create_rule_name_index, {});

    auto create_file_hash_index = TRY(database->prepare_statement(
        "CREATE INDEX IF NOT EXISTS idx_policies_file_hash ON policies(file_hash);"sv));
    database->execute_statement(create_file_hash_index, {});

    auto create_url_pattern_index = TRY(database->prepare_statement(
        "CREATE INDEX IF NOT EXISTS idx_policies_url_pattern ON policies(url_pattern);"sv));
    database->execute_statement(create_url_pattern_index, {});

    // Create threat_history table
    auto create_threats_table = TRY(database->prepare_statement(R"#(
        CREATE TABLE IF NOT EXISTS threat_history (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            detected_at INTEGER NOT NULL,
            url TEXT NOT NULL,
            filename TEXT NOT NULL,
            file_hash TEXT NOT NULL,
            mime_type TEXT,
            file_size INTEGER NOT NULL,
            rule_name TEXT NOT NULL,
            severity TEXT NOT NULL,
            action_taken TEXT NOT NULL,
            policy_id INTEGER,
            alert_json TEXT NOT NULL,
            FOREIGN KEY (policy_id) REFERENCES policies(id)
        );
    )#"sv));
    database->execute_statement(create_threats_table, {});

    // Create indexes on threat_history table
    auto create_detected_at_index = TRY(database->prepare_statement(
        "CREATE INDEX IF NOT EXISTS idx_threat_history_detected_at ON threat_history(detected_at);"sv));
    database->execute_statement(create_detected_at_index, {});

    auto create_threat_rule_index = TRY(database->prepare_statement(
        "CREATE INDEX IF NOT EXISTS idx_threat_history_rule_name ON threat_history(rule_name);"sv));
    database->execute_statement(create_threat_rule_index, {});

    auto create_threat_hash_index = TRY(database->prepare_statement(
        "CREATE INDEX IF NOT EXISTS idx_threat_history_file_hash ON threat_history(file_hash);"sv));
    database->execute_statement(create_threat_hash_index, {});

    // Prepare all statements
    Statements statements {};

    // Policy CRUD statements
    statements.create_policy = TRY(database->prepare_statement(R"#(
        INSERT INTO policies (rule_name, url_pattern, file_hash, mime_type, action,
                             created_at, created_by, expires_at, hit_count, last_hit)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, 0, NULL);
    )#"sv));

    statements.get_last_insert_id = TRY(database->prepare_statement(
        "SELECT last_insert_rowid();"sv));

    statements.get_policy = TRY(database->prepare_statement(
        "SELECT * FROM policies WHERE id = ?;"sv));

    statements.list_policies = TRY(database->prepare_statement(
        "SELECT * FROM policies ORDER BY created_at DESC;"sv));

    statements.update_policy = TRY(database->prepare_statement(R"#(
        UPDATE policies
        SET rule_name = ?, url_pattern = ?, file_hash = ?, mime_type = ?,
            action = ?, expires_at = ?
        WHERE id = ?;
    )#"sv));

    statements.delete_policy = TRY(database->prepare_statement(
        "DELETE FROM policies WHERE id = ?;"sv));

    statements.increment_hit_count = TRY(database->prepare_statement(
        "UPDATE policies SET hit_count = hit_count + 1, last_hit = ? WHERE id = ?;"sv));

    statements.update_last_hit = TRY(database->prepare_statement(
        "UPDATE policies SET last_hit = ? WHERE id = ?;"sv));

    // Policy matching statements (priority order)
    statements.match_by_hash = TRY(database->prepare_statement(R"#(
        SELECT * FROM policies
        WHERE file_hash = ?
          AND (expires_at = -1 OR expires_at > ?)
        LIMIT 1;
    )#"sv));

    statements.match_by_url_pattern = TRY(database->prepare_statement(R"#(
        SELECT * FROM policies
        WHERE url_pattern != ''
          AND ? LIKE url_pattern
          AND (expires_at = -1 OR expires_at > ?)
        LIMIT 1;
    )#"sv));

    statements.match_by_rule_name = TRY(database->prepare_statement(R"#(
        SELECT * FROM policies
        WHERE rule_name = ?
          AND (file_hash = '' OR file_hash IS NULL)
          AND (url_pattern = '' OR url_pattern IS NULL)
          AND (expires_at = -1 OR expires_at > ?)
        LIMIT 1;
    )#"sv));

    // Threat history statements
    statements.record_threat = TRY(database->prepare_statement(R"#(
        INSERT INTO threat_history
            (detected_at, url, filename, file_hash, mime_type, file_size,
             rule_name, severity, action_taken, policy_id, alert_json)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
    )#"sv));

    statements.get_threats_since = TRY(database->prepare_statement(
        "SELECT * FROM threat_history WHERE detected_at >= ? ORDER BY detected_at DESC;"sv));

    statements.get_threats_all = TRY(database->prepare_statement(
        "SELECT * FROM threat_history ORDER BY detected_at DESC;"sv));

    statements.get_threats_by_rule = TRY(database->prepare_statement(
        "SELECT * FROM threat_history WHERE rule_name = ? ORDER BY detected_at DESC;"sv));

    // Utility statements
    statements.delete_expired_policies = TRY(database->prepare_statement(
        "DELETE FROM policies WHERE expires_at IS NOT NULL AND expires_at <= ?;"sv));

    statements.count_policies = TRY(database->prepare_statement(
        "SELECT COUNT(*) FROM policies;"sv));

    statements.count_threats = TRY(database->prepare_statement(
        "SELECT COUNT(*) FROM threat_history;"sv));

    return PolicyGraph { move(database), statements };
}

PolicyGraph::PolicyGraph(NonnullRefPtr<Database::Database> database, Statements statements)
    : m_database(move(database))
    , m_statements(statements)
{
}

// Policy CRUD implementations

ErrorOr<i64> PolicyGraph::create_policy(Policy const& policy)
{
    // Convert action enum to string
    auto action_str = action_to_string(policy.action);

    // Convert optional expiration to i64 or null
    Optional<i64> expires_ms;
    if (policy.expires_at.has_value())
        expires_ms = policy.expires_at->milliseconds_since_epoch();

    // Execute insert
    m_database->execute_statement(
        m_statements.create_policy,
        {},
        policy.rule_name,
        policy.url_pattern.has_value() ? policy.url_pattern.value() : String {},
        policy.file_hash.has_value() ? policy.file_hash.value() : String {},
        policy.mime_type.has_value() ? policy.mime_type.value() : String {},
        action_str,
        policy.created_at.milliseconds_since_epoch(),
        policy.created_by,
        expires_ms.has_value() ? expires_ms.value() : -1
    );

    // Get last insert ID
    i64 last_id = 0;
    m_database->execute_statement(
        m_statements.get_last_insert_id,
        [&](auto statement_id) {
            last_id = m_database->result_column<i64>(statement_id, 0);
        }
    );
    return last_id;
}

ErrorOr<PolicyGraph::Policy> PolicyGraph::get_policy(i64 policy_id)
{
    Optional<Policy> result;

    m_database->execute_statement(
        m_statements.get_policy,
        [&](auto statement_id) {
            Policy policy;
            int col = 0;
            policy.id = m_database->result_column<i64>(statement_id, col++);
            policy.rule_name = m_database->result_column<String>(statement_id, col++);

            auto url_pattern = m_database->result_column<String>(statement_id, col++);
            if (!url_pattern.is_empty())
                policy.url_pattern = url_pattern;

            auto file_hash = m_database->result_column<String>(statement_id, col++);
            if (!file_hash.is_empty())
                policy.file_hash = file_hash;

            auto mime_type = m_database->result_column<String>(statement_id, col++);
            if (!mime_type.is_empty())
                policy.mime_type = mime_type;

            auto action_str = m_database->result_column<String>(statement_id, col++);
            policy.action = string_to_action(action_str);

            policy.created_at = m_database->result_column<UnixDateTime>(statement_id, col++);
            policy.created_by = m_database->result_column<String>(statement_id, col++);

            auto expires_ms = m_database->result_column<i64>(statement_id, col++);
            if (expires_ms > 0)
                policy.expires_at = UnixDateTime::from_milliseconds_since_epoch(expires_ms);

            policy.hit_count = m_database->result_column<i64>(statement_id, col++);

            auto last_hit_ms = m_database->result_column<i64>(statement_id, col++);
            if (last_hit_ms > 0)
                policy.last_hit = UnixDateTime::from_milliseconds_since_epoch(last_hit_ms);

            result = policy;
        },
        policy_id
    );

    if (!result.has_value())
        return Error::from_string_literal("Policy not found");

    return result.release_value();
}

ErrorOr<Vector<PolicyGraph::Policy>> PolicyGraph::list_policies()
{
    Vector<Policy> policies;

    m_database->execute_statement(
        m_statements.list_policies,
        [&](auto statement_id) {
            Policy policy;
            int col = 0;
            policy.id = m_database->result_column<i64>(statement_id, col++);
            policy.rule_name = m_database->result_column<String>(statement_id, col++);

            auto url_pattern = m_database->result_column<String>(statement_id, col++);
            if (!url_pattern.is_empty())
                policy.url_pattern = url_pattern;

            auto file_hash = m_database->result_column<String>(statement_id, col++);
            if (!file_hash.is_empty())
                policy.file_hash = file_hash;

            auto mime_type = m_database->result_column<String>(statement_id, col++);
            if (!mime_type.is_empty())
                policy.mime_type = mime_type;

            auto action_str = m_database->result_column<String>(statement_id, col++);
            policy.action = string_to_action(action_str);

            policy.created_at = m_database->result_column<UnixDateTime>(statement_id, col++);
            policy.created_by = m_database->result_column<String>(statement_id, col++);

            auto expires_ms = m_database->result_column<i64>(statement_id, col++);
            if (expires_ms > 0)
                policy.expires_at = UnixDateTime::from_milliseconds_since_epoch(expires_ms);

            policy.hit_count = m_database->result_column<i64>(statement_id, col++);

            auto last_hit_ms = m_database->result_column<i64>(statement_id, col++);
            if (last_hit_ms > 0)
                policy.last_hit = UnixDateTime::from_milliseconds_since_epoch(last_hit_ms);

            policies.append(move(policy));
        }
    );

    return policies;
}

ErrorOr<void> PolicyGraph::update_policy(i64 policy_id, Policy const& policy)
{
    auto action_str = action_to_string(policy.action);

    Optional<i64> expires_ms;
    if (policy.expires_at.has_value())
        expires_ms = policy.expires_at->milliseconds_since_epoch();

    m_database->execute_statement(
        m_statements.update_policy,
        {},
        policy.rule_name,
        policy.url_pattern.has_value() ? policy.url_pattern.value() : String {},
        policy.file_hash.has_value() ? policy.file_hash.value() : String {},
        policy.mime_type.has_value() ? policy.mime_type.value() : String {},
        action_str,
        expires_ms.has_value() ? expires_ms.value() : -1,
        policy_id
    );

    return {};
}

ErrorOr<void> PolicyGraph::delete_policy(i64 policy_id)
{
    m_database->execute_statement(m_statements.delete_policy, {}, policy_id);
    return {};
}

// Policy matching implementation

ErrorOr<Optional<PolicyGraph::Policy>> PolicyGraph::match_policy(ThreatMetadata const& threat)
{
    auto now = UnixDateTime::now().milliseconds_since_epoch();

    // Priority 1: Match by file hash (most specific)
    if (!threat.file_hash.is_empty()) {
        Optional<Policy> match;
        m_database->execute_statement(
            m_statements.match_by_hash,
            [&](auto statement_id) {
                Policy policy;
                int col = 0;
                policy.id = m_database->result_column<i64>(statement_id, col++);
                policy.rule_name = m_database->result_column<String>(statement_id, col++);

                auto url_pattern = m_database->result_column<String>(statement_id, col++);
                if (!url_pattern.is_empty())
                    policy.url_pattern = url_pattern;

                auto file_hash = m_database->result_column<String>(statement_id, col++);
                if (!file_hash.is_empty())
                    policy.file_hash = file_hash;

                auto mime_type = m_database->result_column<String>(statement_id, col++);
                if (!mime_type.is_empty())
                    policy.mime_type = mime_type;

                auto action_str = m_database->result_column<String>(statement_id, col++);
                policy.action = string_to_action(action_str);

                policy.created_at = m_database->result_column<UnixDateTime>(statement_id, col++);
                policy.created_by = m_database->result_column<String>(statement_id, col++);

                auto expires_ms = m_database->result_column<i64>(statement_id, col++);
                if (expires_ms > 0)
                    policy.expires_at = UnixDateTime::from_milliseconds_since_epoch(expires_ms);

                policy.hit_count = m_database->result_column<i64>(statement_id, col++);

                auto last_hit_ms = m_database->result_column<i64>(statement_id, col++);
                if (last_hit_ms > 0)
                    policy.last_hit = UnixDateTime::from_milliseconds_since_epoch(last_hit_ms);

                match = policy;
            },
            threat.file_hash,
            now
        );

        if (match.has_value()) {
            // Update hit statistics
            m_database->execute_statement(m_statements.increment_hit_count, {}, now, match->id);
            return match;
        }
    }

    // Priority 2: Match by URL pattern
    {
        Optional<Policy> match;
        m_database->execute_statement(
            m_statements.match_by_url_pattern,
            [&](auto statement_id) {
                Policy policy;
                int col = 0;
                policy.id = m_database->result_column<i64>(statement_id, col++);
                policy.rule_name = m_database->result_column<String>(statement_id, col++);

                auto url_pattern = m_database->result_column<String>(statement_id, col++);
                if (!url_pattern.is_empty())
                    policy.url_pattern = url_pattern;

                auto file_hash = m_database->result_column<String>(statement_id, col++);
                if (!file_hash.is_empty())
                    policy.file_hash = file_hash;

                auto mime_type = m_database->result_column<String>(statement_id, col++);
                if (!mime_type.is_empty())
                    policy.mime_type = mime_type;

                auto action_str = m_database->result_column<String>(statement_id, col++);
                policy.action = string_to_action(action_str);

                policy.created_at = m_database->result_column<UnixDateTime>(statement_id, col++);
                policy.created_by = m_database->result_column<String>(statement_id, col++);

                auto expires_ms = m_database->result_column<i64>(statement_id, col++);
                if (expires_ms > 0)
                    policy.expires_at = UnixDateTime::from_milliseconds_since_epoch(expires_ms);

                policy.hit_count = m_database->result_column<i64>(statement_id, col++);

                auto last_hit_ms = m_database->result_column<i64>(statement_id, col++);
                if (last_hit_ms > 0)
                    policy.last_hit = UnixDateTime::from_milliseconds_since_epoch(last_hit_ms);

                match = policy;
            },
            threat.url,
            now
        );

        if (match.has_value()) {
            m_database->execute_statement(m_statements.increment_hit_count, {}, now, match->id);
            return match;
        }
    }

    // Priority 3: Match by rule name (least specific)
    {
        Optional<Policy> match;
        m_database->execute_statement(
            m_statements.match_by_rule_name,
            [&](auto statement_id) {
                Policy policy;
                int col = 0;
                policy.id = m_database->result_column<i64>(statement_id, col++);
                policy.rule_name = m_database->result_column<String>(statement_id, col++);

                auto url_pattern = m_database->result_column<String>(statement_id, col++);
                if (!url_pattern.is_empty())
                    policy.url_pattern = url_pattern;

                auto file_hash = m_database->result_column<String>(statement_id, col++);
                if (!file_hash.is_empty())
                    policy.file_hash = file_hash;

                auto mime_type = m_database->result_column<String>(statement_id, col++);
                if (!mime_type.is_empty())
                    policy.mime_type = mime_type;

                auto action_str = m_database->result_column<String>(statement_id, col++);
                policy.action = string_to_action(action_str);

                policy.created_at = m_database->result_column<UnixDateTime>(statement_id, col++);
                policy.created_by = m_database->result_column<String>(statement_id, col++);

                auto expires_ms = m_database->result_column<i64>(statement_id, col++);
                if (expires_ms > 0)
                    policy.expires_at = UnixDateTime::from_milliseconds_since_epoch(expires_ms);

                policy.hit_count = m_database->result_column<i64>(statement_id, col++);

                auto last_hit_ms = m_database->result_column<i64>(statement_id, col++);
                if (last_hit_ms > 0)
                    policy.last_hit = UnixDateTime::from_milliseconds_since_epoch(last_hit_ms);

                match = policy;
            },
            threat.rule_name,
            now
        );

        if (match.has_value()) {
            m_database->execute_statement(m_statements.increment_hit_count, {}, now, match->id);
            return match;
        }
    }

    // No matching policy found
    return Optional<Policy> {};
}

// Threat history implementations

ErrorOr<void> PolicyGraph::record_threat(ThreatMetadata const& threat,
                                        String action_taken,
                                        Optional<i64> policy_id,
                                        String alert_json)
{
    m_database->execute_statement(
        m_statements.record_threat,
        {},
        UnixDateTime::now().milliseconds_since_epoch(),
        threat.url,
        threat.filename,
        threat.file_hash,
        threat.mime_type,
        threat.file_size,
        threat.rule_name,
        threat.severity,
        action_taken,
        policy_id.has_value() ? policy_id.value() : -1,
        alert_json
    );

    return {};
}

ErrorOr<Vector<PolicyGraph::ThreatRecord>> PolicyGraph::get_threat_history(Optional<UnixDateTime> since)
{
    Vector<ThreatRecord> threats;

    if (since.has_value()) {
        m_database->execute_statement(
            m_statements.get_threats_since,
            [&](auto stmt_id) {
                ThreatRecord record;
                int col = 0;
                record.id = m_database->result_column<i64>(stmt_id, col++);
                record.detected_at = m_database->result_column<UnixDateTime>(stmt_id, col++);
                record.url = m_database->result_column<String>(stmt_id, col++);
                record.filename = m_database->result_column<String>(stmt_id, col++);
                record.file_hash = m_database->result_column<String>(stmt_id, col++);
                record.mime_type = m_database->result_column<String>(stmt_id, col++);
                record.file_size = m_database->result_column<u64>(stmt_id, col++);
                record.rule_name = m_database->result_column<String>(stmt_id, col++);
                record.severity = m_database->result_column<String>(stmt_id, col++);
                record.action_taken = m_database->result_column<String>(stmt_id, col++);

                auto policy_id = m_database->result_column<i64>(stmt_id, col++);
                if (policy_id > 0)
                    record.policy_id = policy_id;

                record.alert_json = m_database->result_column<String>(stmt_id, col++);

                threats.append(move(record));
            },
            since->milliseconds_since_epoch()
        );
    } else {
        m_database->execute_statement(
            m_statements.get_threats_all,
            [&](auto stmt_id) {
                ThreatRecord record;
                int col = 0;
                record.id = m_database->result_column<i64>(stmt_id, col++);
                record.detected_at = m_database->result_column<UnixDateTime>(stmt_id, col++);
                record.url = m_database->result_column<String>(stmt_id, col++);
                record.filename = m_database->result_column<String>(stmt_id, col++);
                record.file_hash = m_database->result_column<String>(stmt_id, col++);
                record.mime_type = m_database->result_column<String>(stmt_id, col++);
                record.file_size = m_database->result_column<u64>(stmt_id, col++);
                record.rule_name = m_database->result_column<String>(stmt_id, col++);
                record.severity = m_database->result_column<String>(stmt_id, col++);
                record.action_taken = m_database->result_column<String>(stmt_id, col++);

                auto policy_id = m_database->result_column<i64>(stmt_id, col++);
                if (policy_id > 0)
                    record.policy_id = policy_id;

                record.alert_json = m_database->result_column<String>(stmt_id, col++);

                threats.append(move(record));
            }
        );
    }

    return threats;
}

ErrorOr<Vector<PolicyGraph::ThreatRecord>> PolicyGraph::get_threats_by_rule(String const& rule_name)
{
    Vector<ThreatRecord> threats;

    m_database->execute_statement(
        m_statements.get_threats_by_rule,
        [&](auto stmt_id) {
            ThreatRecord record;
            int col = 0;
            record.id = m_database->result_column<i64>(stmt_id, col++);
            record.detected_at = m_database->result_column<UnixDateTime>(stmt_id, col++);
            record.url = m_database->result_column<String>(stmt_id, col++);
            record.filename = m_database->result_column<String>(stmt_id, col++);
            record.file_hash = m_database->result_column<String>(stmt_id, col++);
            record.mime_type = m_database->result_column<String>(stmt_id, col++);
            record.file_size = m_database->result_column<u64>(stmt_id, col++);
            record.rule_name = m_database->result_column<String>(stmt_id, col++);
            record.severity = m_database->result_column<String>(stmt_id, col++);
            record.action_taken = m_database->result_column<String>(stmt_id, col++);

            auto policy_id = m_database->result_column<i64>(stmt_id, col++);
            if (policy_id > 0)
                record.policy_id = policy_id;

            record.alert_json = m_database->result_column<String>(stmt_id, col++);

            threats.append(move(record));
        },
        rule_name
    );

    return threats;
}

// Utility implementations

ErrorOr<void> PolicyGraph::cleanup_expired_policies()
{
    auto now = UnixDateTime::now().milliseconds_since_epoch();
    m_database->execute_statement(m_statements.delete_expired_policies, {}, now);
    return {};
}

ErrorOr<u64> PolicyGraph::get_policy_count()
{
    u64 count = 0;
    m_database->execute_statement(
        m_statements.count_policies,
        [&](auto statement_id) {
            count = m_database->result_column<u64>(statement_id, 0);
        }
    );
    return count;
}

ErrorOr<u64> PolicyGraph::get_threat_count()
{
    u64 count = 0;
    m_database->execute_statement(
        m_statements.count_threats,
        [&](auto statement_id) {
            count = m_database->result_column<u64>(statement_id, 0);
        }
    );
    return count;
}

// Utility conversion functions

PolicyGraph::PolicyAction PolicyGraph::string_to_action(String const& action_str)
{
    if (action_str == "allow"sv)
        return PolicyAction::Allow;
    if (action_str == "block"sv)
        return PolicyAction::Block;
    if (action_str == "quarantine"sv)
        return PolicyAction::Quarantine;

    return PolicyAction::Block; // Default to block for safety
}

String PolicyGraph::action_to_string(PolicyAction action)
{
    switch (action) {
    case PolicyAction::Allow:
        return "allow"_string;
    case PolicyAction::Block:
        return "block"_string;
    case PolicyAction::Quarantine:
        return "quarantine"_string;
    }
    VERIFY_NOT_REACHED();
}

}
