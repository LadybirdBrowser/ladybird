/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/OwnPtr.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/CSS/Sizing.h>
#include <LibWeb/Export.h>
#include <LibWeb/Layout/Node.h>

namespace Web::Layout {

struct AbsposLayoutInputs;

enum class RequireExistingPaintable : u8 {
    No,
    Yes,
};

struct LineBoxFragmentCoordinate {
    size_t line_box_index { 0 };
    size_t fragment_index { 0 };
};

struct IntrinsicSizeCacheKey {
    Optional<CSSPixels> measured_at_width;
    Optional<CSSPixels> percentage_basis_width;
    Optional<CSSPixels> percentage_basis_height;
    Optional<CSSPixels> quirks_mode_percentage_basis_height;

    bool operator==(IntrinsicSizeCacheKey const&) const = default;
};

struct IntrinsicSizes {
    HashMap<IntrinsicSizeCacheKey, CSSPixels> min_content_width;
    HashMap<IntrinsicSizeCacheKey, CSSPixels> max_content_width;
    HashMap<IntrinsicSizeCacheKey, CSSPixels> min_content_height;
    HashMap<IntrinsicSizeCacheKey, CSSPixels> max_content_height;
};

class WEB_API Box : public NodeWithStyleAndBoxModelMetrics {
    GC_CELL(Box, NodeWithStyleAndBoxModelMetrics);
    GC_DECLARE_ALLOCATOR(Box);

public:
    RefPtr<Painting::Paintable const> paintable_box() const;
    RefPtr<Painting::Paintable> paintable_box();

    // A partial relayout boundary is a box whose subtree can be re-laid out in
    // isolation: its own used size and position are guaranteed not to change
    // when layout is invalidated somewhere inside its subtree.
    bool is_partial_relayout_boundary(RequireExistingPaintable = RequireExistingPaintable::Yes) const;

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

    virtual RefPtr<Painting::Paintable> create_paintable() const override;

    // The inputs the absolutely positioned element layout algorithm consumed the last time a
    // committing layout pass laid this box out; a partial relayout can replay the algorithm
    // from them without re-running the ancestor formatting context.
    AbsposLayoutInputs const* saved_abspos_layout_inputs() const { return m_saved_abspos_layout_inputs.ptr(); }
    void set_saved_abspos_layout_inputs(AbsposLayoutInputs const&);
    void clear_saved_abspos_layout_inputs();

    // Whether an absolutely or fixed positioned descendant of this box has its containing
    // block outside this box's subtree, so the descendant's layout escapes the subtree.
    // Re-derived whenever containing block pointers are recomputed.
    bool abspos_descendant_escapes() const { return m_abspos_descendant_escapes; }
    void set_abspos_descendant_escapes(bool value) { m_abspos_descendant_escapes = value; }

    void set_default_scroll_shift(WeakPtr<Node> anchor, bool compensates_for_scroll_in_x, bool compensates_for_scroll_in_y)
    {
        m_default_scroll_shift_anchor = move(anchor);
        m_compensates_for_scroll_in_x = compensates_for_scroll_in_x;
        m_compensates_for_scroll_in_y = compensates_for_scroll_in_y;
    }
    Node* default_scroll_shift_anchor() const { return m_default_scroll_shift_anchor.ptr(); }
    bool compensates_for_scroll_in_x() const { return m_compensates_for_scroll_in_x; }
    bool compensates_for_scroll_in_y() const { return m_compensates_for_scroll_in_y; }

    IntrinsicSizes& cached_intrinsic_sizes() const
    {
        if (!m_cached_intrinsic_sizes)
            m_cached_intrinsic_sizes = make<IntrinsicSizes>();
        return *m_cached_intrinsic_sizes;
    }
    void reset_cached_intrinsic_sizes() const { m_cached_intrinsic_sizes.clear(); }

    Box(DOM::Document&, DOM::Node*, NonnullRefPtr<CSS::ComputedValues const>);

protected:
    virtual CSS::SizeWithAspectRatio compute_auto_content_box_size() const { return natural_size(); }

private:
    virtual bool is_box() const final { return true; }

    OwnPtr<AbsposLayoutInputs> m_saved_abspos_layout_inputs;

    WeakPtr<Node> m_default_scroll_shift_anchor;
    bool m_compensates_for_scroll_in_x { false };
    bool m_compensates_for_scroll_in_y { false };
    bool m_abspos_descendant_escapes { false };

    OwnPtr<IntrinsicSizes> mutable m_cached_intrinsic_sizes;
};

template<>
inline bool Node::fast_is<Box>() const { return is_box(); }

}

namespace AK {

template<>
struct Traits<Web::Layout::IntrinsicSizeCacheKey> : public DefaultTraits<Web::Layout::IntrinsicSizeCacheKey> {
    static unsigned hash(Web::Layout::IntrinsicSizeCacheKey const& key)
    {
        auto optional_hash = [](Optional<Web::CSSPixels> const& value) -> unsigned {
            return value.has_value() ? pair_int_hash(1u, Traits<Web::CSSPixels>::hash(*value)) : 0u;
        };
        auto hash = optional_hash(key.measured_at_width);
        hash = pair_int_hash(hash, optional_hash(key.percentage_basis_width));
        hash = pair_int_hash(hash, optional_hash(key.percentage_basis_height));
        hash = pair_int_hash(hash, optional_hash(key.quirks_mode_percentage_basis_height));
        return hash;
    }
};

}
