/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibRegex/RustRegex.h>

namespace regex {

ErrorOr<CompiledRustRegex, String> CompiledRustRegex::compile(StringView pattern, RustRegexFlags flags)
{
    unsigned char const* error_ptr = nullptr;
    size_t error_len = 0;

    auto* regex = rust_regex_compile(
        reinterpret_cast<unsigned char const*>(pattern.characters_without_null_termination()),
        pattern.length(),
        flags,
        &error_ptr,
        &error_len);
    if (!regex) {
        String error_message = "Invalid pattern"_string;
        if (error_ptr) {
            error_message = MUST(String::from_utf8({ reinterpret_cast<char const*>(error_ptr), error_len }));
            rust_regex_free_error(const_cast<unsigned char*>(error_ptr), error_len);
        }
        return error_message;
    }

    CompiledRustRegex result(regex);

    unsigned int group_count = 0;
    auto* groups = rust_regex_get_named_groups(regex, &group_count);
    if (groups) {
        result.m_named_groups.ensure_capacity(group_count);
        for (unsigned int i = 0; i < group_count; ++i) {
            auto name = String::from_utf8({ reinterpret_cast<char const*>(groups[i].name), groups[i].name_len });
            result.m_named_groups.append(RustNamedCaptureGroup { MUST(name), groups[i].index });
        }
        rust_regex_free_named_groups(groups, group_count);
    }

    return result;
}

CompiledRustRegex::~CompiledRustRegex()
{
    if (m_regex)
        rust_regex_free(m_regex);
}

CompiledRustRegex::CompiledRustRegex(CompiledRustRegex&& other)
    : m_regex(other.m_regex)
    , m_named_groups(move(other.m_named_groups))
    , m_capture_buffer(move(other.m_capture_buffer))
    , m_capture_count(other.m_capture_count)
    , m_capture_count_cached(other.m_capture_count_cached)
    , m_find_all_buffer(move(other.m_find_all_buffer))
{
    other.m_regex = nullptr;
    other.m_capture_count = 0;
    other.m_capture_count_cached = false;
}

CompiledRustRegex& CompiledRustRegex::operator=(CompiledRustRegex&& other)
{
    if (this != &other) {
        if (m_regex)
            rust_regex_free(m_regex);
        m_regex = other.m_regex;
        m_named_groups = move(other.m_named_groups);
        m_capture_buffer = move(other.m_capture_buffer);
        m_capture_count = other.m_capture_count;
        m_capture_count_cached = other.m_capture_count_cached;
        m_find_all_buffer = move(other.m_find_all_buffer);
        other.m_regex = nullptr;
        other.m_capture_count = 0;
        other.m_capture_count_cached = false;
    }
    return *this;
}

CompiledRustRegex::CompiledRustRegex(RustRegex* regex)
    : m_regex(regex)
{
}

int CompiledRustRegex::exec_internal(Utf16View input, size_t start_pos) const
{
    if (!m_capture_count_cached) {
        m_capture_count = rust_regex_capture_count(m_regex) + 1;
        m_capture_count_cached = true;
    }
    auto slots = m_capture_count * 2;
    m_capture_buffer.resize(slots);

    if (input.has_ascii_storage()) {
        auto ascii = input.ascii_span();
        return rust_regex_exec_into_ascii(
            m_regex,
            reinterpret_cast<uint8_t const*>(ascii.data()),
            ascii.size(),
            start_pos,
            m_capture_buffer.data(),
            slots);
    }

    auto utf16 = input.utf16_span();
    return rust_regex_exec_into(
        m_regex,
        reinterpret_cast<unsigned short const*>(utf16.data()),
        utf16.size(),
        start_pos,
        m_capture_buffer.data(),
        slots);
}

unsigned int CompiledRustRegex::total_groups() const
{
    if (!m_capture_count_cached) {
        m_capture_count = rust_regex_capture_count(m_regex) + 1;
        m_capture_count_cached = true;
    }
    return m_capture_count;
}

bool CompiledRustRegex::is_single_non_bmp_literal() const
{
    return rust_regex_is_single_non_bmp_literal(m_regex);
}

int CompiledRustRegex::test(Utf16View input, size_t start_pos) const
{
    if (input.has_ascii_storage()) {
        auto ascii = input.ascii_span();
        return rust_regex_test_ascii(
            m_regex,
            reinterpret_cast<uint8_t const*>(ascii.data()),
            ascii.size(),
            start_pos);
    }

    auto utf16 = input.utf16_span();
    return rust_regex_test(
        m_regex,
        reinterpret_cast<unsigned short const*>(utf16.data()),
        utf16.size(),
        start_pos);
}

int CompiledRustRegex::find_all(Utf16View input, size_t start_pos) const
{
    // Start with reasonable capacity; keep doubling until it fits.
    if (m_find_all_buffer.size() < 256)
        m_find_all_buffer.resize(256);

    for (;;) {
        int result;
        if (input.has_ascii_storage()) {
            auto ascii = input.ascii_span();
            result = rust_regex_find_all_ascii(
                m_regex,
                reinterpret_cast<uint8_t const*>(ascii.data()),
                ascii.size(),
                start_pos,
                m_find_all_buffer.data(),
                m_find_all_buffer.size());
        } else {
            auto utf16 = input.utf16_span();
            result = rust_regex_find_all(
                m_regex,
                reinterpret_cast<unsigned short const*>(utf16.data()),
                utf16.size(),
                start_pos,
                m_find_all_buffer.data(),
                m_find_all_buffer.size());
        }
        if (result != -1)
            return result;
        m_find_all_buffer.resize(m_find_all_buffer.size() * 2);
    }
}

unsigned int CompiledRustRegex::capture_count() const
{
    return rust_regex_capture_count(m_regex);
}

} // namespace regex
