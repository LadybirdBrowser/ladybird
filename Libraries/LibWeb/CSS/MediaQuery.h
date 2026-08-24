/*
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/Utf16FlyString.h>
#include <LibWeb/CSS/MediaFeatureID.h>
#include <LibWeb/CSS/Query.h>
#include <LibWeb/CSS/RustQueryHandle.h>
#include <LibWeb/ComputedValuesRustFFI.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

class MediaEnvironmentSnapshot {
public:
    explicit MediaEnvironmentSnapshot(DOM::Document const&);

    Parser::ValueParserFFI::FfiMediaEnvironment ffi_environment() const
    {
        return {
            .values = m_values.data(),
            .value_count = m_values.size(),
            .length_resolution_context = m_length_resolution_context.has_value() ? &*m_length_resolution_context : nullptr,
        };
    }

private:
    static_assert(media_feature_count == to_underlying(MediaFeatureID::Width) + 1);
    Array<Parser::ValueParserFFI::FfiMediaFeatureValue, media_feature_count> m_values {};
    Optional<ComputedValuesFFI::FfiLengthResolutionContext> m_length_resolution_context;
};

namespace Parser {

class RustQueryParser;

}

class MediaQuery : public RefCounted<MediaQuery> {
    friend class Parser::Parser;
    friend class Parser::RustQueryParser;

public:
    ~MediaQuery() = default;

    static NonnullRefPtr<MediaQuery> create_not_all();
    static NonnullRefPtr<MediaQuery> create(RustQueryHandle handle) { return adopt_ref(*new MediaQuery(move(handle))); }

    bool matches() const { return m_matches; }
    bool evaluate(DOM::Document const&);
    bool evaluate(MediaEnvironmentSnapshot const&);
    Utf16String to_string() const;
    void serialize_to(Utf16StringBuilder&) const;

    void dump(StringBuilder&, int indent_levels = 0) const;

private:
    explicit MediaQuery(RustQueryHandle handle)
        : m_rust_query_handle(move(handle))
    {
    }

    RustQueryHandle m_rust_query_handle;

    // Cached value, updated by evaluate()
    bool m_matches { false };
};

MatchResult evaluate_media_condition(RustQueryHandle const&, MediaEnvironmentSnapshot const&);
Utf16String serialize_a_media_query_list(Vector<NonnullRefPtr<MediaQuery>> const&);

}

namespace AK {

template<>
struct Formatter<Web::CSS::MediaQuery> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::MediaQuery const& media_query)
    {
        return Formatter<StringView>::format(builder, media_query.to_string().to_utf8());
    }
};

}
