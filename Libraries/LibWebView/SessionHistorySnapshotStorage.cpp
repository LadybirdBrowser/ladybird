/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Parser.h>
#include <LibURL/URL.h>
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

}
