/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Format.h>
#include <AK/Forward.h>
#include <AK/StringView.h>
#include <stdarg.h>

namespace AK {

class StringBuilder {
public:
    static constexpr size_t inline_capacity = 256;

    using Buffer = Detail::ByteBuffer<inline_capacity>;

    static ErrorOr<StringBuilder> create(size_t initial_capacity = inline_capacity);

    StringBuilder();
    explicit StringBuilder(size_t initial_capacity);
    ~StringBuilder() = default;

    ErrorOr<void> try_append(StringView);
    ErrorOr<void> try_append(Utf16View const&);
    ErrorOr<void> try_append(Utf32View const&);
    ErrorOr<void> try_append_code_point(u32);
    ErrorOr<void> try_append(char);
    template<typename... Parameters>
    ErrorOr<void> try_appendff(CheckedFormatString<Parameters...>&& fmtstr, Parameters const&... parameters)
    {
        VariadicFormatParams<AllowDebugOnlyFormatters::No, Parameters...> variadic_format_params { parameters... };
        return vformat(*this, fmtstr.view(), variadic_format_params);
    }
    ErrorOr<void> try_append(char const*, size_t);
    ErrorOr<void> try_append_repeated(char, size_t);
    ErrorOr<void> try_append_repeated(StringView, size_t);
    ErrorOr<void> try_append_escaped_for_json(StringView);

    void append(StringView);
    void append(Utf16View const&);
    void append(Utf32View const&);
    void append(char);
    void append_code_point(u32);
    void append(char const*, size_t);
    void appendvf(char const*, va_list);
    void append_repeated(char, size_t);
    void append_repeated(StringView, size_t);

    void append_as_lowercase(char);
    void append_escaped_for_json(StringView);

    template<typename... Parameters>
    void appendff(CheckedFormatString<Parameters...>&& fmtstr, Parameters const&... parameters)
    {
        VariadicFormatParams<AllowDebugOnlyFormatters::No, Parameters...> variadic_format_params { parameters... };
        MUST(vformat(*this, fmtstr.view(), variadic_format_params));
    }

    [[nodiscard]] ByteString to_byte_string() const;

    [[nodiscard]] String to_string_without_validation();
    ErrorOr<String> to_string();

    [[nodiscard]] FlyString to_fly_string_without_validation() const;
    ErrorOr<FlyString> to_fly_string() const;

    [[nodiscard]] ErrorOr<ByteBuffer> to_byte_buffer() const;

    [[nodiscard]] StringView string_view() const;
    void clear();

    [[nodiscard]] size_t length() const;
    [[nodiscard]] bool is_empty() const;
    void trim(size_t count);

    template<class SeparatorType, class CollectionType>
    void join(SeparatorType const& separator, CollectionType const& collection, StringView fmtstr = "{}"sv)
    {
        MUST(try_join(separator, collection, fmtstr));
    }

    template<class SeparatorType, class CollectionType>
    ErrorOr<void> try_join(SeparatorType const& separator, CollectionType const& collection, StringView fmtstr = "{}"sv)
    {
        bool first = true;
        for (auto& item : collection) {
            if (!first)
                TRY(try_append(separator));
            TRY(try_appendff(fmtstr, item));
            first = false;
        }
        return {};
    }

    Optional<Buffer::OutlineBuffer> leak_buffer_for_string_construction(Badge<Detail::StringData>);

private:
    explicit StringBuilder(Buffer);

    ErrorOr<void> will_append(size_t);
    u8* data();
    u8 const* data() const;

    Buffer m_buffer;
};

}

#if USING_AK_GLOBALLY
using AK::StringBuilder;
#endif
