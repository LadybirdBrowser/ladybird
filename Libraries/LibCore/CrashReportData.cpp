/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/StringView.h>
#include <LibCore/CrashReportData.h>

namespace Core::CrashReportData {

static StringView sanitize_source_location(StringView location)
{
    constexpr StringView implementation_path { __FILE__, sizeof(__FILE__) - 1 };
    constexpr auto implementation_suffix = "Libraries/LibCore/CrashReportData.cpp"sv;
    auto source_root = implementation_path.ends_with(implementation_suffix) ? implementation_path.substring_view(0, implementation_path.length() - implementation_suffix.length()) : StringView {};
    bool repository_location = false;
    if (!source_root.is_empty() && location.starts_with(source_root)) {
        location = location.substring_view(source_root.length());
        repository_location = true;
    }
    if (!repository_location) {
        for (auto prefix : { "AK/"sv, "Libraries/"sv, "Services/"sv, "Tests/"sv, "UI/"sv }) {
            if (auto prefix_offset = location.find(prefix); prefix_offset.has_value()) {
                location = location.substring_view(*prefix_offset);
                repository_location = true;
                break;
            }
        }
    }
    if (!repository_location || location.contains("../"sv) || location.contains("..\\"sv)) {
        if (auto separator = location.find_last('/'); separator.has_value())
            location = location.substring_view(*separator + 1);
        if (auto separator = location.find_last('\\'); separator.has_value())
            location = location.substring_view(*separator + 1);
    }

    return location;
}

Assertion sanitize_assertion(AK::AssertionFailureKind kind, char const* message)
{
    Assertion assertion;
    assertion.kind = kind == AK::AssertionFailureKind::Verification ? 1 : 2;
    auto append = [&](StringView text) {
        for (auto ch : text) {
            if (assertion.length == assertion.message.size()) {
                assertion.truncated = 1;
                break;
            }
            assertion.message[assertion.length++] = is_ascii_printable(ch) ? ch : '?';
        }
    };
    constexpr size_t maximum_message_length = 8192;
    size_t text_length = 0;
    while (text_length < maximum_message_length && message[text_length] != '\0')
        ++text_length;
    StringView text { message, text_length };
    auto location_separator = text.find_last(" at "sv);
    if (!location_separator.has_value()) {
        append(text);
    } else {
        append(text.substring_view(0, *location_separator));
        append(" at "sv);
        if (text.length() == maximum_message_length) {
            append("[location unavailable]"sv);
        } else {
            auto location = text.substring_view(*location_separator + 4);
            location = sanitize_source_location(location);
            append(location);
        }
    }
    if (text.length() == maximum_message_length)
        assertion.truncated = 1;

    return assertion;
}

StringView sanitize_backtrace_symbol(StringView symbol)
{
    // Demangled symbols may embed source paths; omit those names entirely.
    if (symbol.length() > 2048 || symbol.contains('/') || symbol.contains('\\'))
        return {};
    for (auto ch : symbol) {
        if (!is_ascii_printable(ch))
            return {};
    }
    return symbol;
}

void describe_backtrace_frame(ReportFrame& frame, AK::AssertionBacktraceFrame const& resolved)
{
    auto append = [&](StringView text) {
        for (auto ch : text) {
            if (frame.description_length == frame.description.size())
                break;
            frame.description[frame.description_length++] = is_ascii_printable(ch) ? ch : '?';
        }
    };
    auto append_number = [&](u32 value) {
        Array<char, 10> digits;
        size_t start = digits.size();
        do {
            digits[--start] = '0' + value % 10;
            value /= 10;
        } while (value);
        append({ digits.data() + start, digits.size() - start });
    };
    if (resolved.is_inline)
        append("(inlined) "sv);
    append(sanitize_backtrace_symbol(resolved.symbol));
    if (!resolved.filename.is_empty() && resolved.filename.length() <= 8192) {
        append(" at "sv);
        append(sanitize_source_location(resolved.filename));
        if (resolved.line) {
            append(":"sv);
            append_number(resolved.line);
            if (resolved.column) {
                append(":"sv);
                append_number(resolved.column);
            }
        }
    }
}

}
