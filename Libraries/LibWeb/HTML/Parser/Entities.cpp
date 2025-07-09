/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/BuiltinWrappers.h>
#include <AK/CharacterTypes.h>
#include <LibWeb/HTML/Parser/Entities.h>
#include <LibWeb/HTML/Parser/NamedCharacterReferences.h>

namespace Web::HTML {

static u8 ascii_alphabetic_to_index(u8 c)
{
    ASSERT(AK::is_ascii_alpha(c));
    return c <= 'Z' ? (c - 'A') : (c - 'a' + 26);
}

bool NamedCharacterReferenceMatcher::try_consume_ascii_char(u8 c)
{
    switch (m_search_state_tag) {
    case NamedCharacterReferenceMatcher::SearchStateTag::Init: {
        if (!AK::is_ascii_alpha(c))
            return false;
        auto index = ascii_alphabetic_to_index(c);
        m_search_state_tag = NamedCharacterReferenceMatcher::SearchStateTag::FirstToSecondLayer;
        m_search_state = { .first_to_second_layer = g_named_character_reference_first_to_second_layer[index] };
        m_pending_unique_index = g_named_character_reference_first_layer[index].number;
        m_overconsumed_code_points++;
        return true;
    }
    case NamedCharacterReferenceMatcher::SearchStateTag::FirstToSecondLayer: {
        if (!AK::is_ascii_alpha(c))
            return false;
        auto bit_index = ascii_alphabetic_to_index(c);
        if (((1ull << bit_index) & m_search_state.first_to_second_layer.mask) == 0)
            return false;

        // Get the second layer node by re-using the first_to_second_layer.mask.
        // For example, if the first character is 'n' and the second character is 'o':
        //
        // This is the first_to_second_layer.mask when the first character is 'n':
        // 0001111110110110111111111100001000100000100001000000
        //            └ bit_index of 'o'
        //
        // Create a mask where all of the less significant bits than the
        // bit index of the current character ('o') are set:
        // 0000000000001111111111111111111111111111111111111111
        //            └ bit_index of 'o'
        //
        // Bitwise AND this new mask with the first_to_second_layer.mask
        // to get only the set bits less significant than the bit index of the
        // current character:
        // 0000000000000110111111111100001000100000100001000000
        //
        // Take the popcount of this to get the index of the node within the
        // second layer. In this case, there are 16 bits set, so the index
        // of 'o' in the second layer is first_to_second_layer.second_layer_offset + 16.
        u64 mask = (1ull << bit_index) - 1;
        u8 char_index = AK::popcount(m_search_state.first_to_second_layer.mask & mask);
        auto const& node = g_named_character_reference_second_layer[m_search_state.first_to_second_layer.second_layer_offset + char_index];

        m_pending_unique_index += node.number;
        m_overconsumed_code_points++;
        if (node.end_of_word) {
            m_pending_unique_index++;
            m_last_matched_unique_index = m_pending_unique_index;
            m_ends_with_semicolon = c == ';';
            m_overconsumed_code_points = 0;
        }
        m_search_state_tag = NamedCharacterReferenceMatcher::SearchStateTag::DafsaChildren;
        m_search_state = { .dafsa_children = { &g_named_character_reference_nodes[node.child_index], node.children_len } };
        return true;
    }
    case NamedCharacterReferenceMatcher::SearchStateTag::DafsaChildren: {
        for (auto const& node : m_search_state.dafsa_children) {
            if (node.character == c) {
                m_pending_unique_index += node.number;
                m_overconsumed_code_points++;
                if (node.end_of_word) {
                    m_pending_unique_index++;
                    m_last_matched_unique_index = m_pending_unique_index;
                    m_ends_with_semicolon = c == ';';
                    m_overconsumed_code_points = 0;
                }
                m_search_state = { .dafsa_children = { &g_named_character_reference_nodes[node.child_index], node.children_len } };
                return true;
            }
        }
        return false;
    }
    default:
        VERIFY_NOT_REACHED();
    }
}

}
