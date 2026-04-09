/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Demangle.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/BorderRadiusStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/CSS/StyleValues/RatioStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>
#include <LibWeb/CSS/StyleValues/URLStyleValue.h>
#include <LibWeb/CSS/SystemColor.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Dump.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/FormattingContext.h>
#include <LibWeb/Layout/InlineNode.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/SVGSVGBox.h>
#include <LibWeb/Layout/TableWrapper.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/SVG/SVGFilterElement.h>
#include <LibWeb/SVG/SVGForeignObjectElement.h>

namespace Web::Layout {

Node::Node(DOM::Document& document, DOM::Node* node)
    : m_dom_node(node ? *node : document)
    , m_anonymous(node == nullptr)
{
    if (node)
        node->set_layout_node({}, *this);
}

Node::~Node() = default;

void Node::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_dom_node);
    for (auto const& paintable : m_paintable) {
        visitor.visit(GC::Ptr { &paintable });
    }
    visitor.visit(m_containing_block);
    visitor.visit(m_inline_containing_block_if_applicable);
    visitor.visit(m_pseudo_element_generator);
    TreeNode::visit_edges(visitor);
}

// https://www.w3.org/TR/css-display-3/#out-of-flow
bool Node::is_out_of_flow(FormattingContext const& formatting_context) const
{
    // A layout node is out of flow if either:

    // 1. It is floated (which requires that floating is not inhibited).
    if (!formatting_context.inhibits_floating() && computed_values().float_() != CSS::Float::None)
        return true;

    // 2. It is "absolutely positioned".
    if (is_absolutely_positioned())
        return true;

    return false;
}

// https://drafts.csswg.org/css-position-3/#absolute-positioning-containing-block
// Checks if the computed values of this node would establish an absolute positioning
// containing block. This is separate from establishes_an_absolute_positioning_containing_block()
// because that function also checks is<Box>, but we need these checks for inline elements too.
bool Node::computed_values_establish_absolute_positioning_containing_block() const
{
    auto const& computed_values = this->computed_values();

    if (computed_values.position() != CSS::Positioning::Static)
        return true;

    // https://drafts.csswg.org/css-will-change/#will-change
    // If any non-initial value of a property would cause the element to generate a containing block for absolutely
    // positioned elements, specifying that property in will-change must cause the element to generate a containing
    // block for absolutely positioned elements.
    auto will_change_property = [&](CSS::PropertyID property_id) {
        return computed_values.will_change().has_property(property_id);
    };

    // https://drafts.csswg.org/css-transforms-1/#propdef-transform
    // Any computed value other than none for the transform affects containing block and stacking context
    if (!computed_values.transformations().is_empty() || will_change_property(CSS::PropertyID::Transform))
        return true;
    if (computed_values.translate() || will_change_property(CSS::PropertyID::Translate))
        return true;
    if (computed_values.rotate() || will_change_property(CSS::PropertyID::Rotate))
        return true;
    if (computed_values.scale() || will_change_property(CSS::PropertyID::Scale))
        return true;

    // https://drafts.csswg.org/css-transforms-2/#propdef-perspective
    // The use of this property with any value other than 'none' establishes a stacking context. It also establishes
    // a containing block for all descendants, just like the 'transform' property does.
    if (computed_values.perspective().has_value() || will_change_property(CSS::PropertyID::Perspective))
        return true;

    // https://drafts.csswg.org/filter-effects-1/#FilterProperty
    // A value other than none for the filter property results in the creation of a containing block for absolute and
    // fixed positioned descendants, unless the element it applies to is a document root element in the current
    // browsing context.
    if ((computed_values.filter().has_filters() || will_change_property(CSS::PropertyID::Filter)) && !is_root_element())
        return true;

    // https://drafts.csswg.org/filter-effects-2/#BackdropFilterProperty
    // A computed value of other than none results in the creation of both a stacking context and a containing block
    // for absolute and fixed position descendants, unless the element it applies to is a document root element in the
    // current browsing context.
    if ((computed_values.backdrop_filter().has_filters() || will_change_property(CSS::PropertyID::BackdropFilter)) && !is_root_element())
        return true;

    // https://drafts.csswg.org/css-contain-2/#containment-types
    // 4. The layout containment box establishes an absolute positioning containing block and a fixed positioning
    //    containing block.
    // 4. The paint containment box establishes an absolute positioning containing block and a fixed positioning
    //    containing block.
    if (has_layout_containment() || has_paint_containment() || will_change_property(CSS::PropertyID::Contain))
        return true;

    // https://drafts.csswg.org/css-transforms-2/#transform-style-property
    // A computed value of 'preserve-3d' for 'transform-style' on a transformable element establishes both a
    // stacking context and a containing block for all descendants.
    // FIXME: Check that the element is a transformable element.
    if (computed_values.transform_style() == CSS::TransformStyle::Preserve3d || will_change_property(CSS::PropertyID::TransformStyle))
        return true;

    // https://drafts.csswg.org/css-view-transitions-1/#snapshot-containing-block-concept
    // FIXME: The snapshot containing block is considered to be an absolute positioning containing block and a fixed
    //        positioning containing block for ::view-transition and its descendants.

    return false;
}

// https://drafts.csswg.org/css-position-3/#absolute-positioning-containing-block
bool Node::establishes_an_absolute_positioning_containing_block() const
{
    if (!is<Box>(*this))
        return false;

    if (is<Viewport>(*this))
        return true;

    // https://github.com/w3c/fxtf-drafts/issues/307#issuecomment-499612420
    // foreignObject establishes a containing block for absolutely and fixed positioned elements.
    if (is_svg_foreign_object_box())
        return true;

    return computed_values_establish_absolute_positioning_containing_block();
}

// https://drafts.csswg.org/css-position-3/#fixed-positioning-containing-block
bool Node::establishes_a_fixed_positioning_containing_block() const
{
    if (!is<Box>(*this))
        return false;

    auto const& computed_values = this->computed_values();

    // https://drafts.csswg.org/css-will-change/#will-change
    // If any non-initial value of a property would cause the element to generate a containing block for fixed
    // positioned elements, specifying that property in will-change must cause the element to generate a containing
    // block for fixed positioned elements.
    auto will_change_property = [&](CSS::PropertyID property_id) {
        return computed_values.will_change().has_property(property_id);
    };

    // https://drafts.csswg.org/css-transforms-1/#propdef-transform
    // Any computed value other than none for the transform affects containing block and stacking context
    if (!computed_values.transformations().is_empty() || will_change_property(CSS::PropertyID::Transform))
        return true;
    if (computed_values.translate() || will_change_property(CSS::PropertyID::Translate))
        return true;
    if (computed_values.rotate() || will_change_property(CSS::PropertyID::Rotate))
        return true;
    if (computed_values.scale() || will_change_property(CSS::PropertyID::Scale))
        return true;

    // https://drafts.csswg.org/css-transforms-2/#propdef-perspective
    // The use of this property with any value other than 'none' establishes a stacking context. It also establishes
    // a containing block for all descendants, just like the 'transform' property does.
    if (computed_values.perspective().has_value() || will_change_property(CSS::PropertyID::Perspective))
        return true;

    // https://drafts.csswg.org/filter-effects-1/#FilterProperty
    // A value other than none for the filter property results in the creation of a containing block for absolute and
    // fixed positioned descendants, unless the element it applies to is a document root element in the current
    // browsing context.
    if ((computed_values.filter().has_filters() || will_change_property(CSS::PropertyID::Filter)) && !is_root_element())
        return true;

    // https://drafts.csswg.org/filter-effects-2/#BackdropFilterProperty
    // A computed value of other than none results in the creation of both a stacking context and a containing block
    // for absolute and fixed position descendants, unless the element it applies to is a document root element in the
    // current browsing context.
    if ((computed_values.backdrop_filter().has_filters() || will_change_property(CSS::PropertyID::BackdropFilter)) && !is_root_element())
        return true;

    // https://drafts.csswg.org/css-contain-2/#containment-types
    // 4. The layout containment box establishes an absolute positioning containing block and a fixed positioning
    //    containing block.
    // 4. The paint containment box establishes an absolute positioning containing block and a fixed positioning
    //    containing block.
    if (has_layout_containment() || has_paint_containment() || will_change_property(CSS::PropertyID::Contain))
        return true;

    // https://drafts.csswg.org/css-transforms-2/#transform-style-property
    // A computed value of 'preserve-3d' for 'transform-style' on a transformable element establishes both a
    // stacking context and a containing block for all descendants.
    // FIXME: Check that the element is a transformable element.
    if (computed_values.transform_style() == CSS::TransformStyle::Preserve3d || will_change_property(CSS::PropertyID::TransformStyle))
        return true;

    // https://drafts.csswg.org/css-view-transitions-1/#snapshot-containing-block-concept
    // FIXME: The snapshot containing block is considered to be an absolute positioning containing block and a fixed
    //        positioning containing block for ::view-transition and its descendants.

    return false;
}

