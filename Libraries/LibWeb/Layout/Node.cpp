/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Demangle.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/CursorStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageSetStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/Dump.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/FormattingContext.h>
#include <LibWeb/Layout/ImageBox.h>
#include <LibWeb/Layout/InlineNode.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/TableWrapper.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/Painting/PaintableWithLines.h>
#include <LibWeb/SVG/SVGClipPathElement.h>
#include <LibWeb/SVG/SVGFilterElement.h>
#include <LibWeb/SVG/SVGForeignObjectElement.h>
#include <LibWeb/SVG/SVGGradientElement.h>
#include <LibWeb/SVG/SVGPatternElement.h>
#include <LibWeb/SVG/SVGTextContentElement.h>

namespace Web::Layout {

Node::Node(DOM::Document& document, DOM::Node* node, AttachToDOMNode attach_to_dom_node)
    : m_dom_node(node ? *node : document)
    , m_anonymous(node == nullptr)
{
    if (node && attach_to_dom_node == AttachToDOMNode::Yes)
        node->set_layout_node({}, *this);
}

Node::~Node()
{
    if (m_paintable)
        m_paintable->detach_from_layout_node({});
}

static void invalidate_paint_caches(Node& node)
{
    if (auto paintable = node.paintable())
        paintable->invalidate_paint_cache();
}

void Node::prepare_for_detach_from_layout_tree()
{
    invalidate_paint_caches(*this);
    if (auto* node_with_style = as_if<NodeWithStyle>(*this))
        node_with_style->clear_image_observers();
    if (auto* image_box = as_if<ImageBox>(*this))
        image_box->image_provider().layout_node_was_detached();
}

void Node::prepare_subtree_for_detach_from_layout_tree()
{
    for_each_in_inclusive_subtree([](Node& node) {
        node.prepare_for_detach_from_layout_tree();
        return TraversalDecision::Continue;
    });
}

Node* Node::topmost_layout_node_of_top_layer_placement()
{
    auto* direct_viewport_child_candidate = this;
    while (direct_viewport_child_candidate->parent() && direct_viewport_child_candidate->parent()->is_anonymous())
        direct_viewport_child_candidate = direct_viewport_child_candidate->parent();
    if (!direct_viewport_child_candidate->parent() || !direct_viewport_child_candidate->parent()->is_viewport())
        return nullptr;
    return direct_viewport_child_candidate;
}

// https://www.w3.org/TR/css-display-3/#out-of-flow
bool Node::is_out_of_flow(FormattingContext const& formatting_context) const
{
    auto const* node_with_style = as_if<NodeWithStyle>(*this);
    return node_with_style && node_with_style->is_out_of_flow(formatting_context);
}

bool Node::is_out_of_flow() const
{
    auto const* node_with_style = as_if<NodeWithStyle>(*this);
    return node_with_style && node_with_style->is_out_of_flow();
}

bool NodeWithStyle::is_out_of_flow(FormattingContext const& formatting_context) const
{
    // A layout node is out of flow if either:

    // 1. It is floated (which requires that floating is not inhibited).
    if (!formatting_context.inhibits_floating() && is_floating())
        return true;

    // 2. It is "absolutely positioned".
    if (is_absolutely_positioned())
        return true;

    return false;
}

// https://drafts.csswg.org/css-position-3/#fixed-positioning-containing-block
static bool computed_values_establish_fixed_positioning_containing_block(NodeWithStyle const& node)
{
    auto const& computed_values = node.computed_values();

    // https://drafts.csswg.org/css-will-change/#will-change
    // If any non-initial value of a property would cause the element to generate a containing block for fixed
    // positioned elements, specifying that property in will-change must cause the element to generate a containing
    // block for fixed positioned elements.
    auto const& will_change = computed_values.will_change();
    auto has_will_change = !will_change.is_auto();
    auto will_change_property = [&](CSS::PropertyID property_id) {
        return has_will_change && will_change.has_property(property_id);
    };

    Optional<bool> is_transformable;
    auto node_is_transformable = [&] {
        if (!is_transformable.has_value())
            is_transformable = node.is_transformable();
        return *is_transformable;
    };

    // https://drafts.csswg.org/css-transforms-1/#propdef-transform
    // Any computed value other than none for the transform affects containing block and stacking context.
    if ((!computed_values.transformations().is_empty() || will_change_property(CSS::PropertyID::Transform)) && node_is_transformable())
        return true;
    if ((computed_values.translate() || will_change_property(CSS::PropertyID::Translate)) && node_is_transformable())
        return true;
    if ((computed_values.rotate() || will_change_property(CSS::PropertyID::Rotate)) && node_is_transformable())
        return true;
    if ((computed_values.scale() || will_change_property(CSS::PropertyID::Scale)) && node_is_transformable())
        return true;

    // https://drafts.csswg.org/css-transforms-2/#propdef-perspective
    // The use of this property with any value other than 'none' establishes a stacking context. It also establishes
    // a containing block for all descendants, just like the 'transform' property does.
    if ((computed_values.perspective().has_value() || will_change_property(CSS::PropertyID::Perspective)) && node_is_transformable())
        return true;

    // https://drafts.csswg.org/filter-effects-1/#FilterProperty
    // A value other than none for the filter property results in the creation of a containing block for absolute and
    // fixed positioned descendants, unless the element it applies to is a document root element in the current
    // browsing context.
    if ((computed_values.filter().has_filters() || will_change_property(CSS::PropertyID::Filter)) && !node.is_root_element())
        return true;

    // https://drafts.csswg.org/filter-effects-2/#BackdropFilterProperty
    // A computed value of other than none results in the creation of both a stacking context and a containing block
    // for absolute and fixed position descendants, unless the element it applies to is a document root element in the
    // current browsing context.
    if ((computed_values.backdrop_filter().has_filters() || will_change_property(CSS::PropertyID::BackdropFilter)) && !node.is_root_element())
        return true;

    // https://drafts.csswg.org/css-contain-2/#containment-types
    // 4. The layout containment box establishes an absolute positioning containing block and a fixed positioning
    //    containing block.
    // 4. The paint containment box establishes an absolute positioning containing block and a fixed positioning
    //    containing block.
    if (will_change_property(CSS::PropertyID::Contain))
        return true;
    auto content_visibility_adds_containment = computed_values.content_visibility() == CSS::ContentVisibility::Auto;
    if ((computed_values.contain().layout_containment || content_visibility_adds_containment) && node.has_layout_containment())
        return true;
    if ((computed_values.contain().paint_containment || content_visibility_adds_containment) && node.has_paint_containment())
        return true;

    // https://drafts.csswg.org/css-transforms-2/#transform-style-property
    // A computed value of 'preserve-3d' for 'transform-style' on a transformable element establishes both a
    // stacking context and a containing block for all descendants.
    if ((computed_values.transform_style() == CSS::TransformStyle::Preserve3d || will_change_property(CSS::PropertyID::TransformStyle)) && node_is_transformable())
        return true;

    // https://drafts.csswg.org/css-view-transitions-1/#snapshot-containing-block-concept
    // FIXME: The snapshot containing block is considered to be an absolute positioning containing block and a fixed
    //        positioning containing block for ::view-transition and its descendants.

    return false;
}

// https://drafts.csswg.org/css-position-3/#absolute-positioning-containing-block
// Checks if the computed values of this node would establish an absolute positioning
// containing block. This is separate from establishes_an_absolute_positioning_containing_block()
// because that function also checks is<Box>, but we need these checks for inline elements too.
bool NodeWithStyle::computed_values_establish_absolute_positioning_containing_block() const
{
    auto const& computed_values = this->computed_values();

    // https://drafts.csswg.org/css-position/#position-property
    // Values other than 'static' make the box a positioned box, and cause it to establish an absolute positioning
    // containing block for its descendants.
    if (computed_values.position() != CSS::Positioning::Static
        || (!computed_values.will_change().is_auto() && computed_values.will_change().has_property(CSS::PropertyID::Position)))
        return true;

    return computed_values_establish_fixed_positioning_containing_block(*this);
}

// https://drafts.csswg.org/css-position-3/#absolute-positioning-containing-block
bool NodeWithStyle::establishes_an_absolute_positioning_containing_block() const
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
bool NodeWithStyle::establishes_a_fixed_positioning_containing_block() const
{
    if (!is<Box>(*this))
        return false;

    // https://github.com/w3c/fxtf-drafts/issues/307#issuecomment-499612420
    // foreignObject establishes a containing block for absolutely and fixed positioned elements.
    if (is_svg_foreign_object_box())
        return true;

    return computed_values_establish_fixed_positioning_containing_block(*this);
}

NodeWithStyle::PositioningContainingBlockEstablishment NodeWithStyle::establishes_positioning_containing_blocks() const
{
    if (!is<Box>(*this))
        return {};

    // https://github.com/w3c/fxtf-drafts/issues/307#issuecomment-499612420
    // foreignObject establishes a containing block for absolutely and fixed positioned elements.
    if (is_svg_foreign_object_box())
        return { true, true };

    auto establishes_fixed_positioning_containing_block = computed_values_establish_fixed_positioning_containing_block(*this);
    if (establishes_fixed_positioning_containing_block)
        return { true, true };

    auto const& computed_values = this->computed_values();
    auto establishes_absolute_positioning_containing_block = computed_values.position() != CSS::Positioning::Static
        || (!computed_values.will_change().is_auto() && computed_values.will_change().has_property(CSS::PropertyID::Position));
    if (establishes_absolute_positioning_containing_block)
        return { true, false };

    if (is<Viewport>(*this))
        return { true, false };

    return {};
}

static Box* nearest_ancestor_capable_of_forming_a_containing_block(Node& node)
{
    for (auto* ancestor = node.parent(); ancestor; ancestor = ancestor->parent()) {
        if ((ancestor->is_block_container() && !ancestor->is_fragmented_inline())
            || ancestor->display().is_flex_inside()
            || ancestor->display().is_grid_inside()
            || ancestor->is_replaced_box_with_children()) {
            return static_cast<Box*>(ancestor);
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

    auto position = as<NodeWithStyle>(*this).computed_values().position();

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

                // Restrict the per-property trigger set to those that actually apply to
                // non-atomic inlines: `position` and filter/backdrop-filter. transform,
                // contain, perspective and friends from
                // computed_values_establish_absolute_positioning_containing_block()
                // explicitly do not apply to non-atomic inlines per their respective specs.
                auto const& computed_values = layout_node->computed_values();
                auto const& will_change = computed_values.will_change();
                bool const inline_establishes_cb = layout_node->is_positioned()
                    || will_change.has_property(CSS::PropertyID::Position)
                    || computed_values.filter().has_filters() || will_change.has_property(CSS::PropertyID::Filter)
                    || computed_values.backdrop_filter().has_filters() || will_change.has_property(CSS::PropertyID::BackdropFilter);
                if (inline_establishes_cb) {
                    m_inline_containing_block_if_applicable = &as<InlineNode>(*layout_node);
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
bool NodeWithStyle::establishes_stacking_context() const
{
    // NOTE: While MDN is not authoritative, there isn't a single convenient location
    //       in the CSS specifications where the rules for stacking contexts is described.
    //       That's why the "spec link" here points to MDN.

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

    if (is_transformable()) {
        if (!computed_values.transformations().is_empty() || will_change_property(CSS::PropertyID::Transform))
            return true;

        if (computed_values.translate() || will_change_property(CSS::PropertyID::Translate))
            return true;

        if (computed_values.rotate() || will_change_property(CSS::PropertyID::Rotate))
            return true;

        if (computed_values.scale() || will_change_property(CSS::PropertyID::Scale))
            return true;
    }

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
    if (is_transformable() && (computed_values.perspective().has_value() || will_change_property(CSS::PropertyID::Perspective)))
        return true;

    // https://drafts.csswg.org/css-transforms-2/#transform-style-property
    // A computed value of 'preserve-3d' for 'transform-style' on a transformable element establishes both a
    // stacking context and a containing block for all descendants.
    if (is_transformable() && (computed_values.transform_style() == CSS::TransformStyle::Preserve3d || will_change_property(CSS::PropertyID::TransformStyle)))
        return true;

    return computed_values.opacity() < 1.0f || will_change_property(CSS::PropertyID::Opacity);
}

GC::Ptr<HTML::LocalNavigable> Node::navigable() const
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

bool NodeWithStyle::is_floating() const
{
    // flex-items don't float.
    if (is_flex_item())
        return false;
    return computed_values().float_() != CSS::Float::None;
}

bool NodeWithStyle::is_positioned() const
{
    return computed_values().position() != CSS::Positioning::Static;
}

bool NodeWithStyle::is_absolutely_positioned() const
{
    auto position = computed_values().position();
    return position == CSS::Positioning::Absolute || position == CSS::Positioning::Fixed;
}

bool NodeWithStyle::is_fixed_position() const
{
    auto position = computed_values().position();
    return position == CSS::Positioning::Fixed;
}

bool NodeWithStyle::is_sticky_position() const
{
    auto position = computed_values().position();
    return position == CSS::Positioning::Sticky;
}

NodeWithStyle::NodeWithStyle(DOM::Document& document, DOM::Node* node, NonnullRefPtr<CSS::ComputedValues const> computed_values)
    : Node(document, node)
    , m_computed_values(move(computed_values))
    , m_layout_index(document.allocate_layout_node_index())
{
    m_has_style = true;
    m_is_body = node && node == document.body();
}

NodeWithStyle::ImageObserver::ImageObserver(NodeWithStyle& owner, NonnullRefPtr<CSS::ImageStyleValue const> image)
    : CSS::ImageStyleValue::Client(owner.document(), *image)
    , m_owner(owner)
    , m_image(move(image))
{
}

NodeWithStyle::ImageObserver::~ImageObserver()
{
    image_style_value_finalize();
}

void NodeWithStyle::ImageObserver::image_style_value_did_update(CSS::ImageStyleValue&)
{
    VERIFY(m_owner);

    if (auto paintable = m_owner->paintable())
        paintable->set_needs_repaint();

    // The body's background propagates to the root element's paintable, which holds the cached draw commands.
    if (m_owner->is_body()) {
        auto* html_element = m_owner->document().html_element();
        if (html_element) {
            if (auto html_layout_node = html_element->unsafe_layout_node()) {
                if (html_element->should_use_body_background_properties()) {
                    if (auto paintable = html_layout_node->paintable())
                        paintable->set_needs_repaint();
                }
            }
        }
    }
}

NodeWithStyle::~NodeWithStyle()
{
    clear_image_observers();
}

void NodeWithStyle::clear_image_observers()
{
    m_image_observers.clear();
}

void NodeWithStyle::rebuild_image_observers()
{
    auto add_observer_for = [&](CSS::AbstractImageStyleValue const* abstract_image, Vector<NonnullOwnPtr<ImageObserver>>& observers) {
        if (!abstract_image)
            return;
        CSS::ImageStyleValue const* image_to_observe = nullptr;
        if (abstract_image->is_image()) {
            image_to_observe = &abstract_image->as_image();
        } else if (abstract_image->is_image_set()) {
            if (auto const* selected = abstract_image->as_image_set().selected_image(); selected && selected->is_image())
                image_to_observe = &selected->as_image();
        }
        if (!image_to_observe)
            return;
        observers.append(make<ImageObserver>(*this, *image_to_observe));
    };

    Vector<NonnullOwnPtr<ImageObserver>> new_observers;
    for (auto const& layer : computed_values().background_layers())
        add_observer_for(layer.background_image.ptr(), new_observers);
    add_observer_for(computed_values().list_style_image(), new_observers);
    for (auto const& layer : computed_values().mask_layers())
        add_observer_for(layer.background_image.ptr(), new_observers);
    for (auto const& cursor : computed_values().cursor()) {
        if (auto const* cursor_style_value = cursor.get_pointer<NonnullRefPtr<CSS::CursorStyleValue const>>())
            add_observer_for(&(*cursor_style_value)->image(), new_observers);
    }
    if (auto const& border_image = computed_values().border_image(); border_image.source)
        add_observer_for(border_image.source.ptr(), new_observers);
    // TODO: Observe other <image> accepting properties once we support them.

    m_image_observers = move(new_observers);
}

}

namespace Web::Layout {

void NodeWithStyle::apply_style(NonnullRefPtr<CSS::ComputedValues const> computed_values)
{
    m_computed_values = move(computed_values);

    propagate_style_to_anonymous_wrappers();

    attach_style_resources();
}

void NodeWithStyle::attach_style_resources()
{
    auto load_image = [&](CSS::AbstractImageStyleValue const* image) {
        if (image)
            const_cast<CSS::AbstractImageStyleValue&>(*image).load_any_resources(*this);
    };

    for (auto const& layer : computed_values().background_layers())
        load_image(layer.background_image.ptr());
    for (auto const& layer : computed_values().mask_layers())
        load_image(layer.background_image.ptr());
    if (auto const& border_image = computed_values().border_image(); border_image.source)
        load_image(border_image.source.ptr());
    for (auto const& cursor_data : computed_values().cursor()) {
        if (auto const* cursor_style_value = cursor_data.get_pointer<NonnullRefPtr<CSS::CursorStyleValue const>>())
            load_image(&(*cursor_style_value)->image());
    }
    load_image(computed_values().mask_image().ptr());

    load_image(computed_values().list_style_image());

    rebuild_image_observers();
}

CSS::StyleScope const& NodeWithStyle::style_scope() const
{
    if (auto const* dom_node = this->dom_node())
        return dom_node->style_scope();

    if (is_generated_for_pseudo_element())
        return pseudo_element_generator()->style_scope();

    if (auto const* parent = this->parent())
        return parent->style_scope();

    return document().style_scope();
}

void NodeWithStyle::propagate_non_inherit_values(CSS::ComputedValues::Builder& builder) const
{
    // NOTE: These properties are not inherited, but we still have to propagate them to anonymous wrappers.
    builder->set_text_decoration_line(computed_values().text_decoration_line());
    builder->set_text_decoration_thickness(computed_values().text_decoration_thickness());
    builder->set_text_decoration_color(computed_values().text_decoration_color());
    builder->set_text_decoration_style(computed_values().text_decoration_style());
}

void NodeWithStyle::propagate_style_to_anonymous_wrappers()
{
    // Update the style of any anonymous wrappers that inherit from this node.
    // FIXME: This is pretty hackish. It would be nicer if they shared the inherited style
    //        data structure somehow, so this wasn't necessary.

    // If this is a `display:table` box with an anonymous wrapper parent,
    // the parent inherits style from *this* node, not the other way around.
    if (auto* table_wrapper = as_if<TableWrapper>(parent()); table_wrapper && display().is_table_inside()) {
        CSS::ComputedValues::Builder builder(table_wrapper->computed_values());
        builder->inherit_from(computed_values());
        transfer_table_box_computed_values_to_wrapper_computed_values(builder);
        table_wrapper->set_computed_values(move(builder).build());
    }

    // Propagate style to all anonymous children (except table wrappers!)
    for_each_child_of_type<NodeWithStyle>([&](NodeWithStyle& child) {
        if (child.is_anonymous() && !is<TableWrapper>(child)) {
            // NB: The principal box of a pseudo-element (::before, ::after, ::marker, etc) is anonymous in the
            //     sense that it has no DOM node, but it's not an anonymous wrapper: it has its own computed style,
            //     which is applied to it separately. Don't clobber that style with inherited values from this node.
            if (auto pseudo_element = child.generated_for_pseudo_element(); pseudo_element.has_value()
                && child.pseudo_element_generator()->pseudo_element_unsafe_layout_node(*pseudo_element) == &child) {
                return IterationDecision::Continue;
            }
            CSS::ComputedValues::Builder builder(child.computed_values());
            builder->inherit_from(computed_values());
            propagate_non_inherit_values(builder);
            child.set_computed_values(move(builder).build());
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

bool Node::is_inline() const
{
    if (is<TextNode>(*this))
        return true;
    return as<NodeWithStyle>(*this).display().is_inline_outside();
}

bool NodeWithStyle::is_inline_block() const
{
    auto display = this->display();
    return display.is_inline_outside() && display.is_flow_root_inside();
}

bool NodeWithStyle::is_inline_table() const
{
    auto display = this->display();
    return display.is_inline_outside() && display.is_table_inside();
}

bool Node::is_replaced_element() const
{
    // Some native controls use a generic box so they can host their internal shadow tree, but
    // remain replaced elements for CSS box generation and inline layout.
    return is_replaced_box() || is<HTML::HTMLInputElement>(dom_node());
}

bool NodeWithStyle::has_replaced_element_table_display_adjustment() const
{
    if (!is_replaced_element())
        return false;
    auto display = display_before_box_type_transformation();
    return display.is_table_inside() || display.is_internal_table() || display.is_table_caption();
}

bool Node::is_atomic_inline() const
{
    if (is_replaced_element() || is_list_item_marker_box())
        return true;
    auto const* node_with_style = as_if<NodeWithStyle>(*this);
    if (!node_with_style)
        return false;
    auto display = node_with_style->display();
    return display.is_inline_outside() && !display.is_flow_inside();
}

bool Node::is_fragmented_inline() const
{
    return is_inline_node()
        || (is_list_item_box() && as<NodeWithStyle>(*this).display().is_inline_outside() && as<NodeWithStyle>(*this).display().is_flow_inside());
}

NodeWithStyleAndBoxModelMetrics const* Node::nearest_fragmented_inline_ancestor() const
{
    for (auto const* ancestor = parent(); ancestor; ancestor = ancestor->parent()) {
        if (!ancestor->display().is_inline_outside() || !ancestor->display().is_flow_inside())
            break;
        if (ancestor->is_fragmented_inline())
            return static_cast<NodeWithStyleAndBoxModelMetrics const*>(ancestor);
    }
    return nullptr;
}

// https://drafts.csswg.org/css-transforms-1/#transformable-element
bool NodeWithStyle::is_transformable() const
{
    // A transformable element is an element in one of these categories:
    auto const* dom_node = this->dom_node();

    // * all SVG paint server elements, the clipPath element and SVG renderable elements with the exception
    //   of any descendant element of text content elements [SVG2].
    if (is<SVG::SVGElement>(dom_node)) {
        // Paint servers and clipPath are always transformable.
        if (is<SVG::SVGGradientElement>(*dom_node) || is<SVG::SVGPatternElement>(*dom_node) || is<SVG::SVGClipPathElement>(*dom_node))
            return true;
        auto const is_renderable = (is_svg_graphics_box() && !is_svg_mask_box()) || is_svg_svg_box() || is_svg_foreign_object_box();
        if (!is_renderable)
            return false;
        // ...with the exception of any descendant of a text content element.
        for (auto const* ancestor = parent(); ancestor; ancestor = ancestor->parent()) {
            if (auto const* ancestor_dom_node = ancestor->dom_node(); ancestor_dom_node && is<SVG::SVGTextContentElement>(*ancestor_dom_node))
                return false;
        }
        return true;
    }

    // * all elements whose layout is governed by the CSS box model except for non-replaced inline boxes,
    //   table-column boxes, and table-column-group boxes [CSS2].
    bool is_element_or_pseudo_element = is<DOM::Element>(dom_node) || is_generated_for_pseudo_element();
    if (is_element_or_pseudo_element && is_box()) {
        auto display = this->display();
        if (display.is_table_column() || display.is_table_column_group())
            return false;

        if (is_inline() && !is_atomic_inline())
            return false;

        return true;
    }

    return false;
}

NonnullRefPtr<NodeWithStyle> NodeWithStyle::create_anonymous_wrapper() const
{
    auto builder = CSS::ComputedValues::Builder::create_inheriting_from(computed_values());
    builder->set_display(CSS::Display(CSS::DisplayOutside::Block, CSS::DisplayInside::Flow));
    propagate_non_inherit_values(builder);
    // CSS 2.2 9.2.1.1 creates anonymous block boxes, but 9.4.1 states inline-block creates a BFC.
    // Set wrapper to inline-block to participate correctly in the IFC within the parent inline-block.
    if (display().is_inline_block() && !has_children())
        builder->set_display(CSS::Display::from_short(CSS::Display::Short::InlineBlock));
    auto wrapper = adopt_ref(*new BlockContainer(const_cast<DOM::Document&>(document()), nullptr, move(builder).build()));
    return *wrapper;
}

void NodeWithStyle::set_computed_values(NonnullRefPtr<CSS::ComputedValues const> computed_values)
{
    m_computed_values = move(computed_values);
}

void NodeWithStyle::reset_table_box_computed_values_used_by_wrapper_to_init_values()
{
    VERIFY(this->display().is_table_inside());

    modify_computed_values([](auto& values) {
        values.set_position(CSS::InitialValues::position());
        values.set_position_anchor(CSS::InitialValues::position_anchor());
        values.set_float(CSS::InitialValues::float_());
        values.set_clear(CSS::InitialValues::clear());
        values.set_inset(CSS::InitialValues::inset());
        values.set_grid_column_end(CSS::InitialValues::grid_column_end());
        values.set_grid_column_start(CSS::InitialValues::grid_column_start());
        values.set_grid_row_end(CSS::InitialValues::grid_row_end());
        values.set_grid_row_start(CSS::InitialValues::grid_row_start());
        values.set_align_self(CSS::InitialValues::align_self());
        values.set_justify_self(CSS::InitialValues::justify_self());
        values.set_order(CSS::InitialValues::order());
        values.set_margin(CSS::InitialValues::margin());
        // AD-HOC:
        // To match other browsers, z-index needs to be moved to the wrapper box as well,
        // even if the spec does not mention that: https://github.com/w3c/csswg-drafts/issues/11689
        // Note that there may be more properties that need to be added to this list.
        values.set_z_index(CSS::InitialValues::z_index());
        values.set_clip(CSS::InitialValues::clip());
    });
}

void NodeWithStyle::transfer_table_box_computed_values_to_wrapper_computed_values(CSS::ComputedValues::Builder& builder)
{
    // The computed values of properties 'position', 'float', 'margin-*', 'top', 'right', 'bottom', and 'left' on the table element are used on the table wrapper box and not the table box;
    // all other values of non-inheritable properties are used on the table box and not the table wrapper box.
    // (Where the table element's values are not used on the table and table wrapper boxes, the initial values are used instead.)
    if (display().is_inline_outside())
        builder->set_display(CSS::Display::from_short(CSS::Display::Short::InlineBlock));
    else
        builder->set_display(CSS::Display::from_short(CSS::Display::Short::FlowRoot));
    builder->set_position(computed_values().position());
    builder->set_position_anchor(computed_values().position_anchor_value());
    builder->set_inset(computed_values().inset());
    builder->set_float(computed_values().float_());
    builder->set_clear(computed_values().clear());
    // CSS 2 moves table-root positioning and margins to the wrapper. The wrapper is also the grid item for
    // display:table, so grid placement, self-alignment, and order need to move there as well.
    builder->set_grid_column_end(computed_values().grid_column_end());
    builder->set_grid_column_start(computed_values().grid_column_start());
    builder->set_grid_row_end(computed_values().grid_row_end());
    builder->set_grid_row_start(computed_values().grid_row_start());
    builder->set_align_self(computed_values().align_self());
    builder->set_justify_self(computed_values().justify_self());
    builder->set_order(computed_values().order());
    builder->set_margin(computed_values().margin());
    // AD-HOC:
    // To match other browsers, z-index needs to be moved to the wrapper box as well,
    // even if the spec does not mention that: https://github.com/w3c/csswg-drafts/issues/11689
    // Note that there may be more properties that need to be added to this list.
    builder->set_z_index(computed_values().z_index());
    // "clip" only takes effect on absolutely-positioned elements; the table box isn't one — the wrapper is.
    builder->set_clip(computed_values().clip());

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

void Node::set_paintable(RefPtr<Painting::Paintable> paintable)
{
    m_paintable = move(paintable);
}

void Node::clear_paintable()
{
    if (m_paintable)
        document().invalidate_stacking_context_tree();

    invalidate_paint_caches(*this);
    if (m_paintable) {
        if (m_paintable->parent())
            m_paintable->remove();
        // NB: Layout state may retain this paintable after it stops being the node's current paintable, but its
        //     chrome widgets must no longer use it for input handling.
        m_paintable->detach_chrome_widgets();
        m_paintable = nullptr;
    }
}

RefPtr<Painting::Paintable> Node::create_paintable() const
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
    VERIFY(m_dom_node);
    return m_dom_node.ptr();
}

DOM::Node* Node::dom_node()
{
    if (m_anonymous)
        return nullptr;
    VERIFY(m_dom_node);
    return m_dom_node.ptr();
}

DOM::Element const* Node::pseudo_element_generator() const
{
    VERIFY(m_generated_for.has_value());
    VERIFY(m_pseudo_element_generator);
    return m_pseudo_element_generator.ptr();
}

DOM::Element* Node::pseudo_element_generator()
{
    VERIFY(m_generated_for.has_value());
    VERIFY(m_pseudo_element_generator);
    return m_pseudo_element_generator.ptr();
}

void Node::set_generated_for(CSS::PseudoElement type, DOM::Element& element)
{
    m_generated_for = type;
    m_pseudo_element_generator = element;
}

DOM::Document& Node::document()
{
    VERIFY(m_dom_node);
    return m_dom_node->document();
}

DOM::Document const& Node::document() const
{
    VERIFY(m_dom_node);
    return m_dom_node->document();
}

// https://drafts.csswg.org/css-ui/#propdef-user-select
CSS::UserSelect Node::user_select_used_value() const
{
    if (!has_style_or_parent_with_style())
        return CSS::UserSelect::None;

    if (!is_generated_for_pseudo_element()) {
        if (auto const* node = dom_node())
            return node->user_select_used_value();
    }

    auto const* node_with_style = as_if<NodeWithStyle>(*this);
    auto const& computed_values = node_with_style ? node_with_style->computed_values() : parent()->computed_values();
    auto computed_value = computed_values.user_select();
    if (computed_value != CSS::UserSelect::Auto)
        return computed_value;

    if (is_generated_for_before_pseudo_element() || is_generated_for_after_pseudo_element())
        return CSS::UserSelect::None;

    if (auto parent_node = parent())
        return parent_node->user_select_used_value();

    return CSS::UserSelect::Text;
}

// https://drafts.csswg.org/css-contain-2/#containment-size
bool NodeWithStyle::has_size_containment() const
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
bool NodeWithStyle::has_inline_size_containment() const
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
bool NodeWithStyle::has_layout_containment() const
{
    auto const& computed_values = this->computed_values();
    auto has_layout_containment = computed_values.contain().layout_containment;

    // https://drafts.csswg.org/css-contain-2/#valdef-content-visibility-auto
    // Changes the used value of the 'contain' property so as to turn on layout containment, style containment, and
    // paint containment for the element.
    has_layout_containment = has_layout_containment || computed_values.content_visibility() == CSS::ContentVisibility::Auto;
    if (!has_layout_containment)
        return false;

    // However, giving an element layout containment has no effect if any of the following are true:

    // - if the element does not generate a principal box (as is the case with 'display: contents' or 'display: none')
    // Note: This is the principal box

    // - if its principal box is an internal table box other than 'table-cell'
    if (display().is_internal_table() && !display().is_table_cell())
        return false;

    // - if its principal box is an internal ruby box or a non-atomic inline-level box
    // FIXME: Also check for internal ruby boxes.
    if (display().is_inline_outside() && display().is_flow_inside() && !is_replaced_box())
        return false;

    return true;
}
// https://drafts.csswg.org/css-contain-2/#containment-style
bool NodeWithStyle::has_style_containment() const
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
bool NodeWithStyle::has_paint_containment() const
{
    auto const& computed_values = this->computed_values();
    auto has_paint_containment = computed_values.contain().paint_containment;

    // https://drafts.csswg.org/css-contain-2/#valdef-content-visibility-auto
    // Changes the used value of the 'contain' property so as to turn on layout containment, style containment, and
    // paint containment for the element.
    has_paint_containment = has_paint_containment || computed_values.content_visibility() == CSS::ContentVisibility::Auto;
    if (!has_paint_containment)
        return false;

    // However, giving an element paint containment has no effect if any of the following are true:

    // - if the element does not generate a principal box (as is the case with 'display: contents' or 'display: none')
    // Note: This is the principal box

    // - if its principal box is an internal table box other than 'table-cell'
    if (display().is_internal_table() && !display().is_table_cell())
        return false;

    // - if its principal box is an internal ruby box or a non-atomic inline-level box
    // FIXME: Also check for internal ruby boxes.
    if (display().is_inline_outside() && display().is_flow_inside() && !is_replaced_box())
        return false;

    return true;
}

bool NodeWithStyleAndBoxModelMetrics::is_inline_flow_interrupting_block() const
{
    // This node remains a layout child of its inline-flow parent. InlineLevelIterator emits it as a BlockLevelBox item
    // so the inline formatting context can lay it out as an interrupting block.
    if (!parent())
        return false;
    auto const& parent_display = parent()->display();
    if (!parent_display.is_inline_outside() || !parent_display.is_flow_inside())
        return false;

    // This node must not be inline itself or out of flow (which gets handled separately).
    if (display().is_inline_outside() || is_out_of_flow())
        return false;

    // This node must not have `display: contents`; interrupting block handling gets delegated to its children.
    if (display().is_contents())
        return false;

    // Internal table display types and table captions are handled by the table fixup algorithm.
    if (display().is_internal_table() || display().is_table_caption())
        return false;

    // Parent element must not be <foreignObject>
    if (is<SVG::SVGForeignObjectElement>(parent()->dom_node()))
        return false;

    // Non-root SVG elements and foreign object boxes should not interrupt inline flow.
    if (is_svg_box() || is_svg_foreign_object_box())
        return false;

    // Nested SVG roots should not interrupt inline flow, but a top-level SVG root inside an HTML inline element should.
    if (is_svg_svg_box() && (parent()->is_svg_box() || parent()->is_svg_svg_box()))
        return false;

    // Replaced boxes with children (e.g. media elements with shadow DOM controls)
    // have their own formatting context; don't let their children interrupt inline flow.
    if (parent()->is_replaced_box_with_children())
        return false;

    return true;
}

void Node::set_needs_layout_update(DOM::SetNeedsLayoutReason reason, LayoutUpdatePropagation propagation)
{
    if (m_needs_layout_update && propagation == LayoutUpdatePropagation::ThroughAncestors) {
        // A dirty node normally implies dirty ancestors, but the walk that marked a partial
        // relayout boundary stopped there and left its ancestors clean, so a through-ancestors
        // invalidation arriving on the boundary itself must still walk and mark them.
        auto* box = as_if<Box>(this);
        if (!box || !box->is_partial_relayout_boundary())
            return;
    }

    if (!m_needs_layout_update) {
        if constexpr (UPDATE_LAYOUT_DEBUG) {
            // NOTE: We check some conditions here to avoid debug spam in documents that don't do layout.
            auto navigable = this->navigable();
            if (navigable && navigable->active_document() == &document())
                dbgln_if(UPDATE_LAYOUT_DEBUG, "NEED LAYOUT {}", DOM::to_string(reason));
        }

        m_needs_layout_update = true;
    }

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

    if (propagation == LayoutUpdatePropagation::BoundarySelfOnly) {
        document().partial_relayout_invalidation().record_boundary(as<Box>(*this));
        return;
    }

    for (auto* ancestor = parent(); ancestor; ancestor = ancestor->parent()) {
        if (ancestor->m_needs_layout_update)
            break;
        ancestor->m_needs_layout_update = true;
        if (auto* box = as_if<Box>(ancestor); box && box->is_partial_relayout_boundary()) {
            document().partial_relayout_invalidation().record_boundary(*box);
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
