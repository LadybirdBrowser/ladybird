/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/CSS/Sizing.h>
#include <LibWeb/Export.h>
#include <LibWeb/Layout/Node.h>

namespace Web::Layout {

struct LineBoxFragmentCoordinate {
    size_t line_box_index { 0 };
    size_t fragment_index { 0 };
};

struct IntrinsicSizes {
    Optional<CSSPixels> min_content_width;
    Optional<CSSPixels> max_content_width;
    HashMap<CSSPixels, Optional<CSSPixels>> min_content_height;
    HashMap<CSSPixels, Optional<CSSPixels>> max_content_height;
};

class WEB_API Box : public NodeWithStyleAndBoxModelMetrics {
    GC_CELL(Box, NodeWithStyleAndBoxModelMetrics);
    GC_DECLARE_ALLOCATOR(Box);

public:
    Painting::PaintableBox const* paintable_box() const;
    Painting::PaintableBox* paintable_box();

    // https://www.w3.org/TR/css-images-3/#natural-dimensions
    virtual CSS::SizeWithAspectRatio natural_size() const { return {}; }

    // When computed width/height is auto, auto_content_box_size gives the fallback content-box size for
    // elements whose used size is determined by natural dimensions, attributes, or defaults other than
    // the generic UA fallback (300x150). Any returned aspect ratio comes from natural dimensions (when
    // available) or may be computed from fallback sizing. Don't confuse this with the CSS preferred
    // aspect ratio.
    CSS::SizeWithAspectRatio auto_content_box_size() const;
    virtual bool has_auto_content_box_size() const { return false; }

    // https://www.w3.org/TR/css-sizing-4/#preferred-aspect-ratio
    Optional<CSSPixelFraction> preferred_aspect_ratio() const;
    bool has_preferred_aspect_ratio() const { return preferred_aspect_ratio().has_value(); }

    virtual ~Box() override;

    virtual void did_set_content_size() { }

    virtual GC::Ptr<Painting::Paintable> create_paintable() const override;

    void add_contained_abspos_child(GC::Ref<Node> child) { m_contained_abspos_children.append(child); }
    void clear_contained_abspos_children() { m_contained_abspos_children.clear(); }
    Vector<GC::Ref<Node>> const& contained_abspos_children() const { return m_contained_abspos_children; }

    virtual void visit_edges(Cell::Visitor&) override;

    IntrinsicSizes& cached_intrinsic_sizes() const
    {
        if (!m_cached_intrinsic_sizes)
            m_cached_intrinsic_sizes = make<IntrinsicSizes>();
        return *m_cached_intrinsic_sizes;
    }
    void reset_cached_intrinsic_sizes() const { m_cached_intrinsic_sizes.clear(); }

protected:
    Box(DOM::Document&, DOM::Node*, GC::Ref<CSS::ComputedProperties>);
    Box(DOM::Document&, DOM::Node*, NonnullOwnPtr<CSS::ComputedValues>);
    virtual CSS::SizeWithAspectRatio compute_auto_content_box_size() const { return natural_size(); }

private:
    virtual bool is_box() const final { return true; }

    Vector<GC::Ref<Node>> m_contained_abspos_children;

    OwnPtr<IntrinsicSizes> mutable m_cached_intrinsic_sizes;
};

template<>
inline bool Node::fast_is<Box>() const { return is_box(); }

}
