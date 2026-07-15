/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/Layout/AbsposLayoutInputs.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/FormattingContext.h>
#include <LibWeb/Layout/TableWrapper.h>
#include <LibWeb/Painting/Paintable.h>

namespace Web::Layout {

Box::Box(DOM::Document& document, DOM::Node* node, NonnullRefPtr<CSS::ComputedValues const> computed_values)
    : NodeWithStyleAndBoxModelMetrics(document, node, move(computed_values))
{
}

Box::~Box()
{
}

void Box::set_saved_abspos_layout_inputs(AbsposLayoutInputs const& abspos_layout_inputs)
{
    if (m_saved_abspos_layout_inputs)
        *m_saved_abspos_layout_inputs = abspos_layout_inputs;
    else
        m_saved_abspos_layout_inputs = make<AbsposLayoutInputs>(abspos_layout_inputs);
}

void Box::clear_saved_abspos_layout_inputs()
{
    m_saved_abspos_layout_inputs = nullptr;
}

bool Box::is_partial_relayout_boundary(RequireExistingPaintable require_existing_paintable) const
{
    // An absolutely or fixed positioned descendant whose containing block is outside this
    // box's subtree is laid out by a formatting context outside it, which makes subtree
    // isolation impossible for any kind of boundary.
    if (abspos_descendant_escapes())
        return false;

    // A nested <svg> never qualifies: its subtree is laid out in the outer SVG's
    // viewBox-transformed coordinate system, which a relayout rooted at the inner <svg> cannot
    // reproduce.
    bool is_outermost_svg_root = is_svg_svg_box() && !(parent() && (parent()->is_svg_box() || parent()->is_svg_svg_box()));

    // An in-flow SVG root's used size is determined solely by its own attributes and outer
    // context, never by its children, so its size and position from the previous layout can be
    // reused. An absolutely positioned SVG root's placement is not frozen, so it must qualify
    // through the saved-inputs replay path below instead.
    if (is_svg_svg_box() && !is_absolutely_positioned())
        return is_outermost_svg_root;

    if (!is_absolutely_positioned())
        return false;
    if (is_anonymous())
        return false;
    if (require_existing_paintable == RequireExistingPaintable::Yes && !paintable_box())
        return false;
    if (dom_node() == document().document_element())
        return false;
    if (!saved_abspos_layout_inputs())
        return false;

    // Only a full layout pass resolves anchor() functions in the inset properties to plain
    // values; a replay from saved inputs cannot.
    if (FormattingContext::box_inset_properties_contain_anchor_functions(*this))
        return false;

    // NOTE: Content-dependent sizing (shrink-to-fit, intrinsic constraints, aspect-ratio) does
    //       not disqualify a boundary: replay re-solves the boundary's own size, and a resized
    //       boundary triggers ancestor scrollable overflow recomputation after commit.

    auto formatting_context_type = FormattingContext::formatting_context_type_created_by_box(*this);
    if (!formatting_context_type.has_value())
        return false;
    switch (*formatting_context_type) {
    case FormattingContext::Type::Block:
    case FormattingContext::Type::Flex:
    case FormattingContext::Type::Grid:
        return true;
    case FormattingContext::Type::SVG:
        return is_outermost_svg_root;
    default:
        return false;
    }
}

CSS::SizeWithAspectRatio Box::auto_content_box_size() const
{
    // https://drafts.csswg.org/css-contain-2/#containment-size
    // Replaced elements must be treated as having a natural width and height of 0 and no natural aspect
    // ratio.
    if (has_size_containment())
        return { 0, 0, {} };

    return compute_auto_content_box_size();
}

RefPtr<Painting::Paintable> Box::create_paintable() const
{
    return Painting::Paintable::create(*this);
}

RefPtr<Painting::Paintable> Box::paintable_box()
{
    if (auto paintable = Node::paintable())
        return static_cast<Painting::Paintable&>(*paintable);
    return nullptr;
}

RefPtr<Painting::Paintable const> Box::paintable_box() const
{
    if (auto paintable = Node::paintable())
        return static_cast<Painting::Paintable const&>(*paintable);
    return nullptr;
}

Optional<CSSPixelFraction> Box::preferred_aspect_ratio() const
{
    auto const& computed_aspect_ratio = computed_values().aspect_ratio();

    // https://www.w3.org/TR/css-contain-2/#containment-size
    if (!has_size_containment() && computed_aspect_ratio.use_natural_aspect_ratio_if_available) {
        if (auto auto_size = auto_content_box_size(); auto_size.has_aspect_ratio())
            return auto_size.aspect_ratio;
    }

    if (!computed_aspect_ratio.preferred_ratio.has_value())
        return {};

    auto ratio = computed_aspect_ratio.preferred_ratio.value();
    if (ratio.is_degenerate())
        return {};

    auto fraction = CSSPixelFraction(ratio.numerator(), ratio.denominator());
    // ratio.is_degenerate() operates on doubles while CSSPixelFraction uses CSSPixels, so we need to check again here.
    if (fraction == 0)
        return {};

    return fraction;
}

}