static GC::Ptr<Box> nearest_ancestor_capable_of_forming_a_containing_block(Node& node)
{
    for (auto* ancestor = node.parent(); ancestor; ancestor = ancestor->parent()) {
        if (ancestor->is_block_container()
            || ancestor->display().is_flex_inside()
            || ancestor->display().is_grid_inside()
            || ancestor->is_replaced_box_with_children()) {
            return as<Box>(ancestor);
        }
    }
    return nullptr;
}

void Node::recompute_containing_block(Badge<DOM::Document>)
{
    // Reset the inline containing block - we'll set it below if applicable.
    m_inline_containing_block_if_applicable = nullptr;

    if (is<TextNode>(*this)) {
        m_containing_block = nearest_ancestor_capable_of_forming_a_containing_block(*this);
        return;
    }

    auto position = computed_values().position();

    // https://drafts.csswg.org/css-position-3/#absolute-cb
    if (position == CSS::Positioning::Absolute) {
        auto* ancestor = parent();
        while (ancestor && !ancestor->establishes_an_absolute_positioning_containing_block())
            ancestor = ancestor->parent();
        m_containing_block = static_cast<Box*>(ancestor);

        // FIXME: Containing block handling for absolutely positioned elements needs architectural improvements.
        //
        //        The CSS specification defines the containing block as a *rectangle*, not a box. For most cases,
        //        this rectangle is derived from the padding box of the nearest positioned ancestor Box. However,
        //        when the positioned ancestor is an *inline* element (e.g., a <span> with position: relative),
        //        the containing block rectangle should be the bounding box of that inline's fragments.
        //
        //        Currently, m_containing_block is typed as Box*, which cannot represent inline elements.
        //        The proper fix would be to:
        //        1. Separate the concept of "the node that establishes the containing block" from "the containing
        //           block rectangle".
        //        2. Store a reference to the establishing node (which could be InlineNode or Box).
        //        3. Compute the containing block rectangle on demand based on the establishing node's type.
        //
        //        For now, we use a workaround: check if there's an inline element with position:relative (or
        //        other containing-block-establishing properties) between this node and its containing_block()
        //        in the DOM tree. If found, store it in m_inline_containing_block_if_applicable.
        //
        //        We check the DOM tree here (rather than the layout tree) because when a block element is inside
        //        an inline element, the layout tree restructures so the block becomes a sibling of the inline.
        //        But the CSS containing block relationship is based on the DOM structure.
        if (m_containing_block) {
            auto const* containing_block_dom_node = m_containing_block->dom_node();

            // For pseudo-elements, we need to start from the generating element itself, since it may
            // be the inline containing block. For regular elements, start from parent_element().
            GC::Ptr<DOM::Element const> first_ancestor_to_check;
            if (is_generated_for_pseudo_element()) {
                first_ancestor_to_check = m_pseudo_element_generator.ptr();
            } else if (auto const* this_dom_node = dom_node()) {
                first_ancestor_to_check = this_dom_node->parent_element();
            }

            for (auto dom_ancestor = first_ancestor_to_check; dom_ancestor; dom_ancestor = dom_ancestor->parent_element()) {
                // Stop if we reach the DOM node of the containing block.
                if (dom_ancestor.ptr() == containing_block_dom_node)
                    break;

                // NB: Called during containing block recomputation as part of layout.
                // Check if this DOM element has an InlineNode in the layout tree.
                auto layout_node = dom_ancestor->unsafe_layout_node();
                if (!layout_node || !is<InlineNode>(*layout_node))
                    continue;

                // Check if this inline establishes an absolute positioning containing block.
                if (layout_node->computed_values_establish_absolute_positioning_containing_block()) {
                    m_inline_containing_block_if_applicable = const_cast<InlineNode*>(static_cast<InlineNode const*>(layout_node.ptr()));
                    break;
                }
            }
        }

        return;
    }

    // https://drafts.csswg.org/css-position-3/#fixed-cb
    if (position == CSS::Positioning::Fixed) {
        // The containing block is established by the nearest ancestor box that establishes an fixed positioning
        // containing block, with the bounds of the containing block determined identically to the absolute positioning
        // containing block.
        auto* ancestor = parent();
        while (ancestor && !ancestor->establishes_a_fixed_positioning_containing_block())
            ancestor = ancestor->parent();
        // If no ancestor establishes one, the box’s fixed positioning containing block is the initial fixed containing
        // block:
        if (!ancestor) {
            //  - in continuous media, the layout viewport (whose size matches the dynamic viewport size); as a result,
            //    fixed boxes do not move when the document is scrolled.
            ancestor = &root();
            // FIXME: - in paged media, the page area of each page; fixed positioned boxes are thus replicated on every
            //   page. (They are fixed with respect to the page box only, and are not affected by being seen through a
            //   viewport; as in the case of print preview, for example.)
        }
        m_containing_block = static_cast<Box*>(ancestor);
        return;
    }

    m_containing_block = nearest_ancestor_capable_of_forming_a_containing_block(*this);
}

// returns containing block this node would have had if its position was static
Box const* Node::static_position_containing_block() const
{
    return nearest_ancestor_capable_of_forming_a_containing_block(const_cast<Node&>(*this));
}

Box const* Node::non_anonymous_containing_block() const
{
    auto nearest_ancestor_box = containing_block();
    VERIFY(nearest_ancestor_box);
    while (nearest_ancestor_box->is_anonymous()) {
        nearest_ancestor_box = nearest_ancestor_box->containing_block();
        VERIFY(nearest_ancestor_box);
    }
    return nearest_ancestor_box;
}

