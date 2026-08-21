/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Painting/PaintableWithLines.h>

namespace Web::Painting {

// The paintable of an inline box that participates in an inline formatting context (e.g. a
// <span>, possibly fragmented across lines). It owns no fragments: the fragments and the
// per-line pieces it paints live on the containing block's PaintableWithLines, and its own
// geometry is the union of those pieces.
class InlinePaintable final : public Paintable {
public:
    static NonnullRefPtr<InlinePaintable> create(Layout::NodeWithStyle const&);
    virtual ~InlinePaintable() override;

    // Whether this box paints its own foreground (fragments and caret) instead of the
    // containing block: it forms a group that content must be recorded inside.
    bool is_self_painting() const { return has_stacking_context() || is_positioned(); }

    virtual CSSPixelPoint box_type_agnostic_position() const override;

    bool has_content() const;

private:
    explicit InlinePaintable(Layout::NodeWithStyle const&);

    bool has_content_pieces() const;

public:
    Optional<PaintableWithLines::CaretPaint> resolve_empty_editable_caret_paint() const;
};

template<>
inline bool Paintable::fast_is<InlinePaintable>() const { return is_inline_paintable(); }

}
