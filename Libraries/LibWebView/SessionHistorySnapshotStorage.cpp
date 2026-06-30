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

}