// https://developer.mozilla.org/en-US/docs/Web/CSS/CSS_Positioning/Understanding_z_index/The_stacking_context
bool Node::establishes_stacking_context() const
{
    // NOTE: While MDN is not authoritative, there isn't a single convenient location
    //       in the CSS specifications where the rules for stacking contexts is described.
    //       That's why the "spec link" here points to MDN.

    if (!has_style())
        return false;

    if (is_svg_box())
        return false;

    // We make a stacking context for the viewport. Painting and hit testing starts from here.
    if (is_viewport())
        return true;

    // Root element of the document (<html>).
    if (is_root_element())
        return true;

    auto const& computed_values = this->computed_values();

    auto position = computed_values.position();

    // https://drafts.csswg.org/css-will-change/#will-change
    // If any non-initial value of a property would create a stacking context on the element, specifying that property
    // in will-change must create a stacking context on the element.
    auto will_change_property = [&](CSS::PropertyID property_id) {
        return computed_values.will_change().has_property(property_id);
    };

    auto has_z_index = computed_values.z_index().has_value() || will_change_property(CSS::PropertyID::ZIndex);

    // Element with a position value absolute or relative and z-index value other than auto.
    if (position == CSS::Positioning::Absolute || position == CSS::Positioning::Relative) {
        if (has_z_index) {
            return true;
        }
    }

    // Element with a position value fixed or sticky.
    if (position == CSS::Positioning::Fixed || position == CSS::Positioning::Sticky
        || will_change_property(CSS::PropertyID::Position)) {
        return true;
    }

    if (!computed_values.transformations().is_empty() || will_change_property(CSS::PropertyID::Transform))
        return true;

    if (computed_values.translate() || will_change_property(CSS::PropertyID::Translate))
        return true;

    if (computed_values.rotate() || will_change_property(CSS::PropertyID::Rotate))
        return true;

    if (computed_values.scale() || will_change_property(CSS::PropertyID::Scale))
        return true;

    // Element that is a child of a flex container, with z-index value other than auto.
    if (parent() && parent()->display().is_flex_inside() && has_z_index)
        return true;

    // Element that is a child of a grid container, with z-index value other than auto.
    if (parent() && parent()->display().is_grid_inside() && has_z_index)
        return true;

    // https://drafts.fxtf.org/filter-effects/#FilterProperty
    // https://drafts.fxtf.org/filter-effects-2/#backdrop-filter-operation
    // A computed value of other than none results in the creation of both a stacking context
    // [CSS21] and a Containing Block for absolute and fixed position descendants, unless the
    // element it applies to is a document root element in the current browsing context.
    // Spec Note: This rule works in the same way as for the filter property.
    if (computed_values.backdrop_filter().has_filters() || computed_values.filter().has_filters()
        || will_change_property(CSS::PropertyID::BackdropFilter)
        || will_change_property(CSS::PropertyID::Filter)) {
        return true;
    }

    // Element with any of the following properties with value other than none:
    // - transform
    // - filter
    // - backdrop-filter
    // - perspective
    // - clip-path
    // - mask / mask-image / mask-border
    if (computed_values.mask().has_value() || computed_values.clip_path().has_value() || computed_values.mask_image()
        || will_change_property(CSS::PropertyID::Mask)
        || will_change_property(CSS::PropertyID::ClipPath)
        || will_change_property(CSS::PropertyID::MaskImage)) {
        return true;
    }

    if (is_svg_foreign_object_box())
        return true;

    // https://drafts.fxtf.org/compositing/#propdef-isolation
    // For CSS, setting isolation to isolate will turn the element into a stacking context.
    if (computed_values.isolation() == CSS::Isolation::Isolate || will_change_property(CSS::PropertyID::Isolation))
        return true;

    // https://drafts.csswg.org/css-contain-2/#containment-types
    // 5. The layout containment box creates a stacking context.
    // 3. The paint containment box creates a stacking context.
    if (has_layout_containment() || has_paint_containment() || will_change_property(CSS::PropertyID::Contain))
        return true;

    // https://drafts.fxtf.org/compositing/#mix-blend-mode
    // Applying a blendmode other than normal to the element must establish a new stacking context.
    if (computed_values.mix_blend_mode() != CSS::MixBlendMode::Normal || will_change_property(CSS::PropertyID::MixBlendMode))
        return true;

    // https://drafts.csswg.org/css-view-transitions-1/#named-and-transitioning
    // Elements captured in a view transition during a view transition or whose view-transition-name computed value is
    // not 'none' (at any time):
    // - Form a stacking context.
    if (computed_values.view_transition_name().has_value() || will_change_property(CSS::PropertyID::ViewTransitionName))
        return true;

    // https://drafts.csswg.org/css-transforms-2/#propdef-perspective
    // The use of this property with any value other than 'none' establishes a stacking context.
    if (computed_values.perspective().has_value() || will_change_property(CSS::PropertyID::Perspective))
        return true;

    // https://drafts.csswg.org/css-transforms-2/#transform-style-property
    // A computed value of 'preserve-3d' for 'transform-style' on a transformable element establishes both a
    // stacking context and a containing block for all descendants.
    // FIXME: Check that the element is a transformable element.
    if (computed_values.transform_style() == CSS::TransformStyle::Preserve3d || will_change_property(CSS::PropertyID::TransformStyle))
        return true;

    return computed_values.opacity() < 1.0f || will_change_property(CSS::PropertyID::Opacity);
}

GC::Ptr<HTML::Navigable> Node::navigable() const
{
    return document().navigable();
}

Viewport const& Node::root() const
{
    // NB: Called during layout, which is in progress.
    VERIFY(document().unsafe_layout_node());
    return *document().unsafe_layout_node();
}

Viewport& Node::root()
{
    // NB: Called during layout, which is in progress.
    VERIFY(document().unsafe_layout_node());
    return *document().unsafe_layout_node();
}

bool Node::is_floating() const
{
    if (!has_style())
        return false;
    // flex-items don't float.
    if (is_flex_item())
        return false;
    return computed_values().float_() != CSS::Float::None;
}

bool Node::is_positioned() const
{
    return has_style() && computed_values().position() != CSS::Positioning::Static;
}

bool Node::is_absolutely_positioned() const
{
    if (!has_style())
        return false;
    auto position = computed_values().position();
    return position == CSS::Positioning::Absolute || position == CSS::Positioning::Fixed;
}

bool Node::is_fixed_position() const
{
    if (!has_style())
        return false;
    auto position = computed_values().position();
    return position == CSS::Positioning::Fixed;
}

bool Node::is_sticky_position() const
{
    if (!has_style())
        return false;
    auto position = computed_values().position();
    return position == CSS::Positioning::Sticky;
}

NodeWithStyle::NodeWithStyle(DOM::Document& document, DOM::Node* node, GC::Ref<CSS::ComputedProperties> computed_style)
    : Node(document, node)
    , m_computed_values(make<CSS::ComputedValues>())
{
    m_has_style = true;
    m_is_body = node && node == document.body();
    apply_style(computed_style);
}

NodeWithStyle::NodeWithStyle(DOM::Document& document, DOM::Node* node, NonnullOwnPtr<CSS::ComputedValues> computed_values)
    : Node(document, node)
    , m_computed_values(move(computed_values))
{
    m_has_style = true;
    m_is_body = node && node == document.body();
}

void NodeWithStyle::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto const& layer : computed_values().background_layers())
        layer.background_image->visit_edges(visitor);

    if (m_list_style_image && m_list_style_image->is_image())
        m_list_style_image->as_image().visit_edges(visitor);

    m_computed_values->visit_edges(visitor);
}

