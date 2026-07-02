/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibDatabase/Database.h>
#include <LibWebView/SessionStore.h>

namespace WebView {

static constexpr u32 SESSIONS_SCHEMA_BASELINE_VERSION = 1u;

ErrorOr<Database::MigrationOutcome> SessionStore::migrate_schema(Database::Database& database, Database::MigrationMode mode)
{
    Array<Database::Migration, 1> migrations { {
        { .version = SESSIONS_SCHEMA_BASELINE_VERSION, .sql = R"#(
            CREATE TABLE IF NOT EXISTS Sessions (
                id INTEGER PRIMARY KEY,
                kind INTEGER NOT NULL,
                closed_time INTEGER NOT NULL,
                active_tab_index INTEGER NOT NULL
            );

            CREATE TABLE IF NOT EXISTS SessionTabs (
                id INTEGER PRIMARY KEY,
                session_id INTEGER NOT NULL REFERENCES Sessions(id) ON DELETE CASCADE,
                tab_ordinal INTEGER NOT NULL,
                active_url TEXT NOT NULL,
                current_used_step_index INTEGER NOT NULL
            );

            CREATE TABLE IF NOT EXISTS SessionUsedSteps (
                tab_id INTEGER NOT NULL REFERENCES SessionTabs(id) ON DELETE CASCADE,
                step_ordinal INTEGER NOT NULL,
                step INTEGER NOT NULL
            );

            CREATE TABLE IF NOT EXISTS SessionHistories (
                id INTEGER PRIMARY KEY,
                tab_id INTEGER NOT NULL REFERENCES SessionTabs(id) ON DELETE CASCADE
            );

            CREATE TABLE IF NOT EXISTS SessionNestedHistories (
                parent_history_id INTEGER NOT NULL,
                parent_entry_ordinal INTEGER NOT NULL,
                nested_ordinal INTEGER NOT NULL,
                nested_history_namespace_id INTEGER NOT NULL,
                nested_history_local_id INTEGER NOT NULL,
                child_history_id INTEGER NOT NULL UNIQUE REFERENCES SessionHistories(id) ON DELETE CASCADE,
                FOREIGN KEY (parent_history_id, parent_entry_ordinal) REFERENCES SessionEntries(history_id, entry_ordinal) ON DELETE CASCADE
            );

            CREATE TABLE IF NOT EXISTS SessionEntries (
                id INTEGER PRIMARY KEY,
                history_id INTEGER NOT NULL REFERENCES SessionHistories(id) ON DELETE CASCADE,
                entry_ordinal INTEGER NOT NULL,
                step INTEGER NOT NULL,
                url TEXT NOT NULL,
                document_state_namespace_id INTEGER NOT NULL,
                document_state_local_id INTEGER NOT NULL,
                classic_state BLOB NOT NULL,
                navigation_state BLOB NOT NULL,
                navigation_api_key TEXT NOT NULL,
                navigation_api_id TEXT NOT NULL,
                scroll_restoration_mode INTEGER NOT NULL,
                scroll_x_raw INTEGER,
                scroll_y_raw INTEGER,
                origin_kind INTEGER NOT NULL,
                origin_nonce BLOB,
                origin_scheme TEXT,
                origin_host TEXT,
                origin_port INTEGER,
                origin_domain TEXT,
                initiator_origin_kind INTEGER NOT NULL,
                initiator_origin_nonce BLOB,
                initiator_origin_scheme TEXT,
                initiator_origin_host TEXT,
                initiator_origin_port INTEGER,
                initiator_origin_domain TEXT,
                referrer_kind INTEGER NOT NULL,
                referrer_url TEXT,
                referrer_policy INTEGER NOT NULL,
                resource_kind INTEGER NOT NULL,
                resource_string TEXT,
                about_base_url TEXT,
                navigable_target_name TEXT NOT NULL,
                ever_populated INTEGER NOT NULL,
                reload_pending INTEGER NOT NULL
            );

            CREATE TABLE IF NOT EXISTS SessionPolicyContainers (
                id INTEGER PRIMARY KEY,
                history_id INTEGER NOT NULL,
                entry_ordinal INTEGER NOT NULL,
                referrer_policy INTEGER NOT NULL,
                embedder_policy_value INTEGER NOT NULL,
                embedder_policy_report_only_value INTEGER NOT NULL,
                embedder_policy_reporting_endpoint TEXT NOT NULL,
                embedder_policy_report_only_reporting_endpoint TEXT NOT NULL,
                FOREIGN KEY (history_id, entry_ordinal) REFERENCES SessionEntries(history_id, entry_ordinal) ON DELETE CASCADE
            );

            CREATE TABLE IF NOT EXISTS SessionCspPolicies (
                id INTEGER PRIMARY KEY,
                container_id INTEGER NOT NULL REFERENCES SessionPolicyContainers(id) ON DELETE CASCADE,
                policy_ordinal INTEGER NOT NULL,
                disposition INTEGER NOT NULL,
                source INTEGER NOT NULL,
                self_origin_kind INTEGER NOT NULL,
                self_origin_nonce BLOB,
                self_origin_scheme TEXT,
                self_origin_host TEXT,
                self_origin_port INTEGER,
                self_origin_domain TEXT,
                pre_parsed_policy_string TEXT NOT NULL
            );

            CREATE TABLE IF NOT EXISTS SessionCspDirectives (
                id INTEGER PRIMARY KEY,
                policy_id INTEGER NOT NULL REFERENCES SessionCspPolicies(id) ON DELETE CASCADE,
                directive_ordinal INTEGER NOT NULL,
                name TEXT NOT NULL
            );

            CREATE TABLE IF NOT EXISTS SessionCspDirectiveValues (
                directive_id INTEGER NOT NULL REFERENCES SessionCspDirectives(id) ON DELETE CASCADE,
                value_ordinal INTEGER NOT NULL,
                value TEXT NOT NULL
            );

            CREATE INDEX IF NOT EXISTS SessionsRetentionIndex
            ON Sessions(kind, closed_time DESC, id DESC);

            CREATE INDEX IF NOT EXISTS SessionTabsBySessionIndex
            ON SessionTabs(session_id);

            CREATE UNIQUE INDEX IF NOT EXISTS SessionUsedStepsByTabIndex
            ON SessionUsedSteps(tab_id, step_ordinal);

            CREATE INDEX IF NOT EXISTS SessionHistoriesByTabIndex
            ON SessionHistories(tab_id);

            CREATE UNIQUE INDEX IF NOT EXISTS SessionNestedHistoriesByParentIndex
            ON SessionNestedHistories(parent_history_id, parent_entry_ordinal, nested_ordinal);

            CREATE UNIQUE INDEX IF NOT EXISTS SessionEntriesByHistoryIndex
            ON SessionEntries(history_id, entry_ordinal);

            CREATE UNIQUE INDEX IF NOT EXISTS SessionPolicyContainersByHistoryIndex
            ON SessionPolicyContainers(history_id, entry_ordinal);

            CREATE UNIQUE INDEX IF NOT EXISTS SessionCspPoliciesByContainerIndex
            ON SessionCspPolicies(container_id, policy_ordinal);

            CREATE UNIQUE INDEX IF NOT EXISTS SessionCspDirectivesByPolicyIndex
            ON SessionCspDirectives(policy_id, directive_ordinal);

            CREATE UNIQUE INDEX IF NOT EXISTS SessionCspDirectiveValuesByDirectiveIndex
            ON SessionCspDirectiveValues(directive_id, value_ordinal);
        )#"sv },
    } };

    return database.migrate("Sessions"sv, migrations, mode);
}

}
