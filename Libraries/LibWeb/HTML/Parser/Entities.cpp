/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringView.h>
#include <LibWeb/HTML/Parser/Entities.h>
#include <LibWeb/HTML/Parser/NamedCharacterReferences.h>

namespace Web::HTML {

bool NamedCharacterReferenceMatcher::try_consume_ascii_char(u8 c)
{
    auto child_index = named_character_reference_child_index(m_node_index);
    auto maybe_updated_index = named_character_reference_find_sibling_and_update_unique_index(child_index, c, m_pending_unique_index);
    if (!maybe_updated_index.has_value())
        return false;
    m_overconsumed_code_points++;
    m_node_index = maybe_updated_index.value();
    if (currently_matches()) {
        m_last_matched_unique_index = m_pending_unique_index;
        m_ends_with_semicolon = c == ';';
        m_overconsumed_code_points = 0;
    }
    return true;
}

}