void NodeWithStyle::apply_style(CSS::ComputedProperties const& computed_style)
{
    auto& computed_values = mutable_computed_values();

    // NOTE: color-scheme must be set first to ensure system colors can be resolved correctly.
    auto color_scheme = computed_style.color_scheme(document().page().preferred_color_scheme(), document().supported_color_schemes());
    computed_values.set_color_scheme(color_scheme);

    // NOTE: We have to be careful that font-related properties get set in the right order.
    //       m_font is used by Length::to_px() when resolving sizes against this layout node.
    //       That's why it has to be set before everything else.
    computed_values.set_font_list(computed_style.computed_font_list(document().font_computer()));
    computed_values.set_font_size(computed_style.font_size());
    computed_values.set_font_weight(computed_style.font_weight());
    computed_values.set_line_height(computed_style.line_height());

    // NOTE: color must be set after color-scheme to ensure currentColor can be resolved in other properties (e.g. background-color).
    // NOTE: color must be set after font_size as `CalculatedStyleValue`s can rely on it being set for resolving lengths.
    computed_values.set_color(computed_style.color_or_fallback(CSS::PropertyID::Color, CSS::ColorResolutionContext::for_layout_node_with_style(*this), CSS::InitialValues::color()));
    // NOTE: Currently there are still discussions about `accentColor` and `currentColor` interactions, so the line below might need changing in the future
    computed_values.set_accent_color(computed_style.color_or_fallback(CSS::PropertyID::AccentColor, CSS::ColorResolutionContext::for_layout_node_with_style(*this), CSS::SystemColor::accent_color(color_scheme)));
    // NOTE: This color resolution context must be created after we set color above so that currentColor resolves correctly
    // FIXME: We should resolve colors to their absolute forms at compute time (i.e. by implementing the relevant absolutized methods)
    auto color_resolution_context = CSS::ColorResolutionContext::for_layout_node_with_style(*this);

    computed_values.set_vertical_align(computed_style.vertical_align());

    auto background_layers = computed_style.background_layers();

    for (auto const& layer : background_layers)
        const_cast<CSS::AbstractImageStyleValue&>(*layer.background_image).load_any_resources(document());

    computed_values.set_background_layers(move(background_layers));

    computed_values.set_background_color(computed_style.color_or_fallback(CSS::PropertyID::BackgroundColor, color_resolution_context, CSS::InitialValues::background_color()));
    computed_values.set_background_color_clip(computed_style.background_color_clip());

    computed_values.set_box_sizing(computed_style.box_sizing());

    if (auto maybe_font_language_override = computed_style.font_language_override(); maybe_font_language_override.has_value())
        computed_values.set_font_language_override(maybe_font_language_override.release_value());
    computed_values.set_font_variation_settings(computed_style.font_variation_settings());

    auto border_radius_data_from_style_value = [](CSS::StyleValue const& value) -> CSS::BorderRadiusData {
        return CSS::BorderRadiusData {
            CSS::LengthPercentage::from_style_value(value.as_border_radius().horizontal_radius()),
            CSS::LengthPercentage::from_style_value(value.as_border_radius().vertical_radius())
        };
    };

    computed_values.set_border_bottom_left_radius(border_radius_data_from_style_value(computed_style.property(CSS::PropertyID::BorderBottomLeftRadius)));
    computed_values.set_border_bottom_right_radius(border_radius_data_from_style_value(computed_style.property(CSS::PropertyID::BorderBottomRightRadius)));
    computed_values.set_border_top_left_radius(border_radius_data_from_style_value(computed_style.property(CSS::PropertyID::BorderTopLeftRadius)));
    computed_values.set_border_top_right_radius(border_radius_data_from_style_value(computed_style.property(CSS::PropertyID::BorderTopRightRadius)));
    computed_values.set_display(computed_style.display());
    computed_values.set_display_before_box_type_transformation(computed_style.display_before_box_type_transformation());

    computed_values.set_flex_direction(computed_style.flex_direction());
    computed_values.set_flex_wrap(computed_style.flex_wrap());
    computed_values.set_flex_basis(computed_style.flex_basis());
    computed_values.set_flex_grow(computed_style.flex_grow());
    computed_values.set_flex_shrink(computed_style.flex_shrink());
    computed_values.set_order(computed_style.order());
    computed_values.set_clip(computed_style.clip());

    computed_values.set_backdrop_filter(computed_style.backdrop_filter());
    computed_values.set_filter(computed_style.filter());

    computed_values.set_flood_color(computed_style.color_or_fallback(CSS::PropertyID::FloodColor, color_resolution_context, CSS::InitialValues::flood_color()));
    computed_values.set_flood_opacity(computed_style.flood_opacity());

    computed_values.set_justify_content(computed_style.justify_content());
    computed_values.set_justify_items(computed_style.justify_items());
    computed_values.set_justify_self(computed_style.justify_self());

    auto accent_color = computed_style.accent_color(*this);
    if (accent_color.has_value())
        computed_values.set_accent_color(accent_color.value());

    computed_values.set_align_content(computed_style.align_content());
    computed_values.set_align_items(computed_style.align_items());
    computed_values.set_align_self(computed_style.align_self());

    computed_values.set_appearance(computed_style.appearance());

    computed_values.set_position(computed_style.position());

    computed_values.set_text_align(computed_style.text_align());
    computed_values.set_text_justify(computed_style.text_justify());
    computed_values.set_text_overflow(computed_style.text_overflow());
    computed_values.set_text_underline_offset(computed_style.text_underline_offset());
    computed_values.set_text_underline_position(computed_style.text_underline_position());

    computed_values.set_text_indent(computed_style.text_indent());
    computed_values.set_text_wrap_mode(computed_style.text_wrap_mode());
    computed_values.set_tab_size(computed_style.tab_size());

    computed_values.set_white_space_collapse(computed_style.white_space_collapse());
    computed_values.set_word_break(computed_style.word_break());

    computed_values.set_word_spacing(computed_style.word_spacing());
    computed_values.set_letter_spacing(computed_style.letter_spacing());

    computed_values.set_float(computed_style.float_());

    computed_values.set_border_spacing_horizontal(computed_style.border_spacing_horizontal());
    computed_values.set_border_spacing_vertical(computed_style.border_spacing_vertical());

    computed_values.set_caption_side(computed_style.caption_side());
    computed_values.set_clear(computed_style.clear());
    computed_values.set_overflow_x(computed_style.overflow_x());
    computed_values.set_overflow_y(computed_style.overflow_y());
    computed_values.set_content_visibility(computed_style.content_visibility());
    computed_values.set_cursor(computed_style.cursor());
    computed_values.set_image_rendering(computed_style.image_rendering());
    computed_values.set_pointer_events(computed_style.pointer_events());
    computed_values.set_text_decoration_line(computed_style.text_decoration_line());
    computed_values.set_text_decoration_style(computed_style.text_decoration_style());
    computed_values.set_text_transform(computed_style.text_transform());

    computed_values.set_list_style_type(computed_style.list_style_type(m_dom_node->document().registered_counter_styles()));
    computed_values.set_list_style_position(computed_style.list_style_position());
    auto const& list_style_image = computed_style.property(CSS::PropertyID::ListStyleImage);
    if (list_style_image.is_abstract_image()) {
        m_list_style_image = list_style_image.as_abstract_image();
        const_cast<CSS::AbstractImageStyleValue&>(*m_list_style_image).load_any_resources(document());
    }

    // FIXME: The default text decoration color value is `currentcolor`, but since we can't resolve that easily,
    //        we just manually grab the value from `color`. This makes it dependent on `color` being
    //        specified first, so it's far from ideal.
    computed_values.set_text_decoration_color(computed_style.color_or_fallback(CSS::PropertyID::TextDecorationColor, color_resolution_context, computed_values.color()));
    computed_values.set_text_decoration_thickness(computed_style.text_decoration_thickness());

    computed_values.set_webkit_text_fill_color(computed_style.color_or_fallback(CSS::PropertyID::WebkitTextFillColor, color_resolution_context, computed_values.color()));

    computed_values.set_text_shadow(computed_style.text_shadow(*this));

    computed_values.set_z_index(computed_style.z_index());
    computed_values.set_opacity(computed_style.opacity());

    computed_values.set_visibility(computed_style.visibility());

    computed_values.set_width(computed_style.size_value(CSS::PropertyID::Width));
    computed_values.set_min_width(computed_style.size_value(CSS::PropertyID::MinWidth));
    computed_values.set_max_width(computed_style.size_value(CSS::PropertyID::MaxWidth));

    computed_values.set_height(computed_style.size_value(CSS::PropertyID::Height));
    computed_values.set_min_height(computed_style.size_value(CSS::PropertyID::MinHeight));
    computed_values.set_max_height(computed_style.size_value(CSS::PropertyID::MaxHeight));

    computed_values.set_inset(computed_style.length_box(CSS::PropertyID::Left, CSS::PropertyID::Top, CSS::PropertyID::Right, CSS::PropertyID::Bottom, CSS::LengthPercentageOrAuto::make_auto()));
    computed_values.set_margin(computed_style.length_box(CSS::PropertyID::MarginLeft, CSS::PropertyID::MarginTop, CSS::PropertyID::MarginRight, CSS::PropertyID::MarginBottom, CSS::Length::make_px(0)));
    computed_values.set_padding(computed_style.length_box(CSS::PropertyID::PaddingLeft, CSS::PropertyID::PaddingTop, CSS::PropertyID::PaddingRight, CSS::PropertyID::PaddingBottom, CSS::Length::make_px(0)));
    computed_values.set_overflow_clip_margin(computed_style.length_box(CSS::PropertyID::OverflowClipMarginLeft, CSS::PropertyID::OverflowClipMarginTop, CSS::PropertyID::OverflowClipMarginRight, CSS::PropertyID::OverflowClipMarginBottom, CSS::Length::make_px(0)));

    computed_values.set_box_shadow(computed_style.box_shadow(*this));

    if (auto rotate_value = computed_style.rotate())
        computed_values.set_rotate(rotate_value.release_nonnull());

    if (auto translate_value = computed_style.translate())
        computed_values.set_translate(translate_value.release_nonnull());

    if (auto scale_value = computed_style.scale())
        computed_values.set_scale(scale_value.release_nonnull());

    computed_values.set_transformations(computed_style.transformations());
    computed_values.set_transform_box(computed_style.transform_box());
    computed_values.set_transform_origin(computed_style.transform_origin());
    computed_values.set_transform_style(computed_style.transform_style());
    computed_values.set_perspective(computed_style.perspective());
    computed_values.set_perspective_origin(computed_style.perspective_origin());

    auto const& transition_delay_property = computed_style.property(CSS::PropertyID::TransitionDelay);
    if (transition_delay_property.is_time()) {
        auto const& transition_delay = transition_delay_property.as_time();
        computed_values.set_transition_delay(transition_delay.time());
    } else if (transition_delay_property.is_calculated()) {
        auto const& transition_delay = transition_delay_property.as_calculated();
        computed_values.set_transition_delay(transition_delay.resolve_time({ .length_resolution_context = CSS::Length::ResolutionContext::for_layout_node(*this) }).value());
    }

    auto do_border_style = [&](CSS::BorderData& border, CSS::PropertyID width_property, CSS::PropertyID color_property, CSS::PropertyID style_property) {
        // FIXME: The default border color value is `currentcolor`, but since we can't resolve that easily,
        //        we just manually grab the value from `color`. This makes it dependent on `color` being
        //        specified first, so it's far from ideal.
        border.color = computed_style.color_or_fallback(color_property, color_resolution_context, computed_values.color());
        border.line_style = computed_style.line_style(style_property);

        // If the border-style corresponding to a given border-width is none or hidden, then the used width is 0.
        // https://drafts.csswg.org/css-backgrounds/#border-width
        if (border.line_style == CSS::LineStyle::None || border.line_style == CSS::LineStyle::Hidden) {
            border.width = 0;
        } else {
            // FIXME: Interpolation can cause negative values - we clamp here but should instead clamp as part of interpolation
            border.width = max(CSSPixels { 0 }, computed_style.length(width_property).absolute_length_to_px());
        }
    };

    do_border_style(computed_values.border_left(), CSS::PropertyID::BorderLeftWidth, CSS::PropertyID::BorderLeftColor, CSS::PropertyID::BorderLeftStyle);
    do_border_style(computed_values.border_top(), CSS::PropertyID::BorderTopWidth, CSS::PropertyID::BorderTopColor, CSS::PropertyID::BorderTopStyle);
    do_border_style(computed_values.border_right(), CSS::PropertyID::BorderRightWidth, CSS::PropertyID::BorderRightColor, CSS::PropertyID::BorderRightStyle);
    do_border_style(computed_values.border_bottom(), CSS::PropertyID::BorderBottomWidth, CSS::PropertyID::BorderBottomColor, CSS::PropertyID::BorderBottomStyle);

    if (auto const& outline_color = computed_style.property(CSS::PropertyID::OutlineColor); outline_color.has_color())
        computed_values.set_outline_color(outline_color.to_color(color_resolution_context).value());
    // FIXME: Support calc()
    if (auto const& outline_offset = computed_style.property(CSS::PropertyID::OutlineOffset); outline_offset.is_length())
        computed_values.set_outline_offset(outline_offset.as_length().length());
    computed_values.set_outline_style(computed_style.outline_style());

    // FIXME: Interpolation can cause negative values - we clamp here but should instead clamp as part of interpolation.
    computed_values.set_outline_width(max(CSSPixels { 0 }, computed_style.length(CSS::PropertyID::OutlineWidth).absolute_length_to_px()));

    computed_values.set_grid_auto_columns(computed_style.grid_auto_columns());
    computed_values.set_grid_auto_rows(computed_style.grid_auto_rows());
    computed_values.set_grid_template_columns(computed_style.grid_template_columns());
    computed_values.set_grid_template_rows(computed_style.grid_template_rows());
    computed_values.set_grid_column_end(computed_style.grid_column_end());
    computed_values.set_grid_column_start(computed_style.grid_column_start());
    computed_values.set_grid_row_end(computed_style.grid_row_end());
    computed_values.set_grid_row_start(computed_style.grid_row_start());
    computed_values.set_grid_template_areas(computed_style.grid_template_areas());
    computed_values.set_grid_auto_flow(computed_style.grid_auto_flow());

    computed_values.set_cx(CSS::LengthPercentage::from_style_value(computed_style.property(CSS::PropertyID::Cx)));
    computed_values.set_cy(CSS::LengthPercentage::from_style_value(computed_style.property(CSS::PropertyID::Cy)));
    computed_values.set_r(CSS::LengthPercentage::from_style_value(computed_style.property(CSS::PropertyID::R)));
    computed_values.set_rx(CSS::LengthPercentageOrAuto::from_style_value(computed_style.property(CSS::PropertyID::Rx)));
    computed_values.set_ry(CSS::LengthPercentageOrAuto::from_style_value(computed_style.property(CSS::PropertyID::Ry)));
    computed_values.set_x(CSS::LengthPercentage::from_style_value(computed_style.property(CSS::PropertyID::X)));
    computed_values.set_y(CSS::LengthPercentage::from_style_value(computed_style.property(CSS::PropertyID::Y)));

    auto extract_paint_fallback_color = [&](CSS::URLStyleValue const& url_value) -> Optional<Color> {
        if (auto const& fallback = url_value.paint_fallback()) {
            if (fallback->has_color())
                return fallback->to_color(color_resolution_context);
        }
        return {};
    };

    auto const& fill = computed_style.property(CSS::PropertyID::Fill);
    if (fill.has_color())
        computed_values.set_fill(fill.to_color(color_resolution_context).value());
    else if (fill.is_url())
        computed_values.set_fill(CSS::SVGPaint(fill.as_url().url(), extract_paint_fallback_color(fill.as_url())));
    auto const& stroke = computed_style.property(CSS::PropertyID::Stroke);
    if (stroke.has_color())
        computed_values.set_stroke(stroke.to_color(color_resolution_context).value());
    else if (stroke.is_url())
        computed_values.set_stroke(CSS::SVGPaint(stroke.as_url().url(), extract_paint_fallback_color(stroke.as_url())));

    computed_values.set_stop_color(computed_style.color_or_fallback(CSS::PropertyID::StopColor, color_resolution_context, CSS::InitialValues::stop_color()));

    auto const& stroke_width = computed_style.property(CSS::PropertyID::StrokeWidth);
    // FIXME: Converting to pixels isn't really correct - values should be in "user units"
    //        https://svgwg.org/svg2-draft/coords.html#TermUserUnits
    // FIXME: Support calc()
    if (stroke_width.is_number())
        computed_values.set_stroke_width(CSS::Length::make_px(CSSPixels::nearest_value_for(stroke_width.as_number().number())));
    else if (stroke_width.is_length())
        computed_values.set_stroke_width(stroke_width.as_length().length());
    else if (stroke_width.is_percentage())
        computed_values.set_stroke_width(CSS::LengthPercentage { stroke_width.as_percentage().percentage() });
    computed_values.set_shape_rendering(computed_style.shape_rendering());
    computed_values.set_paint_order(computed_style.paint_order());

    // FIXME: We should actually support more than one mask image rather than just using the first
    auto const& mask_image = [&] -> CSS::StyleValue const& {
        auto const& value = computed_style.property(CSS::PropertyID::MaskImage);

        if (value.is_value_list())
            return value.as_value_list().values()[0];

        return value;
    }();
    if (mask_image.is_url()) {
        computed_values.set_mask(mask_image.as_url().url());
    } else if (mask_image.is_abstract_image()) {
        auto const& abstract_image = mask_image.as_abstract_image();
        computed_values.set_mask_image(abstract_image);
        const_cast<CSS::AbstractImageStyleValue&>(abstract_image).load_any_resources(document());
    }

    computed_values.set_mask_type(computed_style.mask_type());

    auto const& clip_path = computed_style.property(CSS::PropertyID::ClipPath);
    if (clip_path.is_url())
        computed_values.set_clip_path(clip_path.as_url().url());
    else if (clip_path.is_basic_shape())
        computed_values.set_clip_path(clip_path.as_basic_shape());
    computed_values.set_clip_rule(computed_style.clip_rule());
    computed_values.set_fill_rule(computed_style.fill_rule());

    computed_values.set_fill_opacity(computed_style.fill_opacity());
    computed_values.set_stroke_dasharray(computed_style.stroke_dasharray());

    auto const& stroke_dashoffset = computed_style.property(CSS::PropertyID::StrokeDashoffset);
    // FIXME: Converting to pixels isn't really correct - values should be in "user units"
    //        https://svgwg.org/svg2-draft/coords.html#TermUserUnits
    // FIXME: Support calc()
    if (stroke_dashoffset.is_number())
        computed_values.set_stroke_dashoffset(CSS::Length::make_px(CSSPixels::nearest_value_for(stroke_dashoffset.as_number().number())));
    else if (stroke_dashoffset.is_length())
        computed_values.set_stroke_dashoffset(stroke_dashoffset.as_length().length());
    else if (stroke_dashoffset.is_percentage())
        computed_values.set_stroke_dashoffset(CSS::LengthPercentage { stroke_dashoffset.as_percentage().percentage() });

    computed_values.set_stroke_linecap(computed_style.stroke_linecap());
    computed_values.set_stroke_linejoin(computed_style.stroke_linejoin());
    computed_values.set_stroke_miterlimit(computed_style.stroke_miterlimit());

    computed_values.set_stroke_opacity(computed_style.stroke_opacity());
    computed_values.set_stop_opacity(computed_style.stop_opacity());

    computed_values.set_text_anchor(computed_style.text_anchor());
    computed_values.set_dominant_baseline(computed_style.dominant_baseline());

    // FIXME: Support calc()
    if (auto const& column_count = computed_style.property(CSS::PropertyID::ColumnCount); column_count.is_integer())
        computed_values.set_column_count(CSS::ColumnCount::make_integer(column_count.as_integer().integer()));

    computed_values.set_column_span(computed_style.column_span());

    computed_values.set_column_width(computed_style.size_value(CSS::PropertyID::ColumnWidth));
    computed_values.set_column_height(computed_style.size_value(CSS::PropertyID::ColumnHeight));

    computed_values.set_column_gap(computed_style.gap_value(CSS::PropertyID::ColumnGap));
    computed_values.set_row_gap(computed_style.gap_value(CSS::PropertyID::RowGap));

    computed_values.set_border_collapse(computed_style.border_collapse());

    computed_values.set_empty_cells(computed_style.empty_cells());

    computed_values.set_table_layout(computed_style.table_layout());

    auto const& aspect_ratio = computed_style.property(CSS::PropertyID::AspectRatio);
    if (aspect_ratio.is_value_list()) {
        auto const& values_list = aspect_ratio.as_value_list().values();
        if (values_list.size() == 2
            && values_list[0]->is_keyword() && values_list[0]->as_keyword().keyword() == CSS::Keyword::Auto
            && values_list[1]->is_ratio()) {
            computed_values.set_aspect_ratio({ true, values_list[1]->as_ratio().ratio() });
        }
    } else if (aspect_ratio.is_keyword() && aspect_ratio.as_keyword().keyword() == CSS::Keyword::Auto) {
        computed_values.set_aspect_ratio({ true, {} });
    } else if (aspect_ratio.is_ratio()) {
        // https://drafts.csswg.org/css-sizing-4/#aspect-ratio
        // If the <ratio> is degenerate, the property instead behaves as auto.
        if (aspect_ratio.as_ratio().ratio().is_degenerate())
            computed_values.set_aspect_ratio({ true, {} });
        else
            computed_values.set_aspect_ratio({ false, aspect_ratio.as_ratio().ratio() });
    }

    computed_values.set_touch_action(computed_style.touch_action());

    auto const& math_shift_value = computed_style.property(CSS::PropertyID::MathShift);
    if (auto math_shift = keyword_to_math_shift(math_shift_value.to_keyword()); math_shift.has_value())
        computed_values.set_math_shift(math_shift.value());

    auto const& math_style_value = computed_style.property(CSS::PropertyID::MathStyle);
    if (auto math_style = keyword_to_math_style(math_style_value.to_keyword()); math_style.has_value())
        computed_values.set_math_style(math_style.value());

    computed_values.set_math_depth(computed_style.math_depth());
    computed_values.set_quotes(computed_style.quotes());
    computed_values.set_counter_increment(computed_style.counter_data(CSS::PropertyID::CounterIncrement));
    computed_values.set_counter_reset(computed_style.counter_data(CSS::PropertyID::CounterReset));
    computed_values.set_counter_set(computed_style.counter_data(CSS::PropertyID::CounterSet));

    computed_values.set_object_fit(computed_style.object_fit());
    computed_values.set_object_position(computed_style.object_position());
    computed_values.set_direction(computed_style.direction());
    computed_values.set_unicode_bidi(computed_style.unicode_bidi());
    computed_values.set_scrollbar_color(computed_style.scrollbar_color(*this));
    computed_values.set_scrollbar_width(computed_style.scrollbar_width());
    computed_values.set_writing_mode(computed_style.writing_mode());
    computed_values.set_user_select(computed_style.user_select());
    computed_values.set_isolation(computed_style.isolation());
    computed_values.set_mix_blend_mode(computed_style.mix_blend_mode());
    computed_values.set_view_transition_name(computed_style.view_transition_name());
    computed_values.set_contain(computed_style.contain());
    computed_values.set_container_type(computed_style.container_type());
    computed_values.set_shape_rendering(computed_values.shape_rendering());
    computed_values.set_will_change(computed_style.will_change());

    computed_values.set_caret_color(computed_style.caret_color(*this));
    computed_values.set_color_interpolation(computed_style.color_interpolation());
    computed_values.set_resize(computed_style.resize());

    propagate_style_to_anonymous_wrappers();

    if (auto* box_node = as_if<NodeWithStyleAndBoxModelMetrics>(*this))
        box_node->propagate_style_along_continuation(computed_style);
}

