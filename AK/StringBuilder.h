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

template<typename T>
ErrorOr<T> string_builder_to(StringBuilder&& builder) = delete;

template<>
ErrorOr<String> string_builder_to<String>(StringBuilder&& builder);

template<>
ErrorOr<FlyString> string_builder_to<FlyString>(StringBuilder&& builder);

template<>
ErrorOr<ByteString> string_builder_to<ByteString>(StringBuilder&& builder);

template<>
ErrorOr<ByteBuffer> string_builder_to<ByteBuffer>(StringBuilder&& builder);

class StringBuilder {
public:
    static constexpr size_t inline_capacity = 256;

    using Buffer = Detail::ByteBuffer<inline_capacity>;
    using OutputType = ByteString;

    static ErrorOr<StringBuilder> create(size_t initial_capacity = inline_capacity);

    explicit StringBuilder(size_t initial_capacity = inline_capacity);
    ~StringBuilder() = default;

    ErrorOr<void> will_append(size_t);

    ErrorOr<void> try_append(StringView);
    ErrorOr<void> try_append(Wtf16ByteView const&);
    ErrorOr<void> try_append(Utf32View const&);
    ErrorOr<void> try_append(char);
    ErrorOr<void> try_append(UnicodeCodePoint);
    ErrorOr<void> try_append_code_point(u32);
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
    ErrorOr<void> try_append(IterableContainerOf<UnicodeCodePoint> auto const& view)
    {
        if constexpr (requires { { view.length_without_side_effects() } -> ConvertibleTo<Optional<size_t>>; }) {
            if (auto length = view.length_without_side_effects(); length.has_value())
                TRY(will_append(length.value()));
        }
        for (auto cp : view)
            TRY(try_append_code_point(cp));
        return {};
    }

    void append(StringView);
    void append(Wtf16ByteView const&);
    void append(Utf32View const&);
    void append(char);
    void append(UnicodeCodePoint);
    void append_code_point(u32);
    void append(char const*, size_t);
    void appendvf(char const*, va_list);
    void append_repeated(char, size_t);
    void append_repeated(StringView, size_t);
    void append(IterableContainerOf<UnicodeCodePoint> auto const& view)
    {
        MUST(try_append(view));
    }

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

    template<typename T>
    ErrorOr<T> to() &&
    {
        return string_builder_to<T>();
    }

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

    u8* data();
    u8 const* data() const;

    Buffer m_buffer;
};

}

#if USING_AK_GLOBALLY
using AK::StringBuilder;
#endif
