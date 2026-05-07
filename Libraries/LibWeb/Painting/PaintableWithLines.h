/*
 * Copyright (c) 2022-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/PaintableFragment.h>

namespace Web::Painting {

class PaintableWithLines : public PaintableBox {
public:
    static NonnullRefPtr<PaintableWithLines> create(Layout::BlockContainer const&);
    static NonnullRefPtr<PaintableWithLines> create(Layout::InlineNode const&, size_t line_index);
    virtual ~PaintableWithLines() override;
    virtual StringView class_name() const override { return "PaintableWithLines"sv; }

    virtual void reset_for_relayout() override;

    Vector<PaintableFragment> const& fragments() const { return m_fragments; }
    Vector<PaintableFragment>& fragments() { return m_fragments; }

    void add_fragment(Layout::LineBoxFragment const& fragment)
    {
        m_fragments.append(PaintableFragment { fragment });
    }

    virtual void paint(DisplayListRecordingContext&, PaintPhase) const override;
    static void paint_text_fragment_debug_highlight(DisplayListRecordingContext&, PaintableFragment const&);

    [[nodiscard]] virtual TraversalDecision hit_test(CSSPixelPoint position, HitTestType type, Function<TraversalDecision(HitTestResult)> const& callback) const override;
    [[nodiscard]] TraversalDecision hit_test_fragments(CSSPixelPoint position, CSSPixelPoint local_position, HitTestType type, Function<TraversalDecision(HitTestResult)> const& callback) const;

    size_t line_index() const { return m_line_index; }

protected:
    PaintableWithLines(Layout::BlockContainer const&);
    PaintableWithLines(Layout::InlineNode const&, size_t line_index);

private:
    [[nodiscard]] virtual bool is_paintable_with_lines() const final { return true; }

    Optional<PaintableFragment const&> fragment_at_position(DOM::Position const&) const;
    void paint_cursor(DisplayListRecordingContext&) const;

    Vector<PaintableFragment> m_fragments;

    size_t m_line_index { 0 };
};

}
