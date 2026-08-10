/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/LayoutRustBridge.h>
#include <LibWeb/Layout/TableWrapper.h>
#include <LibWeb/Layout/TextInputBox.h>
#include <LibWeb/Painting/Paintable.h>

namespace Web::Layout {

Box::Box(DOM::Document& document, GC::Ptr<DOM::Node> node, NonnullRefPtr<CSS::ComputedValues const> computed_values)
    : NodeWithStyle(document, node, move(computed_values))
{
}

Box::~Box()
{
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
    if (!has_saved_abspos_layout_inputs())
        return false;

    // Only a full layout pass resolves anchor() functions in the inset properties to plain
    // values; a replay from saved inputs cannot.
    if (box_inset_properties_contain_anchor_functions(*this))
        return false;

    // NOTE: Content-dependent sizing (shrink-to-fit, intrinsic constraints, aspect-ratio) does
    //       not disqualify a boundary: replay re-solves the boundary's own size, and a resized
    //       boundary triggers ancestor scrollable overflow recomputation after commit.

    auto formatting_context_type = formatting_context_type_created_by_box(*this);
    if (!formatting_context_type.has_value())
        return false;
    switch (*formatting_context_type) {
    case RustFFI::FfiFormattingContextType::Block:
    case RustFFI::FfiFormattingContextType::Flex:
    case RustFFI::FfiFormattingContextType::Grid:
        return true;
    case RustFFI::FfiFormattingContextType::Svg:
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

RustFFI::FfiReplacedContentFacts Box::build_replaced_content_facts_for_arena() const
{
    RustFFI::FfiReplacedContentFacts facts {};
    auto auto_content_size = auto_content_box_size();
    facts.has_auto_content_width = auto_content_size.has_width();
    facts.auto_content_width = auto_content_size.width.value_or(0).raw_value();
    facts.has_auto_content_height = auto_content_size.has_height();
    facts.auto_content_height = auto_content_size.height.value_or(0).raw_value();
    if (auto_content_size.aspect_ratio.has_value()) {
        facts.auto_content_aspect_ratio_numerator = auto_content_size.aspect_ratio->numerator().raw_value();
        facts.auto_content_aspect_ratio_denominator = auto_content_size.aspect_ratio->denominator().raw_value();
    }
    if (computed_values().appearance() == CSS::Appearance::None) {
        if (auto const* input = as_if<HTML::HTMLInputElement>(dom_node())) {
            switch (input->type_state()) {
            case HTML::HTMLInputElement::TypeAttributeState::Text:
            case HTML::HTMLInputElement::TypeAttributeState::Search:
            case HTML::HTMLInputElement::TypeAttributeState::URL:
            case HTML::HTMLInputElement::TypeAttributeState::Telephone:
            case HTML::HTMLInputElement::TypeAttributeState::Email:
            case HTML::HTMLInputElement::TypeAttributeState::Password:
            case HTML::HTMLInputElement::TypeAttributeState::Number: {
                auto default_preferred_size = TextInputBox::default_preferred_size_for_text_control(*input, *this);
                facts.has_default_preferred_width = default_preferred_size.has_width();
                facts.default_preferred_width = default_preferred_size.width.value_or(0).raw_value();
                facts.has_default_preferred_height = default_preferred_size.has_height();
                facts.default_preferred_height = default_preferred_size.height.value_or(0).raw_value();
                break;
            }
            default:
                break;
            }
        }
    }
    return facts;
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