void NodeWithStyle::propagate_non_inherit_values(NodeWithStyle& target_node) const
{
    // NOTE: These properties are not inherited, but we still have to propagate them to anonymous wrappers.
    target_node.mutable_computed_values().set_text_decoration_line(computed_values().text_decoration_line());
    target_node.mutable_computed_values().set_text_decoration_thickness(computed_values().text_decoration_thickness());
    target_node.mutable_computed_values().set_text_decoration_color(computed_values().text_decoration_color());
    target_node.mutable_computed_values().set_text_decoration_style(computed_values().text_decoration_style());
}

void NodeWithStyle::propagate_style_to_anonymous_wrappers()
{
    // Update the style of any anonymous wrappers that inherit from this node.
    // FIXME: This is pretty hackish. It would be nicer if they shared the inherited style
    //        data structure somehow, so this wasn't necessary.

    // If this is a `display:table` box with an anonymous wrapper parent,
    // the parent inherits style from *this* node, not the other way around.
    if (auto* table_wrapper = as_if<TableWrapper>(parent()); table_wrapper && display().is_table_inside()) {
        static_cast<CSS::MutableComputedValues&>(static_cast<CSS::ComputedValues&>(const_cast<CSS::ImmutableComputedValues&>(table_wrapper->computed_values()))).inherit_from(computed_values());
        transfer_table_box_computed_values_to_wrapper_computed_values(table_wrapper->mutable_computed_values());
    }

    // Propagate style to all anonymous children (except table wrappers!)
    for_each_child_of_type<NodeWithStyle>([&](NodeWithStyle& child) {
        if (child.is_anonymous() && !is<TableWrapper>(child)) {
            auto& child_computed_values = static_cast<CSS::MutableComputedValues&>(static_cast<CSS::ComputedValues&>(const_cast<CSS::ImmutableComputedValues&>(child.computed_values())));
            child_computed_values.inherit_from(computed_values());
            propagate_non_inherit_values(child);
            child.propagate_style_to_anonymous_wrappers();
        }
        return IterationDecision::Continue;
    });
}

