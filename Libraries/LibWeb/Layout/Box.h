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

struct IntrinsicMinMaxCache {
    Optional<CSSPixels> min_content_width;
    Optional<CSSPixels> max_content_width;
    HashMap<CSSPixels, Optional<CSSPixels>> min_content_height;
    HashMap<CSSPixels, Optional<CSSPixels>> max_content_height;
};

class WEB_API Box : public NodeWithStyleAndBoxModelMetrics {
    GC_CELL(Box, NodeWithStyleAndBoxModelMetrics);

public:
    Painting::PaintableBox const* paintable_box() const;
    Painting::PaintableBox* paintable_box();

    // https://www.w3.org/TR/css-images-3/#natural-dimensions
    virtual CSS::SizeWithAspectRatio natural_size() const { return {}; }
    CSS::SizeWithAspectRatio intrinsic_content_box_size() const;
    virtual bool has_intrinsic_content_box_size() const { return false; }

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

    IntrinsicMinMaxCache& intrinsic_min_max_cache() const
    {
        if (!m_intrinsic_min_max_cache)
            m_intrinsic_min_max_cache = make<IntrinsicMinMaxCache>();
        return *m_intrinsic_min_max_cache;
    }
    void reset_cached_intrinsic_sizes() const { m_intrinsic_min_max_cache.clear(); }

protected:
    Box(DOM::Document&, DOM::Node*, GC::Ref<CSS::ComputedProperties>);
    Box(DOM::Document&, DOM::Node*, NonnullOwnPtr<CSS::ComputedValues>);
    virtual CSS::SizeWithAspectRatio compute_intrinsic_content_box_size() const { return natural_size(); }

private:
    virtual bool is_box() const final { return true; }

    Vector<GC::Ref<Node>> m_contained_abspos_children;

    OwnPtr<IntrinsicMinMaxCache> mutable m_intrinsic_min_max_cache;
};

template<>
inline bool Node::fast_is<Box>() const { return is_box(); }

}
