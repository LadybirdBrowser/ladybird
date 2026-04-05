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

enum class FragmentationDecision {
    Fragment,
    Place,
    Shunt
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

    virtual FragmentationDecision make_fragmentation_decision_at(CSSPixels progress, CSSPixels item_height)
    {
        if (remaining_fragmentainer_extent_at(progress) >= item_height)
            return FragmentationDecision::Place;

        return FragmentationDecision::Fragment;
    }

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
    ColumnFragmentationContext(GC::Ref<Box const> root_box, int column_count, CSSPixels column_width, CSSPixels column_gap, CSSPixels ideal_column_height, Optional<CSSPixels> max_column_height);
    virtual ~ColumnFragmentationContext();

    CSSPixels fragmentainer_x_offset_at(CSSPixels progress) const override;
    CSSPixels fragmentainer_y_offset_at(CSSPixels progress) const override;

    // https://drafts.csswg.org/css-break/#remaining-fragmentainer-extent
    CSSPixels remaining_fragmentainer_extent_at(CSSPixels progress) const override;

    virtual FragmentationDecision make_fragmentation_decision_at(CSSPixels progress, CSSPixels item_height) override;

private:
    int m_column_count;
    CSSPixels m_column_width;
    CSSPixels m_column_gap;
    // m_ideal_column_height represents our preferred height for a column which is calculated from the height that the
    // content would have if flowed into a single, long column. Fragmentation breaks are assumed to sit at this
    // preferred height.
    CSSPixels m_ideal_column_height;
    Optional<CSSPixels> m_max_column_height;

    // When monolithic content would be placed across a fragmentation break, it becomes impossible to honor the
    // ideal height for every column. In those cases, we either introduce a gap to shunt the monolithic content across
    // the break, into the next column, or we increase the actual column height to fit that content. If we increased
    // the actual height, we still want to fill all subsequent columns up to the ideal height, but we no longer have
    // enough content to do so. We call this a content deficit, and it is represented by this variable being positive.
    // If we introduce a gap, we now have too much content left. This amount is indicated by negative values and we
    // refer to it as a content surplus.
    // All fragmentation decisions are made to minimize deficit and surplus on both the current column and overall,
    // since the overall deficit and surplus are just the projected deviation we expect to be forced into on the
    // last column.
    CSSPixels m_content_deficit = 0;
};

}
