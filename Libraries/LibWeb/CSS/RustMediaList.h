/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <LibWeb/CSS/MediaFeatureID.h>
#include <LibWeb/ComputedValuesRustFFI.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/ValueParserRustFFI.h>

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

class RustMediaList {
    AK_MAKE_NONCOPYABLE(RustMediaList);

public:
    enum class Ownership {
        Adopt,
        Borrow,
    };

    RustMediaList()
        : RustMediaList(Parser::ValueParserFFI::rust_media_list_create())
    {
    }
    explicit RustMediaList(Parser::ValueParserFFI::MediaList const* list, Ownership ownership = Ownership::Adopt)
        : m_list(list)
        , m_ownership(ownership)
    {
        VERIFY(list);
    }
    RustMediaList(RustMediaList&& other)
        : m_list(exchange(other.m_list, nullptr))
        , m_ownership(exchange(other.m_ownership, Ownership::Adopt))
    {
    }
    RustMediaList& operator=(RustMediaList&& other)
    {
        RustMediaList moved(move(other));
        swap(m_list, moved.m_list);
        swap(m_ownership, moved.m_ownership);
        return *this;
    }
    ~RustMediaList()
    {
        if (m_ownership == Ownership::Adopt)
            Parser::ValueParserFFI::rust_media_list_free(const_cast<Parser::ValueParserFFI::MediaList*>(m_list));
    }
    // https://drafts.csswg.org/mediaqueries-4/#parse-a-media-query-list
    static RustMediaList parse(Utf16View text)
    {
        RustMediaList list;
        list.set_text(text);
        return list;
    }

    RustMediaList retain() const { return RustMediaList { Parser::ValueParserFFI::rust_media_list_retain(m_list) }; }
    RustMediaList share() const { return RustMediaList { Parser::ValueParserFFI::rust_media_list_share(m_list) }; }
    size_t length() const { return Parser::ValueParserFFI::rust_media_list_length(m_list); }
    bool matches() const { return Parser::ValueParserFFI::rust_media_list_matches(m_list); }
    bool evaluate(DOM::Document const& document) const
    {
        MediaEnvironmentSnapshot environment { document };
        return Parser::ValueParserFFI::rust_media_list_evaluate(m_list, environment.ffi_environment());
    }
    void set_text(Utf16View text) const { Parser::ValueParserFFI::rust_media_list_set_text(m_list, view(text)); }
    bool append(Utf16View text) const { return Parser::ValueParserFFI::rust_media_list_append(m_list, view(text)); }
    Parser::ValueParserFFI::MediaListDeleteResult remove(Utf16View text) const { return Parser::ValueParserFFI::rust_media_list_delete(m_list, view(text)); }
    Utf16String media_text() const
    {
        Utf16String result;
        Parser::ValueParserFFI::rust_media_list_serialize(m_list, &result, copy_text);
        return result;
    }
    Optional<Utf16String> item(size_t index) const
    {
        Utf16String result;
        if (!Parser::ValueParserFFI::rust_media_list_item(m_list, index, &result, copy_text))
            return {};
        return result;
    }
    WEB_API void dump(StringBuilder&, int indent_levels = 0) const;
    Parser::ValueParserFFI::MediaList const* handle() const { return m_list; }

private:
    static Parser::ValueParserFFI::FfiUtf16View view(Utf16View text)
    {
        return { text.has_ascii_storage() ? reinterpret_cast<u8 const*>(text.ascii_span().data()) : nullptr,
            text.has_ascii_storage() ? nullptr : reinterpret_cast<u16 const*>(text.utf16_span().data()), text.length_in_code_units() };
    }
    static void copy_text(void* context, u16 const* data, size_t length)
    {
        *static_cast<Utf16String*>(context) = Utf16String::from_utf16({ reinterpret_cast<char16_t const*>(data), length });
    }
    Parser::ValueParserFFI::MediaList const* m_list;
    Ownership m_ownership { Ownership::Adopt };
};

}
