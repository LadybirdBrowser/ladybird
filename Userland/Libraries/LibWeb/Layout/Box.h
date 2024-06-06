/*
 * Copyright (c) 2018-2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <LibGfx/Rect.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Layout/Node.h>

namespace Web::Layout {

struct LineBoxFragmentCoordinate {
    size_t line_box_index { 0 };
    size_t fragment_index { 0 };
};

class Box : public NodeWithStyleAndBoxModelMetrics {
    JS_CELL(Box, NodeWithStyleAndBoxModelMetrics);

public:
    Painting::PaintableBox const* paintable_box() const;
    Painting::PaintableBox* paintable_box();

    bool is_body() const;

    virtual void prepare_for_replaced_layout() { }

    // https://www.w3.org/TR/css-images-3/#natural-dimensions
    Optional<CSSPixels> natural_width() const { return m_natural_width; }
    Optional<CSSPixels> natural_height() const { return m_natural_height; }
    Optional<CSSPixelFraction> natural_aspect_ratio() const { return m_natural_aspect_ratio; }

    bool has_natural_width() const { return natural_width().has_value(); }
    bool has_natural_height() const { return natural_height().has_value(); }
    bool has_natural_aspect_ratio() const { return natural_aspect_ratio().has_value(); }

    void set_natural_width(Optional<CSSPixels> width) { m_natural_width = width; }
    void set_natural_height(Optional<CSSPixels> height) { m_natural_height = height; }
    void set_natural_aspect_ratio(Optional<CSSPixelFraction> ratio) { m_natural_aspect_ratio = ratio; }

    // https://www.w3.org/TR/css-sizing-4/#preferred-aspect-ratio
    Optional<CSSPixelFraction> preferred_aspect_ratio() const;
    bool has_preferred_aspect_ratio() const { return preferred_aspect_ratio().has_value(); }

    virtual ~Box() override;

    virtual void did_set_content_size() { }

    virtual JS::GCPtr<Painting::Paintable> create_paintable() const override;

    bool is_scroll_container() const;

    bool is_user_scrollable() const;

protected:
    Box(DOM::Document&, DOM::Node*, NonnullRefPtr<CSS::StyleProperties>);
    Box(DOM::Document&, DOM::Node*, NonnullOwnPtr<CSS::ComputedValues>);

private:
    virtual bool is_box() const final { return true; }

    Optional<CSSPixels> m_natural_width;
    Optional<CSSPixels> m_natural_height;
    Optional<CSSPixelFraction> m_natural_aspect_ratio;
};

template<>
inline bool Node::fast_is<Box>() const { return is_box(); }

}
