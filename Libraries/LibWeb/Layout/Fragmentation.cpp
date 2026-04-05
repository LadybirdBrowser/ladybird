/*
 * Copyright (c) 2026-present, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/Fragmentation.h>

namespace Web::Layout {

FragmentationContext::FragmentationContext(GC::Ref<Box const> root_box)
    : m_root_box(root_box)
{
}

FragmentationContext::~FragmentationContext() = default;

ColumnFragmentationContext::ColumnFragmentationContext(GC::Ref<Box const> root_box, int column_count, CSSPixels column_width, CSSPixels column_gap, CSSPixels ideal_column_height, Optional<CSSPixels> max_column_height)
    : FragmentationContext(root_box)
    , m_column_count(column_count)
    , m_column_width(column_width)
    , m_column_gap(column_gap)
    , m_ideal_column_height(ideal_column_height)
    , m_max_column_height(max_column_height)
{
}

ColumnFragmentationContext::~ColumnFragmentationContext() = default;

CSSPixels ColumnFragmentationContext::fragmentainer_x_offset_at(CSSPixels progress) const
{
    auto current_fragmentainer = floor(progress / m_ideal_column_height);
    return (m_column_width + m_column_gap) * current_fragmentainer;
}

CSSPixels ColumnFragmentationContext::fragmentainer_y_offset_at(CSSPixels progress) const
{
    auto current_fragmentainer = floor(progress / m_ideal_column_height);
    return -m_ideal_column_height * current_fragmentainer;
}

// https://drafts.csswg.org/css-break/#remaining-fragmentainer-extent
CSSPixels ColumnFragmentationContext::remaining_fragmentainer_extent_at(CSSPixels progress) const
{
    return m_ideal_column_height - (progress % m_ideal_column_height);
}

FragmentationDecision ColumnFragmentationContext::make_fragmentation_decision_at(CSSPixels progress, CSSPixels item_height)
{
    // This function decides whether a piece of monolithic content is placed as-is, shunted down into the next
    // fragmentainer, or placed as-is and then sliced into fragments.
    auto remaining_fragmentainer_extent = remaining_fragmentainer_extent_at(progress);

    // If this item would exceed our height limit, try to shunt it into the next column if it can fit there.
    if (m_max_column_height.has_value() && item_height > m_max_column_height.value() - m_ideal_column_height + remaining_fragmentainer_extent) {
        // If the next column is not tall enough to fit the item, it must be fragmented across multiple columns instead.
        if (item_height > m_max_column_height.value())
            return FragmentationDecision::Fragment;

        // If the item is larger than our ideal height, but still fits within the height limit, shunting it to
        // the start of the next column will still incur a content deficit as it exceeds the ideal height of
        // that column.
        if (item_height > m_ideal_column_height)
            m_content_deficit += item_height - m_ideal_column_height;
        return FragmentationDecision::Shunt;
    }

    // If we are not height-limited and are in the last column, we cannot shunt or fragment anymore. We must place.
    if (floor(progress / m_ideal_column_height) >= m_column_count)
        return FragmentationDecision::Place;

    // If there is no content in the current column yet, we must place and take any potential content deficit to avoid
    // generating infinite empty columns.
    if (remaining_fragmentainer_extent == m_ideal_column_height) {
        if (item_height > m_ideal_column_height)
            m_content_deficit += item_height - m_ideal_column_height;
        return FragmentationDecision::Place;
    }

    // If we are not yet in the last column, calculate the deficit or surplus we would incur by placing the item as-is
    // or shunting it into the next column.

    // Note: The following calculations make it possible to shunt items which should fit entirely within the current
    //       column. The spec does not allow this if the column balancing is set to fill the columns sequentially.
    //       However, this will never be a problem, since these contexts either have their ideal height set to the max
    //       height (and so never allow taking a deficit that would need to be balanced out with surplus from an
    //       overzealous shunt) or their ideal height is equal to the total content height, in which case we never hit
    //       the end of the column and thus never get the chance to take such a deficit either.

    // Potential FIXME: This algorithm is somewhat short-sighted in nature and leads to situations where columns
    // alternate between having n and n+1 lines of text in them, since m_content_deficit likes to bounce between being
    // positive and negative, leading to alternating place and shunt decisions when the ideal content height cuts the
    // usual last line box in half. While the spec has no opinion on this, one could argue that it is more
    // aesthetically pleasing to first put only columns with n+1 lines in them, followed by only columns with n lines
    // in them. It would be nice if we could somehow anticipate this situation up-front and adjust our ideal height
    // both going in and when reaching the column where it needs to decrease by 1 line.

    auto deficit_if_placed = max(item_height - remaining_fragmentainer_extent, 0);
    auto surplus_if_shunted = remaining_fragmentainer_extent;

    // Calculate the improvement scores that placing and shunting would incur, both in the current column and at the
    // projected end of the last column. A lower score is better, because the score is simply the overall deviation
    // from our ideal column height.
    auto current_column_place_score = deficit_if_placed;
    auto current_column_shunt_score = surplus_if_shunted;
    auto last_column_place_score = abs(m_content_deficit + deficit_if_placed);
    auto last_column_shunt_score = abs(m_content_deficit - surplus_if_shunted);
    auto total_place_score = current_column_place_score + last_column_place_score;
    auto total_shunt_score = current_column_shunt_score + last_column_shunt_score;

    // Depending on the scores, we either take the surplus and shunt, or we take the deficit and place.
    // In case of a tie, we prioritize the shunt/place scores of the current column. The idea behind this is that,
    // while the decision does not matter in theory, the current column can only be improved now, whereas the final
    // column might still get more opportunities for improvement.
    if (total_shunt_score == total_place_score ? current_column_shunt_score < current_column_place_score : total_shunt_score < total_place_score) {
        m_content_deficit -= surplus_if_shunted;
        return FragmentationDecision::Shunt;
    }

    m_content_deficit += deficit_if_placed;
    return FragmentationDecision::Place;
}

}
