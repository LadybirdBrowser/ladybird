/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <LibDatabase/Database.h>
#include <LibURL/Origin.h>
#include <LibWebCommon/ContentSecurityPolicy/PolicySource.h>
#include <LibWebCommon/Fetch/Infrastructure/HTTP/RequestReferrer.h>
#include <LibWebCommon/HTML/EmbedderPolicy.h>
#include <LibWebCommon/HTML/POSTResource.h>
#include <LibWebCommon/HTML/SessionHistoryEntryDescriptor.h>
#include <LibWebCommon/ReferrerPolicy/ReferrerPolicy.h>
#include <LibWebCommon/UpgradeInsecureRequests/InsecureRequestsPolicy.h>
#include <LibWebView/SessionHistory.h>

namespace Ladybird {

// Column form of an Optional<URL::Origin>, richer than the lossy Origin::serialize() (which drops the
// tuple domain and renders opaque origins as "null").
struct PersistedOrigin {
    i64 kind { 0 };
    Optional<URL::Origin::OpaqueData::Nonce> nonce {};
    Optional<String> scheme {};
    Optional<String> host {};
    Optional<u16> port {};
    Optional<String> domain {};
};

PersistedOrigin encode_origin(Optional<URL::Origin> const&);
ErrorOr<Optional<URL::Origin>> decode_origin(PersistedOrigin const&);

struct PersistedReferrer {
    i64 kind { 0 };
    Optional<String> url {};
};

PersistedReferrer encode_referrer(Web::Fetch::Infrastructure::RequestReferrerType const&);
ErrorOr<Web::Fetch::Infrastructure::RequestReferrerType> decode_referrer(PersistedReferrer const&);

i64 encode_referrer_policy(Web::ReferrerPolicy::ReferrerPolicy);
ErrorOr<Web::ReferrerPolicy::ReferrerPolicy> decode_referrer_policy(i64 tag);

struct PersistedResource {
    i64 kind { 0 };
    Optional<Utf16String> string {};
};

// v1 does not persist POST bodies: a POST resource encodes as Empty, so decode never yields one.
PersistedResource encode_resource(Web::HTML::DocumentResource const&);
ErrorOr<Web::HTML::DocumentResource> decode_resource(PersistedResource const&);

i64 encode_scroll_restoration_mode(Web::HTML::ScrollRestorationMode);
ErrorOr<Web::HTML::ScrollRestorationMode> decode_scroll_restoration_mode(i64 tag);

i64 encode_embedder_policy_value(Web::HTML::EmbedderPolicyValue);
ErrorOr<Web::HTML::EmbedderPolicyValue> decode_embedder_policy_value(i64 tag);

i64 encode_csp_disposition(Web::Bindings::SecurityPolicyViolationEventDisposition);
ErrorOr<Web::Bindings::SecurityPolicyViolationEventDisposition> decode_csp_disposition(i64 tag);

i64 encode_csp_source(Web::ContentSecurityPolicy::PolicySource);
ErrorOr<Web::ContentSecurityPolicy::PolicySource> decode_csp_source(i64 tag);

i64 encode_insecure_requests_policy(Web::UpgradeInsecureRequests::InsecureRequestsPolicy);
ErrorOr<Web::UpgradeInsecureRequests::InsecureRequestsPolicy> decode_insecure_requests_policy(i64 tag);

struct SessionHistorySnapshotStatements {
    Database::StatementID insert_history;
    Database::StatementID insert_nested_history_link;
    Database::StatementID insert_entry;
    Database::StatementID insert_used_step;
    Database::StatementID insert_policy_container;
    Database::StatementID insert_csp_policy;
    Database::StatementID insert_csp_directive;
    Database::StatementID insert_csp_directive_value;
    Database::StatementID insert_upgrade_insecure_navigation;
    Database::StatementID select_histories;
    Database::StatementID select_nested_history_links;
    Database::StatementID select_entries;
    Database::StatementID select_used_steps;
    Database::StatementID select_policy_containers;
    Database::StatementID select_csp_policies;
    Database::StatementID select_csp_directives;
    Database::StatementID select_csp_directive_values;
    Database::StatementID select_upgrade_insecure_navigations;
    Database::StatementID delete_histories;
    Database::StatementID delete_used_steps;
};

ErrorOr<SessionHistorySnapshotStatements> prepare_session_history_snapshot_statements(Database::Database&);

ErrorOr<void> validate_session_history_snapshot_storable(WebView::SessionHistorySnapshot const&);

// Validates before writing; the caller owns the transaction.
ErrorOr<void> store_session_history_snapshot(Database::Database&, SessionHistorySnapshotStatements const&, i64 tab_id, WebView::SessionHistorySnapshot const&);
ErrorOr<WebView::SessionHistorySnapshot> load_session_history_snapshot(Database::Database&, SessionHistorySnapshotStatements const&, i64 tab_id, size_t current_used_step_index);
ErrorOr<void> delete_session_history_snapshot(Database::Database&, SessionHistorySnapshotStatements const&, i64 tab_id);

}
