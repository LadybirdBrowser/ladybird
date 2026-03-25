/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Noncopyable.h>
#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <AK/Utf16View.h>
#include <AK/Vector.h>
#include <LibRegex/Export.h>

namespace regex {

enum class MatchResult : i8 {
    Match,
    NoMatch,
    LimitExceeded,
};

struct ECMAScriptCompileFlags {
    bool global {};
    bool ignore_case {};
    bool multiline {};
    bool dot_all {};
    bool unicode {};
    bool unicode_sets {};
    bool sticky {};
    bool has_indices {};
};

struct ECMAScriptNamedCaptureGroup {
    String name;
    unsigned int index;
};

class REGEX_API ECMAScriptRegex {
    AK_MAKE_NONCOPYABLE(ECMAScriptRegex);

public:
    static ErrorOr<ECMAScriptRegex, String> compile(StringView utf8_pattern, ECMAScriptCompileFlags);

    ~ECMAScriptRegex();
    ECMAScriptRegex(ECMAScriptRegex&&);
    ECMAScriptRegex& operator=(ECMAScriptRegex&&);

    /// Execute and fill internal capture buffer.
    /// After a successful call, read results via capture_slot().
    [[nodiscard]] MatchResult exec(Utf16View input, size_t start_pos) const;

    /// Read a capture slot from the internal buffer (after exec).
    /// Even slots are start positions, odd slots are end positions.
    /// Returns -1 for unmatched captures.
    int capture_slot(unsigned int slot) const;

    /// Test for a match without filling capture buffer.
    [[nodiscard]] MatchResult test(Utf16View input, size_t start_pos) const;

    /// Number of numbered capture groups (excluding group 0).
    unsigned int capture_count() const;

    /// Total number of capture groups including group 0.
    unsigned int total_groups() const;

    bool is_single_non_bmp_literal() const;

    /// Named capture groups with their indices.
    Vector<ECMAScriptNamedCaptureGroup> const& named_groups() const;

    /// Find all non-overlapping matches. Returns number of matches found.
    /// Access results via find_all_match(i) after calling.
    int find_all(Utf16View input, size_t start_pos) const;

    struct MatchPair {
        int start;
        int end;
    };

    /// Get the i-th match from find_all results.
    MatchPair find_all_match(int index) const;

private:
    struct Impl;
    ECMAScriptRegex(OwnPtr<Impl>);
    OwnPtr<Impl> m_impl;
};

} // namespace regex