bool Node::is_root_element() const
{
    if (is_anonymous())
        return false;
    return is<HTML::HTMLHtmlElement>(*dom_node());
}

String Node::debug_description() const
{
    StringBuilder builder;
    builder.append(class_name());
    if (dom_node()) {
        builder.appendff("<{}>", dom_node()->node_name());
        if (dom_node()->is_element()) {
            auto& element = static_cast<DOM::Element const&>(*dom_node());
            if (element.id().has_value())
                builder.appendff("#{}", element.id().value());
            for (auto const& class_name : element.class_names())
                builder.appendff(".{}", class_name);
        }
    } else {
        builder.append("(anonymous)"sv);
    }
    return MUST(builder.to_string());
}

CSS::Display Node::display() const
{
    if (!has_style()) {
        // NOTE: No style means this is dumb text content.
        return CSS::Display(CSS::DisplayOutside::Inline, CSS::DisplayInside::Flow);
    }

    return computed_values().display();
}

CSS::Display Node::display_before_box_type_transformation() const
{
    if (!has_style()) {
        return CSS::Display(CSS::DisplayOutside::Inline, CSS::DisplayInside::Flow);
    }

    return computed_values().display_before_box_type_transformation();
}

bool Node::is_inline() const
{
    return display().is_inline_outside();
}

