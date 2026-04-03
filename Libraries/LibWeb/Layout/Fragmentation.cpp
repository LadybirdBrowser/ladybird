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

ColumnFragmentationContext::ColumnFragmentationContext(GC::Ref<Box const> root_box, CSSPixels column_width, CSSPixels column_height, CSSPixels column_gap)
    : FragmentationContext(root_box)
    , m_column_width(column_width)
    , m_column_height(column_height)
    , m_column_gap(column_gap)
{
}

ColumnFragmentationContext::~ColumnFragmentationContext() = default;

CSSPixels ColumnFragmentationContext::fragmentainer_x_offset_at(CSSPixels progress) const
{
    auto current_fragmentainer = floor(progress / m_column_height);
    return (m_column_width + m_column_gap) * current_fragmentainer;
}

CSSPixels ColumnFragmentationContext::fragmentainer_y_offset_at(CSSPixels progress) const
{
    auto current_fragmentainer = floor(progress / m_column_height);
    return -m_column_height * current_fragmentainer;
}

// https://drafts.csswg.org/css-break/#remaining-fragmentainer-extent
CSSPixels ColumnFragmentationContext::remaining_fragmentainer_extent_at(CSSPixels progress) const
{
    return m_column_height - (progress % m_column_height);
}

}
