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
#include <LibWeb/ContentSecurityPolicy/Policy.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/HTML/EmbedderPolicy.h>
#include <LibWeb/HTML/POSTResource.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/ReferrerPolicy/ReferrerPolicy.h>
#include <LibWebView/Export.h>
#include <LibWebView/SessionHistory.h>

namespace WebView {

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

WEBVIEW_API PersistedOrigin encode_origin(Optional<URL::Origin> const&);
WEBVIEW_API ErrorOr<Optional<URL::Origin>> decode_origin(PersistedOrigin const&);

struct PersistedReferrer {
    i64 kind { 0 };
    Optional<String> url {};
};

WEBVIEW_API PersistedReferrer encode_referrer(Web::Fetch::Infrastructure::Request::ReferrerType const&);
WEBVIEW_API ErrorOr<Web::Fetch::Infrastructure::Request::ReferrerType> decode_referrer(PersistedReferrer const&);

WEBVIEW_API i64 encode_referrer_policy(Web::ReferrerPolicy::ReferrerPolicy);
WEBVIEW_API ErrorOr<Web::ReferrerPolicy::ReferrerPolicy> decode_referrer_policy(i64 tag);

struct PersistedResource {
    i64 kind { 0 };
    Optional<Utf16String> string {};
};

// v1 does not persist POST bodies: a POST resource encodes as Empty, so decode never yields one.
WEBVIEW_API PersistedResource encode_resource(Web::HTML::DocumentResource const&);
WEBVIEW_API ErrorOr<Web::HTML::DocumentResource> decode_resource(PersistedResource const&);

WEBVIEW_API i64 encode_scroll_restoration_mode(Web::HTML::ScrollRestorationMode);
WEBVIEW_API ErrorOr<Web::HTML::ScrollRestorationMode> decode_scroll_restoration_mode(i64 tag);

WEBVIEW_API i64 encode_embedder_policy_value(Web::HTML::EmbedderPolicyValue);
WEBVIEW_API ErrorOr<Web::HTML::EmbedderPolicyValue> decode_embedder_policy_value(i64 tag);

WEBVIEW_API i64 encode_csp_disposition(Web::ContentSecurityPolicy::Policy::Disposition);
WEBVIEW_API ErrorOr<Web::ContentSecurityPolicy::Policy::Disposition> decode_csp_disposition(i64 tag);

WEBVIEW_API i64 encode_csp_source(Web::ContentSecurityPolicy::Policy::Source);
WEBVIEW_API ErrorOr<Web::ContentSecurityPolicy::Policy::Source> decode_csp_source(i64 tag);

struct SessionHistorySnapshotStatements {
    Database::StatementID insert_history;
    Database::StatementID insert_nested_history_link;
    Database::StatementID insert_entry;
    Database::StatementID insert_used_step;
    Database::StatementID insert_policy_container;
    Database::StatementID insert_csp_policy;
    Database::StatementID insert_csp_directive;
    Database::StatementID insert_csp_directive_value;
    Database::StatementID select_histories;
    Database::StatementID select_nested_history_links;
    Database::StatementID select_entries;
    Database::StatementID select_used_steps;
    Database::StatementID select_policy_containers;
    Database::StatementID select_csp_policies;
    Database::StatementID select_csp_directives;
    Database::StatementID select_csp_directive_values;
    Database::StatementID delete_histories;
    Database::StatementID delete_used_steps;
};

WEBVIEW_API ErrorOr<SessionHistorySnapshotStatements> prepare_session_history_snapshot_statements(Database::Database&);

WEBVIEW_API ErrorOr<void> validate_session_history_snapshot_storable(SessionHistorySnapshot const&);

// Validates before writing; the caller owns the transaction.
WEBVIEW_API ErrorOr<void> store_session_history_snapshot(Database::Database&, SessionHistorySnapshotStatements const&, i64 tab_id, SessionHistorySnapshot const&);
WEBVIEW_API ErrorOr<SessionHistorySnapshot> load_session_history_snapshot(Database::Database&, SessionHistorySnapshotStatements const&, i64 tab_id, size_t current_used_step_index);
WEBVIEW_API ErrorOr<void> delete_session_history_snapshot(Database::Database&, SessionHistorySnapshotStatements const&, i64 tab_id);

}