bool Node::is_inline_block() const
{
    auto display = this->display();
    return display.is_inline_outside() && display.is_flow_root_inside();
}

bool Node::is_inline_table() const
{
    auto display = this->display();
    return display.is_inline_outside() && display.is_table_inside();
}

bool Node::is_atomic_inline() const
{
    if (is_replaced_box())
        return true;
    auto display = this->display();
    return display.is_inline_outside() && !display.is_flow_inside();
}

GC::Ref<NodeWithStyle> NodeWithStyle::create_anonymous_wrapper() const
{
    auto wrapper = heap().allocate<BlockContainer>(const_cast<DOM::Document&>(document()), nullptr, computed_values().clone_inherited_values());
    wrapper->mutable_computed_values().set_display(CSS::Display(CSS::DisplayOutside::Block, CSS::DisplayInside::Flow));
    propagate_non_inherit_values(*wrapper);
    // CSS 2.2 9.2.1.1 creates anonymous block boxes, but 9.4.1 states inline-block creates a BFC.
    // Set wrapper to inline-block to participate correctly in the IFC within the parent inline-block.
    if (display().is_inline_block() && !has_children()) {
        wrapper->mutable_computed_values().set_display(CSS::Display::from_short(CSS::Display::Short::InlineBlock));
    }
    return *wrapper;
}

void NodeWithStyle::set_computed_values(NonnullOwnPtr<CSS::ComputedValues> computed_values)
{
    m_computed_values = move(computed_values);
}

void NodeWithStyle::reset_table_box_computed_values_used_by_wrapper_to_init_values()
{
    VERIFY(this->display().is_table_inside());

    auto& mutable_computed_values = this->mutable_computed_values();
    mutable_computed_values.set_position(CSS::InitialValues::position());
    mutable_computed_values.set_float(CSS::InitialValues::float_());
    mutable_computed_values.set_clear(CSS::InitialValues::clear());
    mutable_computed_values.set_inset(CSS::InitialValues::inset());
    mutable_computed_values.set_margin(CSS::InitialValues::margin());
    // AD-HOC:
    // To match other browsers, z-index needs to be moved to the wrapper box as well,
    // even if the spec does not mention that: https://github.com/w3c/csswg-drafts/issues/11689
    // Note that there may be more properties that need to be added to this list.
    mutable_computed_values.set_z_index(CSS::InitialValues::z_index());
}

void NodeWithStyle::transfer_table_box_computed_values_to_wrapper_computed_values(CSS::ComputedValues& wrapper_computed_values)
{
    // The computed values of properties 'position', 'float', 'margin-*', 'top', 'right', 'bottom', and 'left' on the table element are used on the table wrapper box and not the table box;
    // all other values of non-inheritable properties are used on the table box and not the table wrapper box.
    // (Where the table element's values are not used on the table and table wrapper boxes, the initial values are used instead.)
    auto& mutable_wrapper_computed_values = static_cast<CSS::MutableComputedValues&>(wrapper_computed_values);
    if (display().is_inline_outside())
        mutable_wrapper_computed_values.set_display(CSS::Display::from_short(CSS::Display::Short::InlineBlock));
    else
        mutable_wrapper_computed_values.set_display(CSS::Display::from_short(CSS::Display::Short::FlowRoot));
    mutable_wrapper_computed_values.set_position(computed_values().position());
    mutable_wrapper_computed_values.set_inset(computed_values().inset());
    mutable_wrapper_computed_values.set_float(computed_values().float_());
    mutable_wrapper_computed_values.set_clear(computed_values().clear());
    mutable_wrapper_computed_values.set_margin(computed_values().margin());
    // AD-HOC:
    // To match other browsers, z-index needs to be moved to the wrapper box as well,
    // even if the spec does not mention that: https://github.com/w3c/csswg-drafts/issues/11689
    // Note that there may be more properties that need to be added to this list.
    mutable_wrapper_computed_values.set_z_index(computed_values().z_index());

    reset_table_box_computed_values_used_by_wrapper_to_init_values();
}

bool overflow_value_makes_box_a_scroll_container(CSS::Overflow overflow)
{
    switch (overflow) {
    case CSS::Overflow::Clip:
    case CSS::Overflow::Visible:
        return false;
    case CSS::Overflow::Auto:
    case CSS::Overflow::Hidden:
    case CSS::Overflow::Scroll:
        return true;
    }
    VERIFY_NOT_REACHED();
}

bool NodeWithStyle::is_scroll_container() const
{
    // NOTE: This isn't in the spec, but we want the viewport to behave like a scroll container.
    if (is_viewport())
        return true;

    return overflow_value_makes_box_a_scroll_container(computed_values().overflow_x())
        || overflow_value_makes_box_a_scroll_container(computed_values().overflow_y());
}

void Node::add_paintable(GC::Ptr<Painting::Paintable> paintable)
{
    if (!paintable)
        return;
    m_paintable.append(*paintable);
}

void Node::clear_paintables()
{
    m_paintable.clear();
}

GC::Ptr<Painting::Paintable> Node::create_paintable() const
{
    return nullptr;
}

bool Node::is_anonymous() const
{
    return m_anonymous;
}

DOM::Node const* Node::dom_node() const
{
    if (m_anonymous)
        return nullptr;
    return m_dom_node.ptr();
}

DOM::Node* Node::dom_node()
{
    if (m_anonymous)
        return nullptr;
    return m_dom_node.ptr();
}

DOM::Element const* Node::pseudo_element_generator() const
{
    VERIFY(m_generated_for.has_value());
    return m_pseudo_element_generator.ptr();
}

DOM::Element* Node::pseudo_element_generator()
{
    VERIFY(m_generated_for.has_value());
    return m_pseudo_element_generator.ptr();
}

DOM::Document& Node::document()
{
    return m_dom_node->document();
}

DOM::Document const& Node::document() const
{
    return m_dom_node->document();
}

// https://drafts.csswg.org/css-ui/#propdef-user-select
CSS::UserSelect Node::user_select_used_value() const
{
    // The used value is the same as the computed value, except:
    auto computed_value = computed_values().user_select();

    // 1. on editable elements where the used value is always 'contain' regardless of the computed value

    // 2. when the computed value is 'auto', in which case the used value is one of the other values as defined below

    // For the purpose of this specification, an editable element is either an editing host or a mutable form control with
    // textual content, such as textarea.
    auto* form_control = as_if<HTML::FormAssociatedTextControlElement>(dom_node());
    // FIXME: Check if this needs to exclude input elements with types such as color or range, and if so, which ones exactly.
    if ((dom_node() && dom_node()->is_editing_host()) || (form_control && form_control->is_mutable())) {
        return CSS::UserSelect::Contain;
    } else if (computed_value == CSS::UserSelect::Auto) {
        // The used value of 'auto' is determined as follows:
        // - On the '::before' and '::after' pseudo-elements, the used value is 'none'
        if (is_generated_for_before_pseudo_element() || is_generated_for_after_pseudo_element()) {
            return CSS::UserSelect::None;
        }

        // - If the element is an editable element, the used value is 'contain'
        // NOTE: We already handled this above.

        auto parent_element = parent();
        if (parent_element) {
            auto parent_used_value = parent_element->user_select_used_value();

            // - Otherwise, if the used value of user-select on the parent of this element is 'all', the used value is 'all'
            if (parent_used_value == CSS::UserSelect::All) {
                return CSS::UserSelect::All;
            }

            // - Otherwise, if the used value of user-select on the parent of this element is 'none', the used value is
            //   'none'
            if (parent_used_value == CSS::UserSelect::None) {
                return CSS::UserSelect::None;
            }
        }

        // - Otherwise, the used value is 'text'
        return CSS::UserSelect::Text;
    }

    return computed_value;
}

