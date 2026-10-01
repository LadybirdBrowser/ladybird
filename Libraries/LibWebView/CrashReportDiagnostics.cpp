/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AllOf.h>
#include <AK/CharacterTypes.h>
#include <AK/StringBuilder.h>
#include <LibWebView/CrashReportDiagnostics.h>

namespace WebView {

static constexpr auto stack_heading = "Native stack (binary build ID, object address):"sv;
static constexpr auto partial_stack_note = "Stacks may be partial."sv;

// The time of the crash, written as "2026-10-01T12:34:56Z".
static Optional<UnixDateTime> parse_crash_time(StringView text)
{
    constexpr auto pattern = "####-##-##T##:##:##Z"sv;
    if (text.length() != pattern.length())
        return {};
    for (size_t i = 0; i < pattern.length(); ++i) {
        if (pattern[i] == '#' ? !is_ascii_digit(text[i]) : text[i] != pattern[i])
            return {};
    }

    auto part = [&](size_t start, size_t length) { return text.substring_view(start, length).to_number<u32>().value_or(0); };
    auto year = part(0, 4);
    auto month = part(5, 2);
    auto day = part(8, 2);
    auto hour = part(11, 2);
    auto minute = part(14, 2);
    auto second = part(17, 2);
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59)
        return {};
    return UnixDateTime::from_unix_time_parts(year, month, day, hour, minute, second, 0);
}

// The review screen and submission manifest use the same parse, so submitted diagnostics are
// identical to those the user reviewed.
CrashReportDiagnostics CrashReportDiagnostics::parse(StringView text)
{
    CrashReportDiagnostics diagnostics;

    auto stack_start = text.find(ByteString::formatted("{}\n", stack_heading));
    auto header_section = stack_start.has_value() ? text.substring_view(0, *stack_start) : text;

    // The first line names the format rather than a field.
    auto lines = header_section.split_view('\n', SplitBehavior::KeepEmpty);
    for (size_t i = 1; i < lines.size(); ++i) {
        auto line = lines[i];

        // The first colon separates the two, so a name never contains one but a value may.
        if (auto separator = line.find(':'); separator.has_value() && *separator > 0) {
            auto name = line.substring_view(0, *separator);
            auto value = line.substring_view(*separator + 1).trim_whitespace();
            diagnostics.headers.append({ name, value });
            continue;
        }

        // A value that wrapped onto its own line belongs to the field above it.
        if (!line.trim_whitespace().is_empty() && !diagnostics.headers.is_empty()) {
            auto& previous = diagnostics.headers.last();
            previous.value = ByteString::formatted("{}\n{}", previous.value, line);
        }
    }

    diagnostics.crashed_at = parse_crash_time(diagnostics.header("Crashed at"sv));

    for (auto prefix : { "Termination signal"sv, "Captured signal"sv }) {
        auto name = diagnostics.header(prefix);
        if (name.is_empty())
            continue;
        auto number = diagnostics.header(ByteString::formatted("{} number", prefix));
        diagnostics.signal = { move(name), number.view().to_number<u64>() };
        break;
    }

    if (!stack_start.has_value())
        return diagnostics;

    StringBuilder stack;
    for (auto line : text.substring_view(*stack_start).split_view('\n', SplitBehavior::KeepEmpty)) {
        if (line.trim_whitespace() == partial_stack_note)
            continue;
        stack.appendff("{}\n", line);
    }
    diagnostics.stack = stack.string_view().trim_whitespace();

    return diagnostics;
}

ByteString CrashReportDiagnostics::header(StringView name) const
{
    for (auto const& entry : headers) {
        if (entry.name == name)
            return entry.value;
    }
    return {};
}

}
