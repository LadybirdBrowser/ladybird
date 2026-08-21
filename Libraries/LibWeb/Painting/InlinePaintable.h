/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Painting/Paintable.h>

namespace Web::Painting {

// The paintable of an inline box that participates in an inline formatting context (e.g. a
// <span>, possibly fragmented across lines). It owns no fragments: the fragments and the
// per-line pieces it paints live on the containing block's PaintableWithLines, and its own
// geometry is the union of those pieces.
class InlinePaintable final : public Paintable {
public:
    static NonnullRefPtr<InlinePaintable> create(Layout::NodeWithStyle const&);
    virtual ~InlinePaintable() override;

private:
    explicit InlinePaintable(Layout::NodeWithStyle const&);
};

template<>
inline bool Paintable::fast_is<InlinePaintable>() const { return is_inline_paintable(); }

}
