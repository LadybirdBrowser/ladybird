/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <LibRegex/ECMAScriptRegex.h>
#include <LibRegex/RustRegex.h>

namespace regex {

struct ECMAScriptRegex::Impl {
    CompiledRustRegex rust_regex;
    Vector<ECMAScriptNamedCaptureGroup> named_groups;
};

ErrorOr<ECMAScriptRegex, String> ECMAScriptRegex::compile(StringView utf8_pattern, ECMAScriptCompileFlags flags)
{
    RustRegexFlags rust_flags {};
    rust_flags.global = flags.global;
    rust_flags.ignore_case = flags.ignore_case;
    rust_flags.multiline = flags.multiline;
    rust_flags.dot_all = flags.dot_all;
    rust_flags.unicode = flags.unicode;
    rust_flags.unicode_sets = flags.unicode_sets;
    rust_flags.sticky = flags.sticky;
    rust_flags.has_indices = flags.has_indices;

    auto compiled = CompiledRustRegex::compile(utf8_pattern, rust_flags);
    if (compiled.is_error())
        return compiled.release_error();

    auto rust_regex = compiled.release_value();

    Vector<ECMAScriptNamedCaptureGroup> named_groups;
    named_groups.ensure_capacity(rust_regex.named_groups().size());
    for (auto const& rg : rust_regex.named_groups())
        named_groups.unchecked_append({ .name = rg.name, .index = rg.index });

    auto impl = adopt_own(*new Impl {
        .rust_regex = move(rust_regex),
        .named_groups = move(named_groups),
    });
    return ECMAScriptRegex(move(impl));
}

ECMAScriptRegex::~ECMAScriptRegex() = default;

ECMAScriptRegex::ECMAScriptRegex(ECMAScriptRegex&& other) = default;
ECMAScriptRegex& ECMAScriptRegex::operator=(ECMAScriptRegex&& other) = default;

ECMAScriptRegex::ECMAScriptRegex(OwnPtr<Impl> impl)
    : m_impl(move(impl))
{
}

MatchResult ECMAScriptRegex::exec(Utf16View input, size_t start_pos) const
{
    auto result = m_impl->rust_regex.exec_internal(input, start_pos);
    if (result == 1)
        return MatchResult::Match;
    if (result == -1)
        return MatchResult::LimitExceeded;
    return MatchResult::NoMatch;
}

int ECMAScriptRegex::capture_slot(unsigned int slot) const
{
    return m_impl->rust_regex.capture_slot(slot);
}

MatchResult ECMAScriptRegex::test(Utf16View input, size_t start_pos) const
{
    auto result = m_impl->rust_regex.test(input, start_pos);
    if (result == 1)
        return MatchResult::Match;
    if (result == -1)
        return MatchResult::LimitExceeded;
    return MatchResult::NoMatch;
}

unsigned int ECMAScriptRegex::capture_count() const
{
    return m_impl->rust_regex.capture_count();
}

unsigned int ECMAScriptRegex::total_groups() const
{
    return m_impl->rust_regex.total_groups();
}

bool ECMAScriptRegex::is_single_non_bmp_literal() const
{
    return m_impl->rust_regex.is_single_non_bmp_literal();
}

Vector<ECMAScriptNamedCaptureGroup> const& ECMAScriptRegex::named_groups() const
{
    return m_impl->named_groups;
}

int ECMAScriptRegex::find_all(Utf16View input, size_t start_pos) const
{
    return m_impl->rust_regex.find_all(input, start_pos);
}

ECMAScriptRegex::MatchPair ECMAScriptRegex::find_all_match(int index) const
{
    auto pair = m_impl->rust_regex.find_all_match(index);
    return { pair.start, pair.end };
}

}