// https://drafts.csswg.org/css-contain-2/#containment-size
bool Node::has_size_containment() const
{
    // However, giving an element size containment has no effect if any of the following are true:

    // - if the element does not generate a principal box (as is the case with 'display: contents' or 'display: none')
    // Note: This is the principal box

    // - if its inner display type is 'table'
    if (display().is_table_inside())
        return false;

    // - if its principal box is an internal table box
    if (display().is_internal_table())
        return false;

    // - if its principal box is an internal ruby box or a non-atomic inline-level box
    // FIXME: Implement this.

    if (computed_values().contain().size_containment)
        return true;

    if (computed_values().container_type().is_size_container)
        return true;

    return false;
}
// https://drafts.csswg.org/css-contain-2/#containment-inline-size
bool Node::has_inline_size_containment() const
{
    // Giving an element inline-size containment has no effect if any of the following are true:

    // - if the element does not generate a principal box (as is the case with 'display: contents' or 'display: none')
    // Note: This is the principal box

    // - if its inner display type is 'table'
    if (display().is_table_inside())
        return false;

    // - if its principal box is an internal table box
    if (display().is_internal_table())
        return false;

    // - if its principal box is an internal ruby box or a non-atomic inline-level box
    // FIXME: Implement this.

    if (computed_values().contain().inline_size_containment)
        return true;

    if (computed_values().container_type().is_inline_size_container)
        return true;

    return false;
}
// https://drafts.csswg.org/css-contain-2/#containment-layout
bool Node::has_layout_containment() const
{
    // However, giving an element layout containment has no effect if any of the following are true:

    // - if the element does not generate a principal box (as is the case with 'display: contents' or 'display: none')
    // Note: This is the principal box

    // - if its principal box is an internal table box other than 'table-cell'
    if (display().is_internal_table() && !display().is_table_cell())
        return false;

    // - if its principal box is an internal ruby box or a non-atomic inline-level box
    // FIXME: Implement this.

    if (computed_values().contain().layout_containment)
        return true;

    // https://drafts.csswg.org/css-contain-2/#valdef-content-visibility-auto
    // Changes the used value of the 'contain' property so as to turn on layout containment, style containment, and
    // paint containment for the element.
    if (computed_values().content_visibility() == CSS::ContentVisibility::Auto)
        return true;

    return false;
}
// https://drafts.csswg.org/css-contain-2/#containment-style
bool Node::has_style_containment() const
{
    // However, giving an element style containment has no effect if any of the following are true:

    // - if the element does not generate a principal box (as is the case with 'display: contents' or 'display: none')
    // Note: This is the principal box

    if (computed_values().contain().style_containment)
        return true;

    if (computed_values().container_type().is_size_container || computed_values().container_type().is_inline_size_container)
        return true;

    // https://drafts.csswg.org/css-contain-2/#valdef-content-visibility-auto
    // Changes the used value of the 'contain' property so as to turn on layout containment, style containment, and
    // paint containment for the element.
    if (computed_values().content_visibility() == CSS::ContentVisibility::Auto)
        return true;

    return false;
}
// https://drafts.csswg.org/css-contain-2/#containment-paint
bool Node::has_paint_containment() const
{
    // However, giving an element paint containment has no effect if any of the following are true:

    // - if the element does not generate a principal box (as is the case with 'display: contents' or 'display: none')
    // Note: This is the principal box

    // - if its principal box is an internal table box other than 'table-cell'
    if (display().is_internal_table() && !display().is_table_cell())
        return false;

    // - if its principal box is an internal ruby box or a non-atomic inline-level box
    // FIXME: Implement this

    if (computed_values().contain().paint_containment)
        return true;

    // https://drafts.csswg.org/css-contain-2/#valdef-content-visibility-auto
    // Changes the used value of the 'contain' property so as to turn on layout containment, style containment, and
    // paint containment for the element.
    if (computed_values().content_visibility() == CSS::ContentVisibility::Auto)
        return true;

    return false;
}

bool NodeWithStyleAndBoxModelMetrics::should_create_inline_continuation() const
{
    // This node must have an inline parent.
    if (!parent())
        return false;
    auto const& parent_display = parent()->display();
    if (!parent_display.is_inline_outside() || !parent_display.is_flow_inside())
        return false;

    // This node must not be inline itself or out of flow (which gets handled separately).
    if (display().is_inline_outside() || is_out_of_flow())
        return false;

    // This node must not have `display: contents`; inline continuation gets handled by its children.
    if (display().is_contents())
        return false;

    // Internal table display types and table captions are handled by the table fixup algorithm.
    if (display().is_internal_table() || display().is_table_caption())
        return false;

    // Parent element must not be <foreignObject>
    if (is<SVG::SVGForeignObjectElement>(parent()->dom_node()))
        return false;

    // SVG related boxes should never be split.
    if (is_svg_box() || is_svg_svg_box() || is_svg_foreign_object_box())
        return false;

    // Replaced boxes with children (e.g. media elements with shadow DOM controls)
    // have their own formatting context; don't split them.
    if (parent()->is_replaced_box_with_children())
        return false;

    return true;
}

void NodeWithStyleAndBoxModelMetrics::propagate_style_along_continuation(CSS::ComputedProperties const& computed_style) const
{
    auto continuation = continuation_of_node();
    while (continuation && continuation->is_anonymous())
        continuation = continuation->continuation_of_node();
    if (continuation)
        continuation->apply_style(computed_style);
}

void NodeWithStyleAndBoxModelMetrics::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_continuation_of_node);
}

void Node::set_needs_layout_update(DOM::SetNeedsLayoutReason reason)
{
    if (m_needs_layout_update)
        return;

    if constexpr (UPDATE_LAYOUT_DEBUG) {
        // NOTE: We check some conditions here to avoid debug spam in documents that don't do layout.
        auto navigable = this->navigable();
        if (navigable && navigable->active_document() == &document())
            dbgln_if(UPDATE_LAYOUT_DEBUG, "NEED LAYOUT {}", DOM::to_string(reason));
    }

    m_needs_layout_update = true;

    if (auto* box = as_if<Box>(this))
        box->reset_cached_intrinsic_sizes();

    // Mark any anonymous children generated by this node for layout update.
    // NOTE: if this node generated an anonymous parent, all ancestors are indiscriminately marked below.
    for_each_child_of_type<Box>([&](Box& child) {
        if (child.is_anonymous() && !is<TableWrapper>(child)) {
            child.m_needs_layout_update = true;
            child.reset_cached_intrinsic_sizes();
        }
        return IterationDecision::Continue;
    });

    for (auto* ancestor = parent(); ancestor; ancestor = ancestor->parent()) {
        if (ancestor->m_needs_layout_update)
            break;
        ancestor->m_needs_layout_update = true;
        if (auto* svg_box = as_if<SVGSVGBox>(ancestor)) {
            document().mark_svg_root_as_needing_relayout(*svg_box);
            break;
        }
    }

    // Reset intrinsic size caches for ancestors up to abspos or SVG root boundary.
    // Absolutely positioned elements don't contribute to ancestor intrinsic sizes,
    // so changes inside an abspos box don't require resetting ancestor caches.
    // SVG root elements have intrinsic sizes determined solely by their own attributes
    // (width, height, viewBox), not by their children, so the same logic applies.
    for (auto* ancestor = parent(); ancestor; ancestor = ancestor->parent()) {
        auto* box = as_if<Box>(ancestor);
        if (!box)
            continue;
        box->reset_cached_intrinsic_sizes();
        if (box->is_absolutely_positioned() || box->is_svg_svg_box())
            break;
    }
}

}
