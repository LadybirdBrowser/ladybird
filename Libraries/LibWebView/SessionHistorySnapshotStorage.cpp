/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/QuickSort.h>
#include <LibDatabase/ResultRow.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibWeb/ContentSecurityPolicy/SerializedPolicy.h>
#include <LibWeb/HTML/DocumentState.h>
#include <LibWeb/HTML/SerializedPolicyContainer.h>
#include <LibWebView/SessionHistorySnapshotStorage.h>

namespace WebView {

// Schema-stable tags for PersistedOrigin::kind. These values are persisted; never reorder or reuse them.
enum class OriginKind : i64 {
    Empty = 0,
    OpaqueStandard = 1,
    OpaqueFile = 2,
    Tuple = 3,
};

static Optional<OriginKind> origin_kind_from_tag(i64 tag)
{
    switch (tag) {
    case 0:
        return OriginKind::Empty;
    case 1:
        return OriginKind::OpaqueStandard;
    case 2:
        return OriginKind::OpaqueFile;
    case 3:
        return OriginKind::Tuple;
    default:
        return {};
    }
}

// Persist opaque ID components as signed bit patterns; restore remaps them.
static i64 encode_cross_process_id_component(u64 component)
{
    return bit_cast<i64>(component);
}

static u64 decode_cross_process_id_component(i64 stored_component)
{
    return bit_cast<u64>(stored_component);
}

static constexpr size_t NONCE_COLUMN_BYTES = sizeof(URL::Origin::OpaqueData::Nonce);

static Optional<ReadonlyBytes> nonce_column(Optional<URL::Origin::OpaqueData::Nonce> const& nonce)
{
    return nonce.map([](auto const& value) { return value.span(); });
}

PersistedOrigin encode_origin(Optional<URL::Origin> const& origin)
{
    if (!origin.has_value())
        return { .kind = to_underlying(OriginKind::Empty) };

    if (origin->is_opaque()) {
        auto const& opaque = origin->opaque_data();
        auto kind = opaque.type == URL::Origin::OpaqueData::Type::File ? OriginKind::OpaqueFile : OriginKind::OpaqueStandard;
        return {
            .kind = to_underlying(kind),
            .nonce = opaque.nonce,
        };
    }

    return {
        .kind = to_underlying(OriginKind::Tuple),
        .scheme = origin->scheme().value_or(String {}),
        .host = origin->host().serialize(),
        .port = origin->port(),
        .domain = origin->domain().map([](auto const& domain) { return domain.serialize(); }),
    };
}

static ErrorOr<Optional<URL::Origin>> decode_tuple_origin(PersistedOrigin const& persisted)
{
    auto port = persisted.port;

    if (!persisted.scheme.has_value() || persisted.scheme->is_empty())
        return Error::from_string_literal("Persisted tuple origin has an empty scheme");
    if (!persisted.host.has_value())
        return Error::from_string_literal("Persisted tuple origin has no host");

    auto host_is_empty = persisted.host->is_empty();
    URL::Host host = String {};
    if (!host_is_empty) {
        auto parsed_host = URL::Parser::parse_host(*persisted.host);
        if (!parsed_host.has_value())
            return Error::from_string_literal("Persisted tuple origin has an unparseable host");
        host = parsed_host.release_value();
    }

    // Only accept the tuple-origin shapes URL::origin() actually produces.
    auto scheme = persisted.scheme->bytes_as_string_view();
    if (scheme.is_one_of("http"sv, "https"sv, "ws"sv, "wss"sv, "ftp"sv)) {
        if (host_is_empty)
            return Error::from_string_literal("Persisted network-scheme tuple origin has an empty host");
    } else if (scheme == "resource"sv) {
        if (!host_is_empty || port.has_value())
            return Error::from_string_literal("Persisted resource tuple origin has a host or port");
    } else if (scheme == "file"sv) {
        if (!host_is_empty || port.has_value())
            return Error::from_string_literal("Persisted file tuple origin has a host or port");
        if (!URL::file_scheme_urls_have_tuple_origins())
            return Error::from_string_literal("Persisted file tuple origin while file tuple origins are disabled");
    } else {
        return Error::from_string_literal("Persisted tuple origin has a scheme that cannot form a tuple origin");
    }

    // A domain must equal or suffix a non-empty host, so hostless origins never carry one.
    Optional<URL::Host> domain;
    if (persisted.domain.has_value()) {
        if (host_is_empty)
            return Error::from_string_literal("Persisted tuple origin has a domain with an empty host");
        // parse_host asserts on empty input.
        if (persisted.domain->is_empty())
            return Error::from_string_literal("Persisted tuple origin has an empty domain");
        auto parsed_domain = URL::Parser::parse_host(*persisted.domain);
        if (!parsed_domain.has_value())
            return Error::from_string_literal("Persisted tuple origin has an unparseable domain");
        domain = parsed_domain.release_value();
    }

    return Optional<URL::Origin> { URL::Origin { persisted.scheme, host, port, domain } };
}

ErrorOr<Optional<URL::Origin>> decode_origin(PersistedOrigin const& persisted)
{
    auto kind = origin_kind_from_tag(persisted.kind);
    if (!kind.has_value())
        return Error::from_string_literal("Persisted origin has an unknown kind");

    switch (*kind) {
    case OriginKind::Empty:
        return Optional<URL::Origin> {};
    case OriginKind::OpaqueStandard:
    case OriginKind::OpaqueFile: {
        if (!persisted.nonce.has_value())
            return Error::from_string_literal("Persisted opaque origin has no nonce");
        auto type = *kind == OriginKind::OpaqueFile ? URL::Origin::OpaqueData::Type::File : URL::Origin::OpaqueData::Type::Standard;
        return Optional<URL::Origin> { URL::Origin { URL::Origin::OpaqueData { .nonce = *persisted.nonce, .type = type } } };
    }
    case OriginKind::Tuple:
        return decode_tuple_origin(persisted);
    }
    VERIFY_NOT_REACHED();
}

using Referrer = Web::Fetch::Infrastructure::Request::Referrer;
using ReferrerType = Web::Fetch::Infrastructure::Request::ReferrerType;

// Schema-stable tags for PersistedReferrer::kind. These values are persisted; never reorder or reuse them.
enum class ReferrerKind : i64 {
    Client = 0,
    NoReferrer = 1,
    Url = 2,
};

static Optional<ReferrerKind> referrer_kind_from_tag(i64 tag)
{
    switch (tag) {
    case 0:
        return ReferrerKind::Client;
    case 1:
        return ReferrerKind::NoReferrer;
    case 2:
        return ReferrerKind::Url;
    default:
        return {};
    }
}

PersistedReferrer encode_referrer(ReferrerType const& referrer)
{
    return referrer.visit(
        [](Referrer kind) -> PersistedReferrer {
            return { .kind = to_underlying(kind == Referrer::NoReferrer ? ReferrerKind::NoReferrer : ReferrerKind::Client) };
        },
        [](URL::URL const& url) -> PersistedReferrer {
            return { .kind = to_underlying(ReferrerKind::Url), .url = url.serialize() };
        });
}

ErrorOr<ReferrerType> decode_referrer(PersistedReferrer const& persisted)
{
    auto kind = referrer_kind_from_tag(persisted.kind);
    if (!kind.has_value())
        return Error::from_string_literal("Persisted referrer has an unknown kind");

    switch (*kind) {
    case ReferrerKind::Client:
        return ReferrerType { Referrer::Client };
    case ReferrerKind::NoReferrer:
        return ReferrerType { Referrer::NoReferrer };
    case ReferrerKind::Url: {
        if (!persisted.url.has_value())
            return Error::from_string_literal("Persisted referrer has no url");
        auto url = URL::Parser::basic_parse(*persisted.url);
        if (!url.has_value())
            return Error::from_string_literal("Persisted referrer has an unparseable url");
        return ReferrerType { url.release_value() };
    }
    }
    VERIFY_NOT_REACHED();
}

i64 encode_referrer_policy(Web::ReferrerPolicy::ReferrerPolicy policy)
{
    using RP = Web::ReferrerPolicy::ReferrerPolicy;
    switch (policy) {
    case RP::EmptyString:
        return 0;
    case RP::NoReferrer:
        return 1;
    case RP::NoReferrerWhenDowngrade:
        return 2;
    case RP::SameOrigin:
        return 3;
    case RP::Origin:
        return 4;
    case RP::StrictOrigin:
        return 5;
    case RP::OriginWhenCrossOrigin:
        return 6;
    case RP::StrictOriginWhenCrossOrigin:
        return 7;
    case RP::UnsafeURL:
        return 8;
    }
    VERIFY_NOT_REACHED();
}

ErrorOr<Web::ReferrerPolicy::ReferrerPolicy> decode_referrer_policy(i64 tag)
{
    using RP = Web::ReferrerPolicy::ReferrerPolicy;
    switch (tag) {
    case 0:
        return RP::EmptyString;
    case 1:
        return RP::NoReferrer;
    case 2:
        return RP::NoReferrerWhenDowngrade;
    case 3:
        return RP::SameOrigin;
    case 4:
        return RP::Origin;
    case 5:
        return RP::StrictOrigin;
    case 6:
        return RP::OriginWhenCrossOrigin;
    case 7:
        return RP::StrictOriginWhenCrossOrigin;
    case 8:
        return RP::UnsafeURL;
    default:
        return Error::from_string_literal("Persisted referrer policy has an unknown tag");
    }
}

// Schema-stable tags for PersistedResource::kind. These values are persisted; never reorder or reuse them.
enum class ResourceKind : i64 {
    Empty = 0,
    Srcdoc = 1,
};

static Optional<ResourceKind> resource_kind_from_tag(i64 tag)
{
    switch (tag) {
    case 0:
        return ResourceKind::Empty;
    case 1:
        return ResourceKind::Srcdoc;
    default:
        return {};
    }
}

PersistedResource encode_resource(Web::HTML::DocumentResource const& resource)
{
    // FIXME: POST bodies can contain sensitive data such as login credentials, and we cannot yet ask the user
    // to confirm resubmission when restoring an entry. Do not persist them until both are addressed.
    return resource.visit(
        [](Empty) -> PersistedResource { return { .kind = to_underlying(ResourceKind::Empty) }; },
        [](Utf16String const& srcdoc) -> PersistedResource { return { .kind = to_underlying(ResourceKind::Srcdoc), .string = srcdoc }; },
        [](Web::HTML::POSTResource const&) -> PersistedResource { return { .kind = to_underlying(ResourceKind::Empty) }; });
}

ErrorOr<Web::HTML::DocumentResource> decode_resource(PersistedResource const& persisted)
{
    auto kind = resource_kind_from_tag(persisted.kind);
    if (!kind.has_value())
        return Error::from_string_literal("Persisted resource has an unknown kind");

    switch (*kind) {
    case ResourceKind::Empty:
        return Web::HTML::DocumentResource { Empty {} };
    case ResourceKind::Srcdoc:
        if (!persisted.string.has_value())
            return Error::from_string_literal("Persisted srcdoc resource has no string");
        return Web::HTML::DocumentResource { *persisted.string };
    }
    VERIFY_NOT_REACHED();
}

// Schema-stable tags for the scroll restoration mode. These values are persisted; never reorder them.
i64 encode_scroll_restoration_mode(Web::HTML::ScrollRestorationMode mode)
{
    switch (mode) {
    case Web::HTML::ScrollRestorationMode::Auto:
        return 0;
    case Web::HTML::ScrollRestorationMode::Manual:
        return 1;
    }
    VERIFY_NOT_REACHED();
}

ErrorOr<Web::HTML::ScrollRestorationMode> decode_scroll_restoration_mode(i64 tag)
{
    switch (tag) {
    case 0:
        return Web::HTML::ScrollRestorationMode::Auto;
    case 1:
        return Web::HTML::ScrollRestorationMode::Manual;
    default:
        return Error::from_string_literal("Persisted scroll restoration mode has an unknown tag");
    }
}

// Schema-stable tags for the embedder policy value. These values are persisted; never reorder them.
i64 encode_embedder_policy_value(Web::HTML::EmbedderPolicyValue value)
{
    switch (value) {
    case Web::HTML::EmbedderPolicyValue::UnsafeNone:
        return 0;
    case Web::HTML::EmbedderPolicyValue::RequireCorp:
        return 1;
    case Web::HTML::EmbedderPolicyValue::Credentialless:
        return 2;
    }
    VERIFY_NOT_REACHED();
}

ErrorOr<Web::HTML::EmbedderPolicyValue> decode_embedder_policy_value(i64 tag)
{
    switch (tag) {
    case 0:
        return Web::HTML::EmbedderPolicyValue::UnsafeNone;
    case 1:
        return Web::HTML::EmbedderPolicyValue::RequireCorp;
    case 2:
        return Web::HTML::EmbedderPolicyValue::Credentialless;
    default:
        return Error::from_string_literal("Persisted embedder policy value has an unknown tag");
    }
}

// Schema-stable tags for the CSP policy disposition. These values are persisted; never reorder them.
i64 encode_csp_disposition(Web::ContentSecurityPolicy::Policy::Disposition disposition)
{
    switch (disposition) {
    case Web::ContentSecurityPolicy::Policy::Disposition::Enforce:
        return 0;
    case Web::ContentSecurityPolicy::Policy::Disposition::Report:
        return 1;
    }
    VERIFY_NOT_REACHED();
}

ErrorOr<Web::ContentSecurityPolicy::Policy::Disposition> decode_csp_disposition(i64 tag)
{
    switch (tag) {
    case 0:
        return Web::ContentSecurityPolicy::Policy::Disposition::Enforce;
    case 1:
        return Web::ContentSecurityPolicy::Policy::Disposition::Report;
    default:
        return Error::from_string_literal("Persisted CSP policy disposition has an unknown tag");
    }
}

// Schema-stable tags for the CSP policy source. These values are persisted; never reorder them.
i64 encode_csp_source(Web::ContentSecurityPolicy::Policy::Source source)
{
    switch (source) {
    case Web::ContentSecurityPolicy::Policy::Source::Header:
        return 0;
    case Web::ContentSecurityPolicy::Policy::Source::Meta:
        return 1;
    }
    VERIFY_NOT_REACHED();
}

ErrorOr<Web::ContentSecurityPolicy::Policy::Source> decode_csp_source(i64 tag)
{
    switch (tag) {
    case 0:
        return Web::ContentSecurityPolicy::Policy::Source::Header;
    case 1:
        return Web::ContentSecurityPolicy::Policy::Source::Meta;
    default:
        return Error::from_string_literal("Persisted CSP policy source has an unknown tag");
    }
}

static constexpr size_t MAX_STATE_RECORD_BYTES = 8uz * 1024 * 1024;
static constexpr size_t MAX_TEXT_COLUMN_BYTES = 8uz * 1024 * 1024;
static constexpr size_t MAX_SNAPSHOT_BYTES = 64uz * 1024 * 1024;

// Global row-count and byte budgets shared by validation and loading; each cell has a separate byte cap.
static constexpr size_t MAX_SNAPSHOT_HISTORIES = 4096;
static constexpr size_t MAX_SNAPSHOT_ENTRIES = 8192;
static constexpr size_t MAX_SNAPSHOT_CSP_POLICIES = 16384;
static constexpr size_t MAX_SNAPSHOT_CSP_DIRECTIVES = 65536;
static constexpr size_t MAX_SNAPSHOT_CSP_DIRECTIVE_VALUES = 65536;

struct SnapshotRowTotals {
    size_t histories { 1 }; // the root
    size_t entries { 0 };
    size_t csp_policies { 0 };
    size_t csp_directives { 0 };
    size_t csp_directive_values { 0 };
    size_t bytes { 0 };
};

static ErrorOr<void> add_snapshot_bytes(SnapshotRowTotals& totals, size_t byte_count)
{
    if (byte_count > MAX_SNAPSHOT_BYTES - totals.bytes)
        return Error::from_string_literal("Session history snapshot exceeds the maximum size");
    totals.bytes += byte_count;
    return {};
}

static ErrorOr<void> validate_text_column_within_cap(String const& text, SnapshotRowTotals& totals)
{
    if (text.bytes().size() > MAX_TEXT_COLUMN_BYTES)
        return Error::from_string_literal("Session history entry text column exceeds the maximum size");
    return add_snapshot_bytes(totals, text.bytes().size());
}

static ErrorOr<void> validate_text_column_within_cap(Utf16View const& text, SnapshotRowTotals& totals)
{
    auto utf8 = TRY(text.to_utf8());
    return validate_text_column_within_cap(utf8, totals);
}

template<typename TextType>
static ErrorOr<void> validate_text_column_within_cap(Optional<TextType> const& text, SnapshotRowTotals& totals)
{
    if (!text.has_value())
        return {};
    return validate_text_column_within_cap(*text, totals);
}

static ErrorOr<void> validate_persisted_origin_within_caps(PersistedOrigin const& origin, SnapshotRowTotals& totals)
{
    if (origin.nonce.has_value())
        TRY(add_snapshot_bytes(totals, NONCE_COLUMN_BYTES));
    TRY(validate_text_column_within_cap(origin.scheme, totals));
    TRY(validate_text_column_within_cap(origin.host, totals));
    TRY(validate_text_column_within_cap(origin.domain, totals));
    return {};
}

static ErrorOr<void> validate_entry_within_column_caps(Web::HTML::SessionHistoryEntryDescriptor const& entry, SnapshotRowTotals& totals)
{
    auto const& document_state = entry.document_state;

    if (entry.classic_history_api_state.data.size() > MAX_STATE_RECORD_BYTES)
        return Error::from_string_literal("Session history entry classic history state exceeds the maximum size");
    if (entry.navigation_api_state.data.size() > MAX_STATE_RECORD_BYTES)
        return Error::from_string_literal("Session history entry navigation API state exceeds the maximum size");
    TRY(add_snapshot_bytes(totals, entry.classic_history_api_state.data.size()));
    TRY(add_snapshot_bytes(totals, entry.navigation_api_state.data.size()));

    TRY(validate_text_column_within_cap(entry.url.serialize(), totals));
    TRY(validate_text_column_within_cap(entry.navigation_api_key, totals));
    TRY(validate_text_column_within_cap(entry.navigation_api_id, totals));
    TRY(validate_text_column_within_cap(document_state.navigable_target_name, totals));
    if (document_state.about_base_url.has_value())
        TRY(validate_text_column_within_cap(document_state.about_base_url->serialize(), totals));

    TRY(validate_persisted_origin_within_caps(encode_origin(document_state.origin), totals));
    TRY(validate_persisted_origin_within_caps(encode_origin(document_state.initiator_origin), totals));
    TRY(validate_text_column_within_cap(encode_referrer(document_state.request_referrer).url, totals));
    TRY(validate_text_column_within_cap(encode_resource(document_state.resource).string, totals));

    if (auto const* container = document_state.history_policy_container.get_pointer<Web::HTML::SerializedPolicyContainer>()) {
        TRY(validate_text_column_within_cap(container->embedder_policy.reporting_endpoint, totals));
        TRY(validate_text_column_within_cap(container->embedder_policy.report_only_reporting_endpoint, totals));
        for (auto const& policy : container->csp_list) {
            TRY(validate_persisted_origin_within_caps(encode_origin(policy.self_origin), totals));
            TRY(validate_text_column_within_cap(policy.pre_parsed_policy_string, totals));
            for (auto const& directive : policy.directives) {
                TRY(validate_text_column_within_cap(directive.name, totals));
                for (auto const& value : directive.value)
                    TRY(validate_text_column_within_cap(value, totals));
            }
        }
    }

    return {};
}

ErrorOr<SessionHistorySnapshotStatements> prepare_session_history_snapshot_statements(Database::Database& database)
{
    return SessionHistorySnapshotStatements {
        .insert_history = TRY(database.prepare_statement(
            "INSERT INTO SessionHistories (tab_id) VALUES (:tab_id) RETURNING id;"sv)),
        .insert_nested_history_link = TRY(database.prepare_statement(R"#(
            INSERT INTO SessionNestedHistories (
                parent_history_id, parent_entry_ordinal, nested_ordinal,
                nested_history_namespace_id, nested_history_local_id, child_history_id)
            VALUES (
                :parent_history_id, :parent_entry_ordinal, :nested_ordinal,
                :nested_history_namespace_id, :nested_history_local_id, :child_history_id);
        )#"sv)),
        .insert_entry = TRY(database.prepare_statement(R"#(
            INSERT INTO SessionEntries (
                history_id, entry_ordinal, step, url, document_state_namespace_id, document_state_local_id,
                classic_state, navigation_state,
                navigation_api_key, navigation_api_id, scroll_restoration_mode,
                scroll_x_raw, scroll_y_raw,
                origin_kind, origin_nonce, origin_scheme, origin_host, origin_port, origin_domain,
                initiator_origin_kind, initiator_origin_nonce, initiator_origin_scheme,
                initiator_origin_host, initiator_origin_port, initiator_origin_domain,
                referrer_kind, referrer_url, referrer_policy, resource_kind, resource_string,
                about_base_url, navigable_target_name, ever_populated, reload_pending)
            VALUES (
                :history_id, :entry_ordinal, :step, :url, :document_state_namespace_id, :document_state_local_id,
                :classic_state, :navigation_state,
                :navigation_api_key, :navigation_api_id, :scroll_restoration_mode,
                :scroll_x_raw, :scroll_y_raw,
                :origin_kind, :origin_nonce, :origin_scheme, :origin_host, :origin_port, :origin_domain,
                :initiator_origin_kind, :initiator_origin_nonce, :initiator_origin_scheme,
                :initiator_origin_host, :initiator_origin_port, :initiator_origin_domain,
                :referrer_kind, :referrer_url, :referrer_policy, :resource_kind, :resource_string,
                :about_base_url, :navigable_target_name, :ever_populated, :reload_pending);
        )#"sv)),
        .insert_used_step = TRY(database.prepare_statement(
            "INSERT INTO SessionUsedSteps (tab_id, step_ordinal, step) VALUES (:tab_id, :step_ordinal, :step);"sv)),
        .insert_policy_container = TRY(database.prepare_statement(R"#(
            INSERT INTO SessionPolicyContainers (
                history_id, entry_ordinal, referrer_policy, embedder_policy_value,
                embedder_policy_report_only_value, embedder_policy_reporting_endpoint,
                embedder_policy_report_only_reporting_endpoint)
            VALUES (
                :history_id, :entry_ordinal, :referrer_policy, :embedder_policy_value,
                :embedder_policy_report_only_value, :embedder_policy_reporting_endpoint,
                :embedder_policy_report_only_reporting_endpoint)
            RETURNING id;
        )#"sv)),
        .insert_csp_policy = TRY(database.prepare_statement(R"#(
            INSERT INTO SessionCspPolicies (
                container_id, policy_ordinal, disposition, source,
                self_origin_kind, self_origin_nonce, self_origin_scheme, self_origin_host,
                self_origin_port, self_origin_domain, pre_parsed_policy_string)
            VALUES (
                :container_id, :policy_ordinal, :disposition, :source,
                :self_origin_kind, :self_origin_nonce, :self_origin_scheme, :self_origin_host,
                :self_origin_port, :self_origin_domain, :pre_parsed_policy_string)
            RETURNING id;
        )#"sv)),
        .insert_csp_directive = TRY(database.prepare_statement(
            "INSERT INTO SessionCspDirectives (policy_id, directive_ordinal, name) VALUES (:policy_id, :directive_ordinal, :name) RETURNING id;"sv)),
        .insert_csp_directive_value = TRY(database.prepare_statement(
            "INSERT INTO SessionCspDirectiveValues (directive_id, value_ordinal, value) VALUES (:directive_id, :value_ordinal, :value);"sv)),
        .select_histories = TRY(database.prepare_statement(
            "SELECT id FROM SessionHistories WHERE tab_id = :tab_id;"sv)),
        .select_nested_history_links = TRY(database.prepare_statement(R"#(
            SELECT parent_history_id, parent_entry_ordinal, nested_ordinal,
                nested_history_namespace_id, nested_history_local_id, child_history_id
            FROM SessionNestedHistories
            INNER JOIN SessionHistories AS ParentHistories ON SessionNestedHistories.parent_history_id = ParentHistories.id
            INNER JOIN SessionHistories AS ChildHistories ON SessionNestedHistories.child_history_id = ChildHistories.id
            WHERE ParentHistories.tab_id = :tab_id OR ChildHistories.tab_id = :tab_id;
        )#"sv)),
        .select_entries = TRY(database.prepare_statement(R"#(
            SELECT history_id, entry_ordinal, step, url, document_state_namespace_id, document_state_local_id,
                classic_state, navigation_state,
                navigation_api_key, navigation_api_id, scroll_restoration_mode,
                scroll_x_raw, scroll_y_raw,
                origin_kind, origin_nonce, origin_scheme, origin_host, origin_port, origin_domain,
                initiator_origin_kind, initiator_origin_nonce, initiator_origin_scheme,
                initiator_origin_host, initiator_origin_port, initiator_origin_domain,
                referrer_kind, referrer_url, referrer_policy, resource_kind, resource_string,
                about_base_url, navigable_target_name, ever_populated, reload_pending
            FROM SessionEntries
            INNER JOIN SessionHistories ON SessionEntries.history_id = SessionHistories.id
            WHERE SessionHistories.tab_id = :tab_id;
        )#"sv)),
        .select_used_steps = TRY(database.prepare_statement(
            "SELECT step_ordinal, step FROM SessionUsedSteps WHERE tab_id = :tab_id;"sv)),
        .select_policy_containers = TRY(database.prepare_statement(R"#(
            SELECT SessionPolicyContainers.id AS id, history_id, entry_ordinal, referrer_policy,
                embedder_policy_value, embedder_policy_report_only_value,
                embedder_policy_reporting_endpoint, embedder_policy_report_only_reporting_endpoint
            FROM SessionPolicyContainers
            INNER JOIN SessionHistories ON SessionPolicyContainers.history_id = SessionHistories.id
            WHERE SessionHistories.tab_id = :tab_id;
        )#"sv)),
        .select_csp_policies = TRY(database.prepare_statement(R"#(
            SELECT SessionCspPolicies.id AS id, container_id, policy_ordinal, disposition, source,
                self_origin_kind, self_origin_nonce, self_origin_scheme, self_origin_host,
                self_origin_port, self_origin_domain, pre_parsed_policy_string
            FROM SessionCspPolicies
            INNER JOIN SessionPolicyContainers ON SessionCspPolicies.container_id = SessionPolicyContainers.id
            INNER JOIN SessionHistories ON SessionPolicyContainers.history_id = SessionHistories.id
            WHERE SessionHistories.tab_id = :tab_id;
        )#"sv)),
        .select_csp_directives = TRY(database.prepare_statement(R"#(
            SELECT SessionCspDirectives.id AS id, policy_id, directive_ordinal, name
            FROM SessionCspDirectives
            INNER JOIN SessionCspPolicies ON SessionCspDirectives.policy_id = SessionCspPolicies.id
            INNER JOIN SessionPolicyContainers ON SessionCspPolicies.container_id = SessionPolicyContainers.id
            INNER JOIN SessionHistories ON SessionPolicyContainers.history_id = SessionHistories.id
            WHERE SessionHistories.tab_id = :tab_id;
        )#"sv)),
        .select_csp_directive_values = TRY(database.prepare_statement(R"#(
            SELECT directive_id, value_ordinal, value
            FROM SessionCspDirectiveValues
            INNER JOIN SessionCspDirectives ON SessionCspDirectiveValues.directive_id = SessionCspDirectives.id
            INNER JOIN SessionCspPolicies ON SessionCspDirectives.policy_id = SessionCspPolicies.id
            INNER JOIN SessionPolicyContainers ON SessionCspPolicies.container_id = SessionPolicyContainers.id
            INNER JOIN SessionHistories ON SessionPolicyContainers.history_id = SessionHistories.id
            WHERE SessionHistories.tab_id = :tab_id;
        )#"sv)),
        .delete_histories = TRY(database.prepare_statement(
            "DELETE FROM SessionHistories WHERE tab_id = :tab_id;"sv)),
        .delete_used_steps = TRY(database.prepare_statement(
            "DELETE FROM SessionUsedSteps WHERE tab_id = :tab_id;"sv)),
    };
}

ErrorOr<void> delete_session_history_snapshot(Database::Database& database, SessionHistorySnapshotStatements const& statements, i64 tab_id)
{
    TRY(database.try_execute_bound_statement(statements.delete_histories, [&](auto& bind) -> ErrorOr<void> {
        return bind("tab_id"sv, tab_id);
    }));
    TRY(database.try_execute_bound_statement(statements.delete_used_steps, [&](auto& bind) -> ErrorOr<void> {
        return bind("tab_id"sv, tab_id);
    }));
    return {};
}

static ErrorOr<i64> insert_row_returning_id(Database::Database& database, Database::StatementID statement_id, auto&& bind_all)
{
    return database.try_execute_bound_statement_one<i64>(statement_id, forward<decltype(bind_all)>(bind_all), [](Database::ResultRow& result_row) -> ErrorOr<i64> {
        return result_row.read_integer<i64>("id"sv);
    });
}

static ErrorOr<void> store_policy_container(Database::Database& database, SessionHistorySnapshotStatements const& statements, i64 history_id, i64 entry_ordinal, Web::HTML::SerializedPolicyContainer const& container)
{
    auto container_id = TRY(insert_row_returning_id(database, statements.insert_policy_container, [&](auto& bind) -> ErrorOr<void> {
        TRY(bind("history_id"sv, history_id));
        TRY(bind("entry_ordinal"sv, entry_ordinal));
        TRY(bind("referrer_policy"sv, encode_referrer_policy(container.referrer_policy)));
        TRY(bind("embedder_policy_value"sv, encode_embedder_policy_value(container.embedder_policy.value)));
        TRY(bind("embedder_policy_report_only_value"sv, encode_embedder_policy_value(container.embedder_policy.report_only_value)));
        TRY(bind("embedder_policy_reporting_endpoint"sv, container.embedder_policy.reporting_endpoint));
        TRY(bind("embedder_policy_report_only_reporting_endpoint"sv, container.embedder_policy.report_only_reporting_endpoint));
        return {};
    }));

    i64 policy_ordinal = 0;
    for (auto const& policy : container.csp_list) {
        auto self_origin = encode_origin(policy.self_origin);
        auto policy_id = TRY(insert_row_returning_id(database, statements.insert_csp_policy, [&](auto& bind) -> ErrorOr<void> {
            TRY(bind("container_id"sv, container_id));
            TRY(bind("policy_ordinal"sv, policy_ordinal));
            TRY(bind("disposition"sv, encode_csp_disposition(policy.disposition)));
            TRY(bind("source"sv, encode_csp_source(policy.source)));
            TRY(bind("self_origin_kind"sv, self_origin.kind));
            TRY(bind("self_origin_nonce"sv, nonce_column(self_origin.nonce)));
            TRY(bind("self_origin_scheme"sv, self_origin.scheme));
            TRY(bind("self_origin_host"sv, self_origin.host));
            TRY(bind("self_origin_port"sv, self_origin.port));
            TRY(bind("self_origin_domain"sv, self_origin.domain));
            TRY(bind("pre_parsed_policy_string"sv, policy.pre_parsed_policy_string));
            return {};
        }));

        i64 directive_ordinal = 0;
        for (auto const& directive : policy.directives) {
            auto directive_id = TRY(insert_row_returning_id(database, statements.insert_csp_directive, [&](auto& bind) -> ErrorOr<void> {
                TRY(bind("policy_id"sv, policy_id));
                TRY(bind("directive_ordinal"sv, directive_ordinal));
                TRY(bind("name"sv, directive.name.to_utf16_string()));
                return {};
            }));

            i64 value_ordinal = 0;
            for (auto const& value : directive.value) {
                TRY(database.try_execute_bound_statement(statements.insert_csp_directive_value, [&](auto& bind) -> ErrorOr<void> {
                    TRY(bind("directive_id"sv, directive_id));
                    TRY(bind("value_ordinal"sv, value_ordinal));
                    TRY(bind("value"sv, value));
                    return {};
                }));
                ++value_ordinal;
            }
            ++directive_ordinal;
        }
        ++policy_ordinal;
    }

    return {};
}

static ErrorOr<void> validate_entries_storable(Vector<Web::HTML::SessionHistoryEntryDescriptor> const& entries, size_t depth, SnapshotRowTotals& totals)
{
    if (depth > MAX_NESTED_HISTORY_DEPTH)
        return Error::from_string_literal("Session history nests deeper than the supported budget");

    for (auto const& entry : entries) {
        auto const& document_state = entry.document_state;
        TRY(validate_entry_within_column_caps(entry, totals));

        if (++totals.entries > MAX_SNAPSHOT_ENTRIES)
            return Error::from_string_literal("Session history snapshot has more entries than the supported budget");

        if (auto const* container = document_state.history_policy_container.get_pointer<Web::HTML::SerializedPolicyContainer>()) {
            totals.csp_policies += container->csp_list.size();
            if (totals.csp_policies > MAX_SNAPSHOT_CSP_POLICIES)
                return Error::from_string_literal("Session history snapshot has more CSP policies than the supported budget");
            for (auto const& policy : container->csp_list) {
                totals.csp_directives += policy.directives.size();
                if (totals.csp_directives > MAX_SNAPSHOT_CSP_DIRECTIVES)
                    return Error::from_string_literal("Session history snapshot has more CSP directives than the supported budget");
                for (auto const& directive : policy.directives) {
                    totals.csp_directive_values += directive.value.size();
                    if (totals.csp_directive_values > MAX_SNAPSHOT_CSP_DIRECTIVE_VALUES)
                        return Error::from_string_literal("Session history snapshot has more CSP directive values than the supported budget");
                }
            }
        }

        for (auto const& nested_history : document_state.nested_histories) {
            if (++totals.histories > MAX_SNAPSHOT_HISTORIES)
                return Error::from_string_literal("Session history snapshot has more histories than the supported budget");
            TRY(validate_entries_storable(nested_history.entries, depth + 1, totals));
        }
    }

    return {};
}

ErrorOr<void> validate_session_history_snapshot_storable(SessionHistorySnapshot const& snapshot)
{
    SnapshotRowTotals totals;
    TRY(validate_entries_storable(snapshot.entries, 0, totals));
    return validate_snapshot_is_restorable(snapshot.entries, snapshot.used_steps, snapshot.current_used_step_index);
}

static ErrorOr<void> store_history_entries(Database::Database& database, SessionHistorySnapshotStatements const& statements, i64 tab_id, i64 history_id, Vector<Web::HTML::SessionHistoryEntryDescriptor> const& entries)
{
    i64 entry_ordinal = 0;
    for (auto const& entry : entries) {
        auto const& document_state = entry.document_state;
        auto const& scroll = entry.scroll_position_data.viewport_scroll_position;
        auto origin = encode_origin(document_state.origin);
        auto initiator_origin = encode_origin(document_state.initiator_origin);
        auto referrer = encode_referrer(document_state.request_referrer);
        auto resource = encode_resource(document_state.resource);
        auto about_base_url = document_state.about_base_url.map([](auto const& url) { return url.serialize(); });

        TRY(database.try_execute_bound_statement(statements.insert_entry, [&](auto& bind) -> ErrorOr<void> {
            TRY(bind("history_id"sv, history_id));
            TRY(bind("entry_ordinal"sv, entry_ordinal));
            TRY(bind("step"sv, entry.step));
            TRY(bind("url"sv, entry.url.serialize()));
            TRY(bind("document_state_namespace_id"sv, encode_cross_process_id_component(document_state.id.namespace_id)));
            TRY(bind("document_state_local_id"sv, encode_cross_process_id_component(document_state.id.local_id)));
            TRY(bind("classic_state"sv, entry.classic_history_api_state.data.bytes()));
            TRY(bind("navigation_state"sv, entry.navigation_api_state.data.bytes()));
            TRY(bind("navigation_api_key"sv, entry.navigation_api_key));
            TRY(bind("navigation_api_id"sv, entry.navigation_api_id));
            TRY(bind("scroll_restoration_mode"sv, encode_scroll_restoration_mode(entry.scroll_restoration_mode)));
            TRY(bind("scroll_x_raw"sv, scroll.map([](auto const& point) { return point.x().raw_value(); })));
            TRY(bind("scroll_y_raw"sv, scroll.map([](auto const& point) { return point.y().raw_value(); })));
            TRY(bind("origin_kind"sv, origin.kind));
            TRY(bind("origin_nonce"sv, nonce_column(origin.nonce)));
            TRY(bind("origin_scheme"sv, origin.scheme));
            TRY(bind("origin_host"sv, origin.host));
            TRY(bind("origin_port"sv, origin.port));
            TRY(bind("origin_domain"sv, origin.domain));
            TRY(bind("initiator_origin_kind"sv, initiator_origin.kind));
            TRY(bind("initiator_origin_nonce"sv, nonce_column(initiator_origin.nonce)));
            TRY(bind("initiator_origin_scheme"sv, initiator_origin.scheme));
            TRY(bind("initiator_origin_host"sv, initiator_origin.host));
            TRY(bind("initiator_origin_port"sv, initiator_origin.port));
            TRY(bind("initiator_origin_domain"sv, initiator_origin.domain));
            TRY(bind("referrer_kind"sv, referrer.kind));
            TRY(bind("referrer_url"sv, referrer.url));
            TRY(bind("referrer_policy"sv, encode_referrer_policy(document_state.request_referrer_policy)));
            TRY(bind("resource_kind"sv, resource.kind));
            TRY(bind("resource_string"sv, resource.string));
            TRY(bind("about_base_url"sv, about_base_url));
            TRY(bind("navigable_target_name"sv, document_state.navigable_target_name));
            TRY(bind("ever_populated"sv, document_state.ever_populated));
            TRY(bind("reload_pending"sv, document_state.reload_pending));
            return {};
        }));
        ++entry_ordinal;
    }

    entry_ordinal = 0;
    for (auto const& entry : entries) {
        auto const& document_state = entry.document_state;
        if (auto const* container = document_state.history_policy_container.get_pointer<Web::HTML::SerializedPolicyContainer>())
            TRY(store_policy_container(database, statements, history_id, entry_ordinal, *container));

        i64 nested_ordinal = 0;
        for (auto const& nested_history : document_state.nested_histories) {
            auto child_history_id = TRY(insert_row_returning_id(database, statements.insert_history, [&](auto& bind) -> ErrorOr<void> {
                return bind("tab_id"sv, tab_id);
            }));
            TRY(database.try_execute_bound_statement(statements.insert_nested_history_link, [&](auto& bind) -> ErrorOr<void> {
                TRY(bind("parent_history_id"sv, history_id));
                TRY(bind("parent_entry_ordinal"sv, entry_ordinal));
                TRY(bind("nested_ordinal"sv, nested_ordinal));
                TRY(bind("nested_history_namespace_id"sv, encode_cross_process_id_component(nested_history.id.namespace_id)));
                TRY(bind("nested_history_local_id"sv, encode_cross_process_id_component(nested_history.id.local_id)));
                TRY(bind("child_history_id"sv, child_history_id));
                return {};
            }));
            TRY(store_history_entries(database, statements, tab_id, child_history_id, nested_history.entries));
            ++nested_ordinal;
        }
        ++entry_ordinal;
    }

    return {};
}

ErrorOr<void> store_session_history_snapshot(Database::Database& database, SessionHistorySnapshotStatements const& statements, i64 tab_id, SessionHistorySnapshot const& snapshot)
{
    TRY(validate_session_history_snapshot_storable(snapshot));

    i64 step_ordinal = 0;
    for (auto step : snapshot.used_steps) {
        TRY(database.try_execute_bound_statement(statements.insert_used_step, [&](auto& bind) -> ErrorOr<void> {
            TRY(bind("tab_id"sv, tab_id));
            TRY(bind("step_ordinal"sv, step_ordinal));
            TRY(bind("step"sv, step));
            return {};
        }));
        ++step_ordinal;
    }

    auto root_history_id = TRY(insert_row_returning_id(database, statements.insert_history, [&](auto& bind) -> ErrorOr<void> {
        return bind("tab_id"sv, tab_id);
    }));
    TRY(store_history_entries(database, statements, tab_id, root_history_id, snapshot.entries));

    return {};
}

namespace {

struct EntryRow {
    i64 history_id { 0 };
    i64 entry_ordinal { 0 };
    i32 step { 0 };
    String url;
    i64 document_state_namespace_id { 0 };
    i64 document_state_local_id { 0 };
    ByteBuffer classic_state;
    ByteBuffer navigation_state;
    Utf16String navigation_api_key;
    Utf16String navigation_api_id;
    i64 scroll_restoration_mode { 0 };
    Optional<i32> scroll_x_raw;
    Optional<i32> scroll_y_raw;
    PersistedOrigin origin;
    PersistedOrigin initiator_origin;
    PersistedReferrer referrer;
    i64 referrer_policy { 0 };
    PersistedResource resource;
    Optional<String> about_base_url;
    Utf16String navigable_target_name;
    bool ever_populated { false };
    bool reload_pending { false };
};

struct PolicyContainerRow {
    i64 id { 0 };
    i64 history_id { 0 };
    i64 entry_ordinal { 0 };
    i64 referrer_policy { 0 };
    i64 embedder_policy_value { 0 };
    i64 embedder_policy_report_only_value { 0 };
    Utf16String embedder_policy_reporting_endpoint;
    Utf16String embedder_policy_report_only_reporting_endpoint;
};

struct CspPolicyRow {
    i64 id { 0 };
    i64 container_id { 0 };
    i64 policy_ordinal { 0 };
    i64 disposition { 0 };
    i64 source { 0 };
    PersistedOrigin self_origin;
    String pre_parsed_policy_string;
};

struct CspDirectiveRow {
    i64 id { 0 };
    i64 policy_id { 0 };
    i64 directive_ordinal { 0 };
    Utf16String name;
};

struct CspDirectiveValueRow {
    i64 directive_id { 0 };
    i64 value_ordinal { 0 };
    Utf16String value;
};

struct UsedStepRow {
    i64 step_ordinal { 0 };
    i32 step { 0 };
};

struct NestedHistoryLinkRow {
    i64 parent_history_id { 0 };
    size_t parent_entry_ordinal { 0 };
    size_t nested_ordinal { 0 };
    i64 nested_history_namespace_id { 0 };
    i64 nested_history_local_id { 0 };
    i64 child_history_id { 0 };
};

}

static ErrorOr<String> read_snapshot_text(Database::ResultRow& result_row, StringView column, SnapshotRowTotals& totals)
{
    auto value = TRY(result_row.read_text(column, MAX_TEXT_COLUMN_BYTES));
    TRY(add_snapshot_bytes(totals, value.bytes().size()));
    return value;
}

static ErrorOr<Optional<String>> read_optional_snapshot_text(Database::ResultRow& result_row, StringView column, SnapshotRowTotals& totals)
{
    if (TRY(result_row.is_null(column)))
        return Optional<String> {};
    return Optional<String> { TRY(read_snapshot_text(result_row, column, totals)) };
}

static ErrorOr<Utf16String> read_snapshot_utf16_text(Database::ResultRow& result_row, StringView column, SnapshotRowTotals& totals)
{
    auto value = TRY(read_snapshot_text(result_row, column, totals));
    return Utf16String::try_from_utf8(value.bytes_as_string_view());
}

static ErrorOr<Optional<Utf16String>> read_optional_snapshot_utf16_text(Database::ResultRow& result_row, StringView column, SnapshotRowTotals& totals)
{
    if (TRY(result_row.is_null(column)))
        return Optional<Utf16String> {};
    return Optional<Utf16String> { TRY(read_snapshot_utf16_text(result_row, column, totals)) };
}

static ErrorOr<ByteBuffer> read_snapshot_blob(Database::ResultRow& result_row, StringView column, size_t max_bytes, SnapshotRowTotals& totals)
{
    auto value = TRY(result_row.read_blob(column, max_bytes));
    TRY(add_snapshot_bytes(totals, value.size()));
    return value;
}

static ErrorOr<Optional<URL::Origin::OpaqueData::Nonce>> read_nonce_column(Database::ResultRow& result_row, StringView column, SnapshotRowTotals& totals)
{
    if (TRY(result_row.is_null(column)))
        return Optional<URL::Origin::OpaqueData::Nonce> {};
    auto bytes = TRY(read_snapshot_blob(result_row, column, NONCE_COLUMN_BYTES, totals));
    if (bytes.size() != NONCE_COLUMN_BYTES)
        return Error::from_string_literal("Persisted opaque origin nonce is not the nonce length");
    return URL::Origin::OpaqueData::Nonce::from_span(bytes.bytes());
}

template<typename Row, size_t N>
static ErrorOr<void> sort_by_contiguous_ordinal(Vector<Row>& rows, i64 Row::* ordinal, char const (&error_message)[N])
{
    quick_sort(rows, [ordinal](Row const& a, Row const& b) { return a.*ordinal < b.*ordinal; });
    i64 expected_ordinal = 0;
    for (auto const& row : rows) {
        if (row.*ordinal != expected_ordinal)
            return Error::from_string_literal(error_message);
        ++expected_ordinal;
    }
    return {};
}

static ErrorOr<Web::ContentSecurityPolicy::SerializedPolicy> build_csp_policy(CspPolicyRow const& row, Vector<Web::ContentSecurityPolicy::Directives::SerializedDirective> directives)
{
    auto self_origin = TRY(decode_origin(row.self_origin));
    if (!self_origin.has_value())
        return Error::from_string_literal("Persisted CSP policy has an empty self origin");

    return Web::ContentSecurityPolicy::SerializedPolicy {
        .directives = move(directives),
        .disposition = TRY(decode_csp_disposition(row.disposition)),
        .source = TRY(decode_csp_source(row.source)),
        .self_origin = self_origin.release_value(),
        .pre_parsed_policy_string = row.pre_parsed_policy_string,
    };
}

static ErrorOr<Web::HTML::SerializedPolicyContainer> build_policy_container(PolicyContainerRow const& row, Vector<Web::ContentSecurityPolicy::SerializedPolicy> csp_list)
{
    return Web::HTML::SerializedPolicyContainer {
        .csp_list = move(csp_list),
        .embedder_policy = {
            .value = TRY(decode_embedder_policy_value(row.embedder_policy_value)),
            .report_only_value = TRY(decode_embedder_policy_value(row.embedder_policy_report_only_value)),
            .reporting_endpoint = row.embedder_policy_reporting_endpoint,
            .report_only_reporting_endpoint = row.embedder_policy_report_only_reporting_endpoint,
        },
        .referrer_policy = TRY(decode_referrer_policy(row.referrer_policy)),
    };
}

static ErrorOr<HashMap<i64, HashMap<i64, Web::HTML::SerializedPolicyContainer>>> load_policy_containers(Database::Database& database, SessionHistorySnapshotStatements const& statements, i64 tab_id, SnapshotRowTotals& totals)
{
    auto container_rows = TRY(database.try_collect_bound_statement<PolicyContainerRow>(
        statements.select_policy_containers,
        MAX_SNAPSHOT_ENTRIES,
        [&](auto& bind) -> ErrorOr<void> { return bind("tab_id"sv, tab_id); },
        [&](Database::ResultRow& result_row) -> ErrorOr<PolicyContainerRow> {
            PolicyContainerRow row;
            row.id = TRY(result_row.read_integer<i64>("id"sv));
            row.history_id = TRY(result_row.read_integer<i64>("history_id"sv));
            row.entry_ordinal = TRY(result_row.read_integer<i64>("entry_ordinal"sv));
            row.referrer_policy = TRY(result_row.read_integer<i64>("referrer_policy"sv));
            row.embedder_policy_value = TRY(result_row.read_integer<i64>("embedder_policy_value"sv));
            row.embedder_policy_report_only_value = TRY(result_row.read_integer<i64>("embedder_policy_report_only_value"sv));
            row.embedder_policy_reporting_endpoint = TRY(read_snapshot_utf16_text(result_row, "embedder_policy_reporting_endpoint"sv, totals));
            row.embedder_policy_report_only_reporting_endpoint = TRY(read_snapshot_utf16_text(result_row, "embedder_policy_report_only_reporting_endpoint"sv, totals));
            return row;
        }));

    auto policy_rows = TRY(database.try_collect_bound_statement<CspPolicyRow>(
        statements.select_csp_policies,
        MAX_SNAPSHOT_CSP_POLICIES,
        [&](auto& bind) -> ErrorOr<void> { return bind("tab_id"sv, tab_id); },
        [&](Database::ResultRow& result_row) -> ErrorOr<CspPolicyRow> {
            CspPolicyRow row;
            row.id = TRY(result_row.read_integer<i64>("id"sv));
            row.container_id = TRY(result_row.read_integer<i64>("container_id"sv));
            row.policy_ordinal = TRY(result_row.read_integer<i64>("policy_ordinal"sv));
            row.disposition = TRY(result_row.read_integer<i64>("disposition"sv));
            row.source = TRY(result_row.read_integer<i64>("source"sv));
            row.self_origin = {
                .kind = TRY(result_row.read_integer<i64>("self_origin_kind"sv)),
                .nonce = TRY(read_nonce_column(result_row, "self_origin_nonce"sv, totals)),
                .scheme = TRY(read_optional_snapshot_text(result_row, "self_origin_scheme"sv, totals)),
                .host = TRY(read_optional_snapshot_text(result_row, "self_origin_host"sv, totals)),
                .port = TRY(result_row.read_optional_integer<u16>("self_origin_port"sv)),
                .domain = TRY(read_optional_snapshot_text(result_row, "self_origin_domain"sv, totals)),
            };
            row.pre_parsed_policy_string = TRY(read_snapshot_text(result_row, "pre_parsed_policy_string"sv, totals));
            return row;
        }));

    auto directive_rows = TRY(database.try_collect_bound_statement<CspDirectiveRow>(
        statements.select_csp_directives,
        MAX_SNAPSHOT_CSP_DIRECTIVES,
        [&](auto& bind) -> ErrorOr<void> { return bind("tab_id"sv, tab_id); },
        [&](Database::ResultRow& result_row) -> ErrorOr<CspDirectiveRow> {
            CspDirectiveRow row;
            row.id = TRY(result_row.read_integer<i64>("id"sv));
            row.policy_id = TRY(result_row.read_integer<i64>("policy_id"sv));
            row.directive_ordinal = TRY(result_row.read_integer<i64>("directive_ordinal"sv));
            row.name = TRY(read_snapshot_utf16_text(result_row, "name"sv, totals));
            return row;
        }));

    auto value_rows = TRY(database.try_collect_bound_statement<CspDirectiveValueRow>(
        statements.select_csp_directive_values,
        MAX_SNAPSHOT_CSP_DIRECTIVE_VALUES,
        [&](auto& bind) -> ErrorOr<void> { return bind("tab_id"sv, tab_id); },
        [&](Database::ResultRow& result_row) -> ErrorOr<CspDirectiveValueRow> {
            CspDirectiveValueRow row;
            row.directive_id = TRY(result_row.read_integer<i64>("directive_id"sv));
            row.value_ordinal = TRY(result_row.read_integer<i64>("value_ordinal"sv));
            row.value = TRY(read_snapshot_utf16_text(result_row, "value"sv, totals));
            return row;
        }));

    HashMap<i64, Vector<CspDirectiveValueRow>> values_by_directive;
    for (auto& row : value_rows)
        values_by_directive.ensure(row.directive_id).append(move(row));

    HashMap<i64, Vector<CspDirectiveRow>> directives_by_policy;
    for (auto& row : directive_rows)
        directives_by_policy.ensure(row.policy_id).append(move(row));

    HashMap<i64, Vector<CspPolicyRow>> policies_by_container;
    for (auto& row : policy_rows)
        policies_by_container.ensure(row.container_id).append(move(row));

    HashMap<i64, HashMap<i64, Web::HTML::SerializedPolicyContainer>> containers;
    for (auto const& container_row : container_rows) {
        auto policy_group = policies_by_container.take(container_row.id).value_or({});
        TRY(sort_by_contiguous_ordinal(policy_group, &CspPolicyRow::policy_ordinal, "Session history CSP policies are not contiguously ordered"));

        Vector<Web::ContentSecurityPolicy::SerializedPolicy> csp_list;
        csp_list.ensure_capacity(policy_group.size());
        for (auto const& policy_row : policy_group) {
            auto directive_group = directives_by_policy.take(policy_row.id).value_or({});
            TRY(sort_by_contiguous_ordinal(directive_group, &CspDirectiveRow::directive_ordinal, "Session history CSP directives are not contiguously ordered"));

            Vector<Web::ContentSecurityPolicy::Directives::SerializedDirective> directives;
            directives.ensure_capacity(directive_group.size());
            for (auto& directive_row : directive_group) {
                auto value_group = values_by_directive.take(directive_row.id).value_or({});
                TRY(sort_by_contiguous_ordinal(value_group, &CspDirectiveValueRow::value_ordinal, "Session history CSP directive values are not contiguously ordered"));

                Vector<Utf16String> values;
                values.ensure_capacity(value_group.size());
                for (auto& value_row : value_group)
                    values.unchecked_append(move(value_row.value));
                directives.unchecked_append({ .name = directive_row.name, .value = move(values) });
            }
            csp_list.unchecked_append(TRY(build_csp_policy(policy_row, move(directives))));
        }
        containers.ensure(container_row.history_id).set(container_row.entry_ordinal, TRY(build_policy_container(container_row, move(csp_list))));
    }
    return containers;
}

static ErrorOr<Web::HTML::SessionHistoryEntryDescriptor> build_entry(EntryRow&& row, Optional<Web::HTML::SerializedPolicyContainer> policy_container)
{
    if (row.scroll_x_raw.has_value() != row.scroll_y_raw.has_value())
        return Error::from_string_literal("Session history entry has only one scroll coordinate");

    auto url = URL::Parser::basic_parse(row.url);
    if (!url.has_value())
        return Error::from_string_literal("Session history entry has an unparseable url");

    auto scroll_restoration_mode = TRY(decode_scroll_restoration_mode(row.scroll_restoration_mode));
    auto origin = TRY(decode_origin(row.origin));
    auto initiator_origin = TRY(decode_origin(row.initiator_origin));
    auto referrer = TRY(decode_referrer(row.referrer));
    auto referrer_policy = TRY(decode_referrer_policy(row.referrer_policy));
    auto resource = TRY(decode_resource(row.resource));

    Optional<URL::URL> about_base_url;
    if (row.about_base_url.has_value()) {
        about_base_url = URL::Parser::basic_parse(*row.about_base_url);
        if (!about_base_url.has_value())
            return Error::from_string_literal("Session history entry has an unparseable about base url");
    }

    Web::HTML::SessionHistoryEntryDescriptor entry;
    entry.step = row.step;
    entry.url = url.release_value();
    entry.document_state.id = Web::HTML::CrossProcessId { .namespace_id = decode_cross_process_id_component(row.document_state_namespace_id), .local_id = decode_cross_process_id_component(row.document_state_local_id) };
    if (policy_container.has_value())
        entry.document_state.history_policy_container = policy_container.release_value();
    entry.document_state.request_referrer = referrer;
    entry.document_state.request_referrer_policy = referrer_policy;
    entry.document_state.initiator_origin = initiator_origin;
    entry.document_state.origin = origin;
    entry.document_state.about_base_url = about_base_url;
    entry.document_state.resource = resource;
    entry.document_state.reload_pending = row.reload_pending;
    entry.document_state.ever_populated = row.ever_populated;
    entry.document_state.navigable_target_name = row.navigable_target_name;
    entry.classic_history_api_state = Web::HTML::StorageSerializationRecord { move(row.classic_state) };
    entry.navigation_api_state = Web::HTML::StorageSerializationRecord { move(row.navigation_state) };
    entry.navigation_api_key = row.navigation_api_key;
    entry.navigation_api_id = row.navigation_api_id;
    entry.scroll_restoration_mode = scroll_restoration_mode;
    if (row.scroll_x_raw.has_value())
        entry.scroll_position_data.viewport_scroll_position = Web::CSSPixelPoint { Web::CSSPixels::from_raw(*row.scroll_x_raw), Web::CSSPixels::from_raw(*row.scroll_y_raw) };
    return entry;
}

namespace {

struct HistoryTreeLoadState {
    HashMap<i64, Vector<EntryRow>> entries_by_history;
    HashMap<i64, Vector<NestedHistoryLinkRow>> links_by_parent;
    HashMap<i64, HashMap<i64, Web::HTML::SerializedPolicyContainer>> containers_by_history;
    HashTable<i64> unvisited_histories;
};

}

static ErrorOr<Vector<Web::HTML::SessionHistoryEntryDescriptor>> load_history_entry_tree(HistoryTreeLoadState& state, i64 history_id, size_t depth)
{
    if (depth > MAX_NESTED_HISTORY_DEPTH)
        return Error::from_string_literal("Session history nests deeper than the supported budget");
    if (!state.unvisited_histories.remove(history_id))
        return Error::from_string_literal("Session history has an unreachable nested history");

    auto entry_rows = state.entries_by_history.take(history_id).value_or({});
    TRY(sort_by_contiguous_ordinal(entry_rows, &EntryRow::entry_ordinal, "Session history entries are not contiguously ordered"));

    auto containers = state.containers_by_history.take(history_id).value_or({});

    Vector<Web::HTML::SessionHistoryEntryDescriptor> entries;
    entries.ensure_capacity(entry_rows.size());
    for (auto& row : entry_rows) {
        auto container = containers.take(row.entry_ordinal);
        entries.unchecked_append(TRY(build_entry(move(row), move(container))));
    }
    if (!containers.is_empty())
        return Error::from_string_literal("Session history policy container references a missing entry");

    auto links = state.links_by_parent.take(history_id).value_or({});
    quick_sort(links, [](auto const& a, auto const& b) {
        if (a.parent_entry_ordinal != b.parent_entry_ordinal)
            return a.parent_entry_ordinal < b.parent_entry_ordinal;
        return a.nested_ordinal < b.nested_ordinal;
    });
    for (auto& link : links) {
        if (link.parent_entry_ordinal >= entries.size())
            return Error::from_string_literal("Session history nested history references a missing parent entry");
        auto& nested_histories = entries[link.parent_entry_ordinal].document_state.nested_histories;
        if (link.nested_ordinal != nested_histories.size())
            return Error::from_string_literal("Session history nested histories are not contiguously ordered");
        nested_histories.append({
            .id = Web::HTML::CrossProcessId { .namespace_id = decode_cross_process_id_component(link.nested_history_namespace_id), .local_id = decode_cross_process_id_component(link.nested_history_local_id) },
            .entries = TRY(load_history_entry_tree(state, link.child_history_id, depth + 1)),
        });
    }

    return entries;
}

ErrorOr<SessionHistorySnapshot> load_session_history_snapshot(Database::Database& database, SessionHistorySnapshotStatements const& statements, i64 tab_id, size_t current_used_step_index)
{
    SnapshotRowTotals totals;
    auto used_step_rows = TRY(database.try_collect_bound_statement<UsedStepRow>(
        statements.select_used_steps,
        MAX_SNAPSHOT_ENTRIES,
        [&](auto& bind) -> ErrorOr<void> { return bind("tab_id"sv, tab_id); },
        [&](Database::ResultRow& result_row) -> ErrorOr<UsedStepRow> {
            return UsedStepRow {
                .step_ordinal = TRY(result_row.read_integer<i64>("step_ordinal"sv)),
                .step = TRY(result_row.read_integer<i32>("step"sv)),
            };
        }));

    quick_sort(used_step_rows, [](auto const& a, auto const& b) { return a.step_ordinal < b.step_ordinal; });

    Vector<i32> used_steps;
    used_steps.ensure_capacity(used_step_rows.size());
    i64 expected_step_ordinal = 0;
    for (auto const& used_step_row : used_step_rows) {
        if (used_step_row.step_ordinal != expected_step_ordinal)
            return Error::from_string_literal("Session history used steps are not contiguously ordered");
        used_steps.unchecked_append(used_step_row.step);
        ++expected_step_ordinal;
    }

    auto entry_rows = TRY(database.try_collect_bound_statement<EntryRow>(
        statements.select_entries,
        MAX_SNAPSHOT_ENTRIES,
        [&](auto& bind) -> ErrorOr<void> { return bind("tab_id"sv, tab_id); },
        [&](Database::ResultRow& result_row) -> ErrorOr<EntryRow> {
            EntryRow row;
            row.history_id = TRY(result_row.read_integer<i64>("history_id"sv));
            row.entry_ordinal = TRY(result_row.read_integer<i64>("entry_ordinal"sv));
            row.step = TRY(result_row.read_integer<i32>("step"sv));
            row.url = TRY(read_snapshot_text(result_row, "url"sv, totals));
            row.document_state_namespace_id = TRY(result_row.read_integer<i64>("document_state_namespace_id"sv));
            row.document_state_local_id = TRY(result_row.read_integer<i64>("document_state_local_id"sv));
            row.classic_state = TRY(read_snapshot_blob(result_row, "classic_state"sv, MAX_STATE_RECORD_BYTES, totals));
            row.navigation_state = TRY(read_snapshot_blob(result_row, "navigation_state"sv, MAX_STATE_RECORD_BYTES, totals));
            row.navigation_api_key = TRY(read_snapshot_utf16_text(result_row, "navigation_api_key"sv, totals));
            row.navigation_api_id = TRY(read_snapshot_utf16_text(result_row, "navigation_api_id"sv, totals));
            row.scroll_restoration_mode = TRY(result_row.read_integer<i64>("scroll_restoration_mode"sv));
            row.scroll_x_raw = TRY(result_row.read_optional_integer<i32>("scroll_x_raw"sv));
            row.scroll_y_raw = TRY(result_row.read_optional_integer<i32>("scroll_y_raw"sv));
            row.origin = {
                .kind = TRY(result_row.read_integer<i64>("origin_kind"sv)),
                .nonce = TRY(read_nonce_column(result_row, "origin_nonce"sv, totals)),
                .scheme = TRY(read_optional_snapshot_text(result_row, "origin_scheme"sv, totals)),
                .host = TRY(read_optional_snapshot_text(result_row, "origin_host"sv, totals)),
                .port = TRY(result_row.read_optional_integer<u16>("origin_port"sv)),
                .domain = TRY(read_optional_snapshot_text(result_row, "origin_domain"sv, totals)),
            };
            row.initiator_origin = {
                .kind = TRY(result_row.read_integer<i64>("initiator_origin_kind"sv)),
                .nonce = TRY(read_nonce_column(result_row, "initiator_origin_nonce"sv, totals)),
                .scheme = TRY(read_optional_snapshot_text(result_row, "initiator_origin_scheme"sv, totals)),
                .host = TRY(read_optional_snapshot_text(result_row, "initiator_origin_host"sv, totals)),
                .port = TRY(result_row.read_optional_integer<u16>("initiator_origin_port"sv)),
                .domain = TRY(read_optional_snapshot_text(result_row, "initiator_origin_domain"sv, totals)),
            };
            row.referrer = {
                .kind = TRY(result_row.read_integer<i64>("referrer_kind"sv)),
                .url = TRY(read_optional_snapshot_text(result_row, "referrer_url"sv, totals)),
            };
            row.referrer_policy = TRY(result_row.read_integer<i64>("referrer_policy"sv));
            row.resource = {
                .kind = TRY(result_row.read_integer<i64>("resource_kind"sv)),
                .string = TRY(read_optional_snapshot_utf16_text(result_row, "resource_string"sv, totals)),
            };
            row.about_base_url = TRY(read_optional_snapshot_text(result_row, "about_base_url"sv, totals));
            row.navigable_target_name = TRY(read_snapshot_utf16_text(result_row, "navigable_target_name"sv, totals));
            row.ever_populated = TRY(result_row.read_bool("ever_populated"sv));
            row.reload_pending = TRY(result_row.read_bool("reload_pending"sv));
            return row;
        }));

    auto history_ids = TRY(database.try_collect_bound_statement_set<i64>(
        statements.select_histories,
        MAX_SNAPSHOT_HISTORIES,
        [&](auto& bind) -> ErrorOr<void> { return bind("tab_id"sv, tab_id); },
        [&](Database::ResultRow& result_row) -> ErrorOr<i64> {
            return result_row.read_integer<i64>("id"sv);
        }));

    // A valid tree links every history except the root exactly once.
    auto link_rows = TRY(database.try_collect_bound_statement<NestedHistoryLinkRow>(
        statements.select_nested_history_links,
        MAX_SNAPSHOT_HISTORIES - 1,
        [&](auto& bind) -> ErrorOr<void> { return bind("tab_id"sv, tab_id); },
        [&](Database::ResultRow& result_row) -> ErrorOr<NestedHistoryLinkRow> {
            NestedHistoryLinkRow row;
            row.parent_history_id = TRY(result_row.read_integer<i64>("parent_history_id"sv));
            row.parent_entry_ordinal = TRY(result_row.read_integer<size_t>("parent_entry_ordinal"sv));
            row.nested_ordinal = TRY(result_row.read_integer<size_t>("nested_ordinal"sv));
            row.nested_history_namespace_id = TRY(result_row.read_integer<i64>("nested_history_namespace_id"sv));
            row.nested_history_local_id = TRY(result_row.read_integer<i64>("nested_history_local_id"sv));
            row.child_history_id = TRY(result_row.read_integer<i64>("child_history_id"sv));
            return row;
        }));

    HistoryTreeLoadState state;
    for (auto& row : entry_rows)
        state.entries_by_history.ensure(row.history_id).append(move(row));
    state.containers_by_history = TRY(load_policy_containers(database, statements, tab_id, totals));

    HashTable<i64> child_history_ids;
    for (auto& link : link_rows) {
        if (!history_ids.contains(link.parent_history_id) || !history_ids.contains(link.child_history_id))
            return Error::from_string_literal("Session history nested history link references another tab");
        child_history_ids.set(link.child_history_id);
        state.links_by_parent.ensure(link.parent_history_id).append(move(link));
    }

    for (auto history_id : history_ids)
        state.unvisited_histories.set(history_id);

    SessionHistorySnapshot snapshot;
    if (!history_ids.is_empty()) {
        Optional<i64> root_history_id;
        for (auto history_id : history_ids) {
            if (child_history_ids.contains(history_id))
                continue;
            if (root_history_id.has_value())
                return Error::from_string_literal("Session history snapshot lacks a unique root history");
            root_history_id = history_id;
        }
        if (!root_history_id.has_value())
            return Error::from_string_literal("Session history snapshot lacks a unique root history");

        snapshot.entries = TRY(load_history_entry_tree(state, *root_history_id, 0));
        if (!state.unvisited_histories.is_empty())
            return Error::from_string_literal("Session history has an unreachable nested history");
    }

    snapshot.used_steps = move(used_steps);
    snapshot.current_used_step_index = current_used_step_index;
    if (current_used_step_index >= snapshot.used_steps.size())
        return Error::from_string_literal("Session history current step index is out of range");

    TRY(validate_snapshot_is_restorable(snapshot.entries, snapshot.used_steps, snapshot.current_used_step_index));

    return snapshot;
}

}
