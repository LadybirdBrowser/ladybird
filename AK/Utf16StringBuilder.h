/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/Format.h>
#include <AK/Forward.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/StringView.h>
#include <AK/Types.h>
#include <AK/Utf16View.h>

namespace AK {

class Utf16StringBuilder {
public:
    static constexpr size_t inline_capacity = 256;

    class Buffer {
    public:
        Buffer() = default;
        ~Buffer();

        Buffer(Buffer const&) = delete;
        Buffer(Buffer&&) = delete;
        Buffer& operator=(Buffer const&) = delete;
        Buffer& operator=(Buffer&&) = delete;

        [[nodiscard]] u8* data();
        [[nodiscard]] u8 const* data() const;

        [[nodiscard]] Bytes span() LIFETIME_BOUND;
        [[nodiscard]] ReadonlyBytes span() const LIFETIME_BOUND;

        [[nodiscard]] size_t size() const { return m_size; }
        [[nodiscard]] size_t capacity() const { return m_inline ? inline_capacity : m_outline_capacity; }
        [[nodiscard]] bool is_inline() const { return m_inline; }

        void clear();
        void resize(size_t);
        void set_size(size_t);
        void ensure_capacity(size_t);
        void append(void const*, size_t);

        struct OutlineBuffer {
            Bytes buffer;
            size_t capacity { 0 };
        };
        Optional<OutlineBuffer> leak_outline_buffer();

    private:
        void trim(size_t, bool may_discard_existing_data);
        void shrink_into_inline_buffer(size_t, bool may_discard_existing_data);
        void ensure_capacity_slowpath(size_t);

        union {
            u8 m_inline_buffer[inline_capacity];
            struct {
                u8* m_outline_buffer;
                size_t m_outline_capacity;
            };
        };
        size_t m_size { 0 };
        bool m_inline { true };
    };

    Utf16StringBuilder();
    explicit Utf16StringBuilder(size_t initial_capacity_in_code_units);
    ~Utf16StringBuilder() = default;

    Utf16StringBuilder(Utf16StringBuilder const&) = delete;
    Utf16StringBuilder(Utf16StringBuilder&&) = delete;
    Utf16StringBuilder& operator=(Utf16StringBuilder const&) = delete;
    Utf16StringBuilder& operator=(Utf16StringBuilder&&) = delete;

    void append(Utf16View const&);
    void append_ascii(char);
    void append_ascii(StringView);
    void append_code_unit(char16_t);
    void append_code_point(u32);
    void append_repeated_ascii(char, size_t);
    void append_repeated(Utf16View const&, size_t);

    template<typename... Parameters>
    ErrorOr<void> try_appendff(CheckedFormatString<Parameters...>&& fmtstr, Parameters const&... parameters)
    {
        VariadicFormatParams<AllowDebugOnlyFormatters::No, Parameters...> variadic_format_parameters { parameters... };
        return vformat(*this, fmtstr.view(), variadic_format_parameters);
    }

    template<typename... Parameters>
    void appendff(CheckedFormatString<Parameters...>&& fmtstr, Parameters const&... parameters)
    {
        VariadicFormatParams<AllowDebugOnlyFormatters::No, Parameters...> variadic_format_parameters { parameters... };
        MUST(vformat(*this, fmtstr.view(), variadic_format_parameters));
    }

    [[nodiscard]] Utf16String to_string();

    [[nodiscard]] Utf16View view() const;
    [[nodiscard]] size_t length_in_code_units() const;
    [[nodiscard]] bool is_empty() const { return length_in_code_units() == 0; }
    void trim(size_t count_in_code_units);
    void clear();

    Optional<Buffer::OutlineBuffer> leak_buffer_for_string_construction(Badge<Detail::Utf16StringData>)
    {
        return leak_buffer_for_string_construction();
    }

private:
    void initialize_buffer(size_t capacity_in_code_units);
    void will_append(size_t size_in_bytes);
    void ensure_storage_is_utf16();

    Optional<Buffer::OutlineBuffer> leak_buffer_for_string_construction();

    [[nodiscard]] u8* data();
    [[nodiscard]] u8 const* data() const;
    [[nodiscard]] size_t length_in_bytes() const;

    Buffer m_buffer;
    bool m_is_ascii { true };
};

}

#if USING_AK_GLOBALLY
using AK::Utf16StringBuilder;
#endif
