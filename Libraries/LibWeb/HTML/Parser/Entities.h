/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Span.h>
#include <LibWeb/HTML/Parser/NamedCharacterReferences.h>

namespace Web::HTML {

class NamedCharacterReferenceMatcher {
public:
    NamedCharacterReferenceMatcher() = default;

    // If `c` is the codepoint of a child of the current `node_index`, the `node_index`
    // is updated to that child and the function returns `true`.
    // Otherwise, the `node_index` is unchanged and the function returns false.
    bool try_consume_code_point(u32 c)
    {
        if (c > 0x7F)
            return false;
        return try_consume_ascii_char(static_cast<u8>(c));
    }

    // If `c` is the character of a child of the current `node_index`, the `node_index`
    // is updated to that child and the function returns `true`.
    // Otherwise, the `node_index` is unchanged and the function returns false.
    bool try_consume_ascii_char(u8 c);

    // Returns the code points associated with the last match, if any.
    Optional<NamedCharacterReferenceCodepoints> code_points() const { return named_character_reference_codepoints_from_unique_index(m_last_matched_unique_index); }

    bool last_match_ends_with_semicolon() const { return m_ends_with_semicolon; }

    u8 overconsumed_code_points() const { return m_overconsumed_code_points; }

private:
    enum class SearchStateTag : u8 {
        Init,
        FirstToSecondLayer,
        DafsaChildren,
    };
    union SearchState {
        NamedCharacterReferenceFirstToSecondLayerLink first_to_second_layer;
        ReadonlySpan<NamedCharacterReferenceNode> dafsa_children;
    };

    SearchStateTag m_search_state_tag { SearchStateTag::Init };
    SearchState m_search_state { { 0, 0 } };
    u16 m_last_matched_unique_index { 0 };
    u16 m_pending_unique_index { 0 };
    u8 m_overconsumed_code_points { 0 };
    bool m_ends_with_semicolon { false };
};

}
