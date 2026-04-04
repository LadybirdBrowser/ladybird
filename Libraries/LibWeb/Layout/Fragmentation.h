/*
 * Copyright (c) 2026-present, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Layout {

enum class FragmentationState {
    Unfragmented,
    HorizontalStart,
    HorizontalMiddle,
    HorizontalEnd,
    VerticalStart,
    VerticalMiddle,
    VerticalEnd
};

// https://drafts.csswg.org/css-break/#fragmentation-context
// FIXME: Some of this will need to be merged with AvailableSpace in some form once we get to fragmentation contexts
//        with variable-width fragmentainers.
//        (print mode under the influence of @page rules that turn only some pages sideways comes to mind)
class FragmentationContext {
public:
    virtual ~FragmentationContext();

    virtual CSSPixels fragmentainer_x_offset_at(CSSPixels) const { return 0; }
    virtual CSSPixels fragmentainer_y_offset_at(CSSPixels) const { return 0; }

    // https://drafts.csswg.org/css-break/#remaining-fragmentainer-extent
    virtual CSSPixels remaining_fragmentainer_extent_at(CSSPixels) const { return 0; }

    GC::Ref<Box const> root() const { return m_root_box; }

protected:
    FragmentationContext(GC::Ref<Box const> root_box);

private:
    CSSPixels m_used_fragmentainer_extent;
    // This is the box that hosts the fragmented flow. It is used to determine the fragmentation-relative position of
    // elements in said flow. A containing block for fragmentation-related positioning, if you will.
    GC::Ref<Box const> m_root_box;
};

// https://drafts.csswg.org/css-multicol-2/#the-multi-column-model
class ColumnFragmentationContext : public FragmentationContext {
public:
    ColumnFragmentationContext(GC::Ref<Box const> root_box, CSSPixels column_width, CSSPixels column_height, CSSPixels column_gap);
    virtual ~ColumnFragmentationContext();

    CSSPixels fragmentainer_x_offset_at(CSSPixels progress) const override;
    CSSPixels fragmentainer_y_offset_at(CSSPixels progress) const override;

    // https://drafts.csswg.org/css-break/#remaining-fragmentainer-extent
    CSSPixels remaining_fragmentainer_extent_at(CSSPixels progress) const override;

private:
    CSSPixels m_column_width;
    CSSPixels m_column_height;
    CSSPixels m_column_gap;
};

}
