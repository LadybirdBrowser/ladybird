/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#ifdef ENABLE_RUST

#    include <AK/Error.h>
#    include <AK/Noncopyable.h>
#    include <AK/String.h>
#    include <AK/Utf16View.h>
#    include <AK/Vector.h>
#    include <LibRegex/Export.h>
#    include <RustFFI.h>

namespace regex {

struct RustNamedCaptureGroup {
    String name;
    unsigned int index;
};

class REGEX_API CompiledRustRegex {
    AK_MAKE_NONCOPYABLE(CompiledRustRegex);

public:
    static ErrorOr<CompiledRustRegex, String> compile(StringView pattern, RustRegexFlags flags);

    ~CompiledRustRegex();
    CompiledRustRegex(CompiledRustRegex&& other);
    CompiledRustRegex& operator=(CompiledRustRegex&& other);

    /// Execute into internal capture buffer. Returns 1 on match, 0 on no match, -1 on limit exceeded.
    /// After a successful call, read results via capture_slot().
    int exec_internal(Utf16View input, size_t start_pos) const;
    /// Read a capture slot from the internal buffer (after exec_internal).
    /// Even slots are start positions, odd slots are end positions.
    /// Returns -1 for unmatched captures.
    int capture_slot(unsigned int slot) const { return m_capture_buffer[slot]; }
    /// Test for a match. Returns 1 on match, 0 on no match, -1 on limit exceeded.
    int test(Utf16View input, size_t start_pos = 0) const;
    unsigned int capture_count() const;
    /// Total number of capture groups including group 0.
    unsigned int total_groups() const;
    bool is_single_non_bmp_literal() const;

    /// Find all non-overlapping matches. Returns number of matches found.
    /// Results are written as (start, end) i32 pairs to the internal find_all buffer.
    /// Access results via find_all_match(i) after calling.
    int find_all(Utf16View input, size_t start_pos) const;
    /// Get the i-th match from find_all results. Returns (start, end).
    struct MatchPair {
        int start;
        int end;
    };
    MatchPair find_all_match(int index) const { return { m_find_all_buffer[index * 2], m_find_all_buffer[index * 2 + 1] }; }

    Vector<RustNamedCaptureGroup> const& named_groups() const { return m_named_groups; }

private:
    explicit CompiledRustRegex(RustRegex* regex);

    RustRegex* m_regex { nullptr };
    Vector<RustNamedCaptureGroup> m_named_groups;
    /// Pre-allocated buffer for capture results to avoid per-exec allocation.
    mutable Vector<int> m_capture_buffer;
    mutable unsigned int m_capture_count { 0 };
    mutable bool m_capture_count_cached { false };
    /// Buffer for find_all results.
    mutable Vector<int> m_find_all_buffer;
};

} // namespace regex

#endif // ENABLE_RUST
