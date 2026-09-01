/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Demangle.h>
#include <LibWeb/CSS/ComputedStyleWorkingSet.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/CursorStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/Dump.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLTableCellElement.h>
#include <LibWeb/HTML/HTMLTableColElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/LayoutRustBridge.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/NodeArena.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/ScrollSnap.h>
#include <LibWeb/SVG/SVGClipPathElement.h>
#include <LibWeb/SVG/SVGFilterElement.h>
#include <LibWeb/SVG/SVGGradientElement.h>
#include <LibWeb/SVG/SVGPatternElement.h>
#include <LibWeb/SVG/SVGTextContentElement.h>

namespace Web::Layout {

NodeArenaAllocation::NodeArenaAllocation(DOM::Document& document)
    : m_arena(document.layout_node_arena())
{
    auto allocation = m_arena->allocate();
    m_slot = allocation.slot;
    m_data = allocation.data;
    m_slot_generation = allocation.generation;
}

NodeArenaAllocation::~NodeArenaAllocation()
{
    m_arena->free(m_slot, m_slot_generation);
}

Node::Node(DOM::Document& document, GC::Ptr<DOM::Node> node, AttachToDOMNode attach_to_dom_node)
    : NodeArenaAllocation(document)
    , m_dom_node(node ? *node : document)
{
    m_data->shell = this;
    set_node_kind(RustFFI::NodeKind::Node);
    set_flag(RustFFI::NodeFlag::Anonymous, node == nullptr);
    // Some native controls use a generic box so they can host their internal shadow tree, but
    // remain replaced elements for CSS box generation and inline layout. (Box's constructor
    // sets the flag for the replaced box kinds.)
    set_flag(RustFFI::NodeFlag::IsReplacedElement, node && is<HTML::HTMLInputElement>(*node));
    set_flag(RustFFI::NodeFlag::IsHtmlInputElement, node && is<HTML::HTMLInputElement>(*node));
    set_flag(RustFFI::NodeFlag::IsHtmlHtmlElement, node && node->is_html_html_element());
    set_flag(RustFFI::NodeFlag::IsInUserAgentShadowTree,
        node && node->containing_shadow_root() && node->containing_shadow_root()->is_user_agent_internal());
    set_flag(RustFFI::NodeFlag::UsesButtonLayout,
        node && is<HTML::HTMLElement>(*node) && static_cast<HTML::HTMLElement const&>(*node).uses_button_layout());
    set_flag(RustFFI::NodeFlag::IsEditingHost, node && node->is_editing_host());

    if (node && attach_to_dom_node == AttachToDOMNode::Yes)
        node->set_layout_node({}, *this);
}

Node::~Node()
{
    VERIFY(!parent_ptr());
}

RustFFI::NodeSlotId Node::slot_id(Node const* node)
{
    return node ? node->m_slot : RustFFI::NodeSlotId_INVALID;
}

void Node::set_node_kind(RustFFI::NodeKind kind)
{
    m_data->kind = kind;
    enroll_for_arena_replaced_content_facts_sync_if_eligible();
}

StringView Node::class_name() const
{
#define LAYOUT_NODE_KIND_NAME_CASE(kind_name) \
    case RustFFI::NodeKind::kind_name:        \
        return #kind_name##sv;
    switch (kind()) {
        LAYOUT_NODE_KIND_NAME_CASE(AudioBox)
        LAYOUT_NODE_KIND_NAME_CASE(BlockContainer)
        LAYOUT_NODE_KIND_NAME_CASE(Box)
        LAYOUT_NODE_KIND_NAME_CASE(BreakNode)
        LAYOUT_NODE_KIND_NAME_CASE(CanvasBox)
        LAYOUT_NODE_KIND_NAME_CASE(CheckBox)
        LAYOUT_NODE_KIND_NAME_CASE(FieldSetBox)
        LAYOUT_NODE_KIND_NAME_CASE(GeneratedTextNode)
        LAYOUT_NODE_KIND_NAME_CASE(ImageBox)
        LAYOUT_NODE_KIND_NAME_CASE(InlineNode)
        LAYOUT_NODE_KIND_NAME_CASE(LegendBox)
        LAYOUT_NODE_KIND_NAME_CASE(ListItemBox)
        LAYOUT_NODE_KIND_NAME_CASE(ListItemMarkerBox)
        LAYOUT_NODE_KIND_NAME_CASE(NavigableContainerViewport)
        LAYOUT_NODE_KIND_NAME_CASE(Node)
        LAYOUT_NODE_KIND_NAME_CASE(NodeWithStyle)
        LAYOUT_NODE_KIND_NAME_CASE(RadioButton)
        LAYOUT_NODE_KIND_NAME_CASE(RangeInputBox)
        LAYOUT_NODE_KIND_NAME_CASE(ReplacedBox)
        LAYOUT_NODE_KIND_NAME_CASE(SVGBox)
        LAYOUT_NODE_KIND_NAME_CASE(SVGClipBox)
        LAYOUT_NODE_KIND_NAME_CASE(SVGForeignObjectBox)
        LAYOUT_NODE_KIND_NAME_CASE(SVGGeometryBox)
        LAYOUT_NODE_KIND_NAME_CASE(SVGGraphicsBox)
        LAYOUT_NODE_KIND_NAME_CASE(SVGImageBox)
        LAYOUT_NODE_KIND_NAME_CASE(SVGMaskBox)
        LAYOUT_NODE_KIND_NAME_CASE(SVGPatternBox)
        LAYOUT_NODE_KIND_NAME_CASE(SVGSVGBox)
        LAYOUT_NODE_KIND_NAME_CASE(SVGTextBox)
        LAYOUT_NODE_KIND_NAME_CASE(SVGTextPathBox)
        LAYOUT_NODE_KIND_NAME_CASE(TableWrapper)
        LAYOUT_NODE_KIND_NAME_CASE(TextAreaBox)
        LAYOUT_NODE_KIND_NAME_CASE(TextInputBox)
        LAYOUT_NODE_KIND_NAME_CASE(TextNode)
        LAYOUT_NODE_KIND_NAME_CASE(TextSliceNode)
        LAYOUT_NODE_KIND_NAME_CASE(VideoBox)
        LAYOUT_NODE_KIND_NAME_CASE(Viewport)
    case RustFFI::NodeKind::Unset:
        break;
    }
#undef LAYOUT_NODE_KIND_NAME_CASE
    VERIFY_NOT_REACHED();
}

void Node::enroll_for_arena_replaced_content_facts_sync_if_eligible()
{
    if (m_enrolled_for_arena_replaced_content_facts_sync)
        return;
    if (!RustFFI::layout_node_data_may_have_replaced_content_facts(m_data))
        return;
    m_enrolled_for_arena_replaced_content_facts_sync = true;
    node_arena().enroll_node_for_replaced_content_facts_sync(*this);
}

bool Node::fragment_cache_epochs_enabled()
{
    // The Rust cache module owns the only LADYBIRD_FC_RUN_CACHE parser; the
    // bump walks follow whatever mode it resolved.
    static bool const enabled = RustFFI::layout_fc_run_cache_epochs_enabled();
    return enabled;
}

void Node::bump_fragment_cache_epoch()
{
    if (!fragment_cache_epochs_enabled())
        return;
    ++node_data().fragment_cache_epoch;
}

void Node::bump_fragment_cache_epoch_of_self_and_ancestors()
{
    RustFFI::layout_arena_bump_fragment_cache_epoch_of_self_and_ancestors(arena_handle(), slot_id(this));
}

// Reset intrinsic size caches for ancestors up to abspos or SVG root boundary.
// Absolutely positioned elements don't contribute to ancestor intrinsic sizes,
// so changes inside an abspos box don't require resetting ancestor caches.
// SVG root elements have intrinsic sizes determined solely by their own attributes
// (width, height, viewBox), not by their children, so the same logic applies.
static void reset_cached_intrinsic_sizes_of_ancestors(Node& node)
{
    for (auto* ancestor = node.parent(); ancestor; ancestor = ancestor->parent()) {
        auto* box = as_if<Box>(ancestor);
        if (!box)
            continue;
        box->reset_cached_intrinsic_sizes();
        if (box->is_absolutely_positioned() || box->is_svg_svg_box())
            break;
    }
}

static void reset_cached_intrinsic_sizes_of_self_and_ancestors(Node& node)
{
    if (auto* box = as_if<Box>(node))
        box->reset_cached_intrinsic_sizes();
    reset_cached_intrinsic_sizes_of_ancestors(node);
}

void* Node::arena_handle() const
{
    return m_arena->handle();
}

Box const* Node::containing_block() const
{
    return static_cast<Box const*>(tree_node_from_slot_if_live(m_data->containing_block));
}

Box* Node::containing_block()
{
    return static_cast<Box*>(tree_node_from_slot_if_live(m_data->containing_block));
}

static void invalidate_paint_caches(Node& node)
{
    if (Painting::has_committed_box(node))
        Painting::invalidate_paint_cache(node);
}

void Node::pin_style_record_for_detachment()
{
    if (auto* node_with_style = as_if<NodeWithStyle>(*this))
        node_with_style->pin_style_record_for_cxx_consumers();
}

void Node::prepare_for_detach_from_layout_tree()
{
    pin_style_record_for_detachment();
    invalidate_paint_caches(*this);
    if (auto* node_with_style = as_if<NodeWithStyle>(*this))
        node_with_style->clear_image_observers();
    if (kind() == RustFFI::NodeKind::ImageBox)
        static_cast<Box&>(*this).image_provider().layout_node_was_detached();
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

bool Node::is_pseudo_element_principal_box() const
{
    auto pseudo_element = generated_for_pseudo_element();
    return pseudo_element.has_value() && pseudo_element_generator()->pseudo_element_unsafe_layout_node(*pseudo_element) == this;
}

// https://drafts.csswg.org/css-position-3/#fixed-positioning-containing-block
static bool style_establishes_fixed_positioning_containing_block(NodeWithStyle const& node)
{
    // https://drafts.csswg.org/css-will-change/#will-change
    // If any non-initial value of a property would cause the element to generate a containing block for fixed
    // positioned elements, specifying that property in will-change must cause the element to generate a containing
    // block for fixed positioned elements.
    auto const& will_change = node.will_change();
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
    if ((node.has_transformations() || will_change_property(CSS::PropertyID::Transform)) && node_is_transformable())
        return true;
    if ((node.has_translate() || will_change_property(CSS::PropertyID::Translate)) && node_is_transformable())
        return true;
    if ((node.has_rotate() || will_change_property(CSS::PropertyID::Rotate)) && node_is_transformable())
        return true;
    if ((node.has_scale() || will_change_property(CSS::PropertyID::Scale)) && node_is_transformable())
        return true;

    // https://drafts.csswg.org/css-transforms-2/#propdef-perspective
    // The use of this property with any value other than 'none' establishes a stacking context. It also establishes
    // a containing block for all descendants, just like the 'transform' property does.
    if ((node.perspective().has_value() || will_change_property(CSS::PropertyID::Perspective)) && node_is_transformable())
        return true;

    // https://drafts.csswg.org/filter-effects-1/#FilterProperty
    // A value other than none for the filter property results in the creation of a containing block for absolute and
    // fixed positioned descendants, unless the element it applies to is a document root element in the current
    // browsing context.
    if ((node.filter().has_filters() || will_change_property(CSS::PropertyID::Filter)) && !node.is_root_element())
        return true;

    // https://drafts.csswg.org/filter-effects-2/#BackdropFilterProperty
    // A computed value of other than none results in the creation of both a stacking context and a containing block
    // for absolute and fixed position descendants, unless the element it applies to is a document root element in the
    // current browsing context.
    if ((node.backdrop_filter().has_filters() || will_change_property(CSS::PropertyID::BackdropFilter)) && !node.is_root_element())
        return true;

    // https://drafts.csswg.org/css-contain-2/#containment-types
    // 4. The layout containment box establishes an absolute positioning containing block and a fixed positioning
    //    containing block.
    // 4. The paint containment box establishes an absolute positioning containing block and a fixed positioning
    //    containing block.
    if (will_change_property(CSS::PropertyID::Contain))
        return true;
    auto content_visibility_adds_containment = node.content_visibility() == CSS::ContentVisibility::Auto;
    if ((node.contain().layout_containment || content_visibility_adds_containment) && node.has_layout_containment())
        return true;
    if ((node.contain().paint_containment || content_visibility_adds_containment) && node.has_paint_containment())
        return true;

    // https://drafts.csswg.org/css-transforms-2/#transform-style-property
    // A computed value of 'preserve-3d' for 'transform-style' on a transformable element establishes both a
    // stacking context and a containing block for all descendants.
    if ((node.transform_style() == CSS::TransformStyle::Preserve3d || will_change_property(CSS::PropertyID::TransformStyle)) && node_is_transformable())
        return true;

    // https://drafts.csswg.org/css-transforms-2/#backface-visibility-property
    // A computed value of hidden for backface-visibility on a transformable element that participates in a 3D
    // rendering context establishes both a stacking context and a containing block for all descendants.
    if ((node.style_group<CSS::ComputedValues::TransformValues>().backface_visibility_value() == CSS::BackfaceVisibility::Hidden || will_change_property(CSS::PropertyID::BackfaceVisibility))
        && node_is_transformable() && node.participates_in_a_3d_rendering_context())
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
bool NodeWithStyle::style_establishes_absolute_positioning_containing_block() const
{
    // https://drafts.csswg.org/css-position/#position-property
    // Values other than 'static' make the box a positioned box, and cause it to establish an absolute positioning
    // containing block for its descendants.
    if (position() != CSS::Positioning::Static
        || (!will_change().is_auto() && will_change().has_property(CSS::PropertyID::Position)))
        return true;

    return style_establishes_fixed_positioning_containing_block(*this);
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

    return style_establishes_absolute_positioning_containing_block();
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

    return style_establishes_fixed_positioning_containing_block(*this);
}

// FIXME: Containing block handling for absolutely positioned elements needs architectural improvements.
//
//        The CSS specification defines the containing block as a *rectangle*, not a box. For most cases,
//        this rectangle is derived from the padding box of the nearest positioned ancestor Box. However,
//        when the positioned ancestor is an *inline* element (e.g., a <span> with position: relative),
//        the containing block rectangle should be the bounding box of that inline's fragments.
//
//        Currently, the stored containing block can only name a Box, which cannot represent inline
//        elements. The proper fix would be to:
//        1. Separate the concept of "the node that establishes the containing block" from "the containing
//           block rectangle".
//        2. Store a reference to the establishing node (which could be InlineNode or Box).
//        3. Compute the containing block rectangle on demand based on the establishing node's type.
//
//        For now, we use a workaround: check if there's an inline element with position:relative (or
//        other containing-block-establishing properties) between this node and its containing block
//        in the DOM tree. If found, it is stored in the arena's inline_containing_block slot.
//
//        We check the DOM tree here (rather than the layout tree) because when a block element is inside
//        an inline element, the layout tree restructures so the block becomes a sibling of the inline.
//        But the CSS containing block relationship is based on the DOM structure.
NodeWithStyle const* Node::find_inline_containing_block(Box const& containing_block) const
{
    auto const* containing_block_dom_node = containing_block.dom_node();

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
        if (!layout_node || !layout_node->is_inline_node())
            continue;

        // Restrict the per-property trigger set to those that actually apply to
        // non-atomic inlines: `position` and filter/backdrop-filter. transform,
        // contain, perspective and friends from
        // style_establishes_absolute_positioning_containing_block()
        // explicitly do not apply to non-atomic inlines per their respective specs.
        auto const& will_change = layout_node->will_change();
        bool const inline_establishes_cb = layout_node->is_positioned()
            || will_change.has_property(CSS::PropertyID::Position)
            || layout_node->filter().has_filters() || will_change.has_property(CSS::PropertyID::Filter)
            || layout_node->backdrop_filter().has_filters() || will_change.has_property(CSS::PropertyID::BackdropFilter);
        if (inline_establishes_cb)
            return static_cast<NodeWithStyle const*>(layout_node);
    }
    return nullptr;
}

GC::Ptr<HTML::LocalNavigable> Node::navigable() const
{
    return document().navigable();
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
    return float_() != CSS::Float::None;
}

bool NodeWithStyle::is_positioned() const
{
    return position() != CSS::Positioning::Static;
}

bool NodeWithStyle::is_absolutely_positioned() const
{
    auto position = this->position();
    return position == CSS::Positioning::Absolute || position == CSS::Positioning::Fixed;
}

bool NodeWithStyle::is_fixed_position() const
{
    auto position = this->position();
    return position == CSS::Positioning::Fixed;
}

bool NodeWithStyle::is_sticky_position() const
{
    auto position = this->position();
    return position == CSS::Positioning::Sticky;
}

NodeWithStyle::NodeWithStyle(DOM::Document& document, GC::Ptr<DOM::Node> node, CSS::LayoutStyle style, RustFFI::NodeKind kind)
    : Node(document, node)
{
    set_node_kind(kind);
    VERIFY(style);
    if (!!style.style_record_identity()) {
        m_style_record_identity = style.style_record_identity();
    } else if (auto* element = as_if<DOM::Element>(node.ptr())) {
        m_owned_computed_values = style.values();
        m_style_record_identity = document.style_computer().intern_computed_style_inputs({ *element }, *style.values());
    } else {
        m_owned_computed_values = style.values();
        m_style_record_identity = document.style_computer().intern_anonymous_layout_style(*style.values());
    }
    set_flag(RustFFI::NodeFlag::HasStyle, true);
    set_flag(RustFFI::NodeFlag::IsBody, node && node == GC::Ptr { document.body() });
    // NB: Nodes constructed from an interned style record own no ComputedValues; read anchor names through the record view there.
    bool has_anchor_names = false;
    bool insets_use_anchor_functions = false;
    if (m_owned_computed_values) {
        has_anchor_names = !m_owned_computed_values->anchor_names().is_empty();
        insets_use_anchor_functions = m_owned_computed_values->inset_properties_contain_anchor_functions();
    } else if (auto record_view = computed_style_record_view()) {
        has_anchor_names = !record_view->anchor_names().is_empty();
        insets_use_anchor_functions = record_view->inset_properties_contain_anchor_functions();
    }
    set_flag(RustFFI::NodeFlag::HasAnchorNames, has_anchor_names);
    set_flag(RustFFI::NodeFlag::InsetsUseAnchorFunctions, insets_use_anchor_functions);
    set_flag(RustFFI::NodeFlag::HasAnimatedOpacityOrTransform, false);
    publish_style_record_to_node_data();
    set_flag(RustFFI::NodeFlag::HasPreserve3dTransformStyle, transform_style() == CSS::TransformStyle::Preserve3d);
    synchronize_table_span_data();
    enroll_for_arena_replaced_content_facts_sync_if_eligible();
    if (m_owned_computed_values)
        pin_style_record_for_cxx_consumers();
}

CSS::ComputedValues const& NodeWithStyle::owned_computed_values() const
{
    VERIFY(m_owned_computed_values);
    return *m_owned_computed_values;
}

NonnullRefPtr<CSS::ComputedValues const> NodeWithStyle::copy_computed_values() const
{
    if (m_owned_computed_values)
        return *m_owned_computed_values;
    auto record_view = computed_style_record_view();
    VERIFY(record_view);
    return CSS::ComputedValues::Builder { *record_view }.build();
}

CSS::ComputedStyleRecordView NodeWithStyle::computed_style_record_view() const
{
    VERIFY(m_style_record_identity);
    return document().style_computer().computed_style_record_view(m_style_record_identity);
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

    if (Painting::has_committed_box(*m_owner))
        Painting::set_needs_repaint(*m_owner);
}

NodeWithStyle::~NodeWithStyle()
{
    clear_image_observers();
    release_pinned_style_record();
}

void NodeWithStyle::clear_image_observers()
{
    m_image_observers = {};
}

void NodeWithStyle::rebuild_image_observers()
{
    auto observer_for = [&](CSS::AbstractImageStyleValue const* abstract_image) -> OwnPtr<ImageObserver> {
        if (!abstract_image)
            return nullptr;
        auto const* image_to_observe = abstract_image->selected_image_style_value();
        if (!image_to_observe)
            return nullptr;
        return make<ImageObserver>(*this, *image_to_observe);
    };

    ImageObserverSlots new_observers;
    for (auto const& layer : background_layers())
        new_observers.background_layers.append(observer_for(layer.background_image.ptr()));
    for (auto const& layer : mask_layers())
        new_observers.mask_layers.append(observer_for(layer.background_image.ptr()));
    for (auto const& cursor_style_value : m_cursor_style_values)
        new_observers.cursors.append(cursor_style_value ? observer_for(&cursor_style_value->image()) : nullptr);
    new_observers.border_image_source = observer_for(border_image().source.ptr());
    new_observers.list_style_image = observer_for(list_style_image());
    // TODO: Observe other <image> accepting properties once we support them.

    // Register the new observers before the old ones unregister so a shared resource is never dropped and refetched.
    m_image_observers = move(new_observers);
}

static NodeWithStyle::ImageObserver const* image_observer_at(Vector<OwnPtr<NodeWithStyle::ImageObserver>> const& observers, size_t index)
{
    if (index >= observers.size())
        return nullptr;
    return observers[index].ptr();
}

NodeWithStyle::ImageObserver const* NodeWithStyle::background_image_observer(size_t layer_index) const
{
    return image_observer_at(m_image_observers.background_layers, layer_index);
}

NodeWithStyle::ImageObserver const* NodeWithStyle::mask_image_observer(size_t layer_index) const
{
    return image_observer_at(m_image_observers.mask_layers, layer_index);
}

NodeWithStyle::ImageObserver const* NodeWithStyle::cursor_image_observer(size_t cursor_index) const
{
    return image_observer_at(m_image_observers.cursors, cursor_index);
}

}

namespace Web::Layout {

void NodeWithStyle::apply_style(CSS::StyleRecordID style_record_identity)
{
    release_pinned_style_record();
    m_background_layers.clear();
    m_mask_layers.clear();
    m_border_image.clear();
    m_list_style_image.clear();
    m_owned_computed_values = nullptr;
    m_style_record_identity = style_record_identity;
    publish_style_record_to_node_data();
    auto record_view = computed_style_record_view();
    VERIFY(record_view);
    set_flag(RustFFI::NodeFlag::HasAnchorNames, !record_view->anchor_names().is_empty());
    set_flag(RustFFI::NodeFlag::InsetsUseAnchorFunctions, record_view->inset_properties_contain_anchor_functions());
    set_flag(RustFFI::NodeFlag::HasAnimatedOpacityOrTransform, false);
    set_flag(RustFFI::NodeFlag::HasPreserve3dTransformStyle, transform_style() == CSS::TransformStyle::Preserve3d);
    // A style change can introduce the properties that make a node carry replaced-content facts,
    // such as size containment arriving on a kept layout node.
    enroll_for_arena_replaced_content_facts_sync_if_eligible();
    propagate_style_to_anonymous_wrappers();
    attach_style_resources();
    // A pseudo layout node can outlive replacement of the DOM pseudo's record until the layout
    // tree is rebuilt. Root its record across that gap, including metadata-only style changes that
    // keep the existing layout node.
    if (is_generated_for_pseudo_element())
        pin_style_record_for_cxx_consumers();
}

void NodeWithStyle::attach_style_resources()
{
    auto load_image = [&](CSS::AbstractImageStyleValue const* image) {
        if (image)
            const_cast<CSS::AbstractImageStyleValue&>(*image).load_any_resources(*this);
    };

    for (auto const& layer : background_layers())
        load_image(layer.background_image.ptr());
    for (auto const& layer : mask_layers())
        load_image(layer.background_image.ptr());
    load_image(border_image().source.ptr());
    m_cursor_style_values.clear();
    m_cursor_style_values.ensure_capacity(cursor().size());
    for (auto const& cursor_data : cursor()) {
        auto cursor_style_value = CSS::ComputedValues::InheritedUIValues::cursor_style_value(cursor_data);
        if (cursor_style_value)
            load_image(&cursor_style_value->image());
        m_cursor_style_values.unchecked_append(move(cursor_style_value));
    }
    load_image(list_style_image());

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

void NodeWithStyle::propagate_style_to_anonymous_wrappers()
{
    // Update the style of any anonymous wrappers that inherit from this node.
    // FIXME: This is pretty hackish. It would be nicer if they shared the inherited style
    //        data structure somehow, so this wasn't necessary.

    // If this is a `display:table` box with an anonymous wrapper parent,
    // the parent inherits style from *this* node, not the other way around.
    if (auto* table_wrapper = parent() && parent()->is_table_wrapper() ? parent() : nullptr; table_wrapper && display().is_table_inside()) {
        CSS::ComputedValues::Builder builder(table_wrapper->owned_computed_values());
        auto values = copy_computed_values();
        builder->inherit_from(*values);
        transfer_table_box_computed_values_to_wrapper_computed_values(builder);
        table_wrapper->set_computed_values(move(builder).build());
    }

    // Propagate style to all anonymous children (except table wrappers!)
    for_each_child_of_type<NodeWithStyle>([&](NodeWithStyle& child) {
        if (child.is_anonymous() && !child.is_table_wrapper()) {
            // NB: The principal box of a pseudo-element (::before, ::after, ::marker, etc) has its own computed
            //     style, which is applied to it separately. Don't clobber that style with inherited values from
            //     this node.
            if (child.is_pseudo_element_principal_box())
                return IterationDecision::Continue;
            CSS::ComputedValues::Builder builder(child.owned_computed_values());
            auto values = copy_computed_values();
            builder->inherit_from(*values);
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
    return has_flag(RustFFI::NodeFlag::IsReplacedElement);
}

bool Node::is_atomic_inline() const
{
    return RustFFI::layout_node_data_is_atomic_inline(m_data);
}

bool Node::is_fragmented_inline() const
{
    return RustFFI::layout_node_data_is_fragmented_inline(m_data);
}

NodeWithStyle const* Node::nearest_fragmented_inline_ancestor() const
{
    for (auto const* ancestor = parent(); ancestor; ancestor = ancestor->parent()) {
        if (!ancestor->display().is_inline_outside() || !ancestor->display().is_flow_inside())
            break;
        if (ancestor->is_fragmented_inline())
            return static_cast<NodeWithStyle const*>(ancestor);
    }
    return nullptr;
}

// https://drafts.csswg.org/css-transforms-1/#transformable-element
// The used transform of an SVG element in its own user space, for bounding box computation:
// style transforms in property-application order plus the element's additional transform, without
// transform-origin conjugation. Percentages resolve against an empty reference box because the
// box is not available at layout time, so such transforms under-report the bounding box.
Gfx::AffineTransform NodeWithStyle::used_svg_element_transform() const
{
    auto matrix = Gfx::FloatMatrix4x4::identity();
    for_each_resolved_transform([&](auto const& transform) {
        matrix = matrix * transform.to_matrix({}, {});
    });
    auto transform = Gfx::extract_2d_affine_transform(matrix);
    if (auto const* graphics_element = as_if<SVG::SVGGraphicsElement>(dom_node()))
        transform.multiply(graphics_element->additional_element_transform());
    return transform;
}

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

// https://drafts.csswg.org/css-transforms-2/#grouping-property-values
CSS::TransformStyle NodeWithStyle::used_transform_style() const
{
    if (transform_style() == CSS::TransformStyle::Flat)
        return CSS::TransformStyle::Flat;

    // Keep this in sync with ComputedValues::has_transform_style_grouping_property().
    auto has_mask_layer_image = any_of(mask_layers(), [](auto const& layer) { return layer.background_image != nullptr; });
    bool has_transform_style_grouping_property = (overflow_x() != CSS::Overflow::Visible && overflow_x() != CSS::Overflow::Clip)
        || (overflow_y() != CSS::Overflow::Visible && overflow_y() != CSS::Overflow::Clip)
        || opacity() < 1
        || filter().has_filters()
        || !clip().is_auto()
        || clip_path().has_value()
        || isolation() == CSS::Isolation::Isolate
        || mask().has_value()
        || has_mask_layer_image
        || mix_blend_mode() != CSS::MixBlendMode::Normal
        || backdrop_filter().has_filters();
    if (has_transform_style_grouping_property)
        return CSS::TransformStyle::Flat;

    // contain: paint and any other property/value combination that causes paint containment.
    // FIXME: has_paint_containment() does not cover content-visibility: hidden, which also causes paint containment.
    if (has_paint_containment())
        return CSS::TransformStyle::Flat;

    return CSS::TransformStyle::Preserve3d;
}

bool NodeWithStyle::establishes_or_extends_a_3d_rendering_context() const
{
    return is_transformable() && used_transform_style() == CSS::TransformStyle::Preserve3d;
}

// https://drafts.csswg.org/css-transforms-2/#3d-rendering-contexts
bool NodeWithStyle::participates_in_a_3d_rendering_context() const
{
    // An element participates in a 3D rendering context if its parent establishes or extends a 3D rendering context.
    auto const* ancestor = parent();
    while (ancestor && ancestor->is_anonymous())
        ancestor = ancestor->parent();
    return ancestor && ancestor->establishes_or_extends_a_3d_rendering_context();
}

NonnullRefPtr<NodeWithStyle> NodeWithStyle::create_anonymous_wrapper() const
{
    auto values = copy_computed_values();
    auto builder = CSS::ComputedValues::Builder::create_inheriting_from(*values);
    builder->set_display(CSS::Display(CSS::DisplayOutside::Block, CSS::DisplayInside::Flow));
    // CSS 2.2 9.2.1.1 creates anonymous block boxes, but 9.4.1 states inline-block creates a BFC.
    // Set wrapper to inline-block to participate correctly in the IFC within the parent inline-block.
    if (display().is_inline_block() && !has_children())
        builder->set_display(CSS::Display::from_short(CSS::Display::Short::InlineBlock));
    auto wrapper = adopt_ref(*new BlockContainer(const_cast<DOM::Document&>(document()), nullptr, move(builder).build()));
    return *wrapper;
}

void NodeWithStyle::set_computed_values(NonnullRefPtr<CSS::ComputedValues const> computed_values)
{
    VERIFY(!layout_pass_currently_running());

    // Every path that lands computed values on a layout node funnels through here — element
    // restyles, inherited-style recomputation (including the animation fast path's descendant
    // walk), pseudo-element application, and anonymous wrapper propagation at any depth — so
    // this is the one place that can tell whether a style change can affect this box's layout.
    // Style-side layout inputs are exactly the layout-affecting group payloads published to
    // node data plus the animated-value overlay, which lives outside the groups and
    // disqualifies pointer diffing the same way it disqualifies the style differ's group
    // fast path.
    auto differs_from = [&](CSS::ComputedValues const& previous_values) {
        return CSS::ComputedValues::either_carries_animated_overlay(previous_values, *computed_values)
            || computed_values->differs_in_any_layout_affecting_group_payload_from(previous_values);
    };
    bool changes_layout_affecting_style = false;
    if (m_owned_computed_values)
        changes_layout_affecting_style = differs_from(*m_owned_computed_values);
    else if (auto record_view = computed_style_record_view())
        changes_layout_affecting_style = differs_from(*record_view);

    release_pinned_style_record();
    m_background_layers.clear();
    m_mask_layers.clear();
    m_border_image.clear();
    m_list_style_image.clear();
    Optional<DOM::AbstractElement> abstract_element;
    if (is_generated_for_pseudo_element())
        abstract_element = DOM::AbstractElement { *pseudo_element_generator(), generated_for_pseudo_element() };
    else if (auto* element = as_if<DOM::Element>(dom_node()))
        abstract_element = DOM::AbstractElement { *element };

    if (abstract_element.has_value()) {
        auto style_record_identity = document().style_computer().intern_computed_style_inputs(*abstract_element, *computed_values);
        if (!m_owned_computed_values && style_record_identity == m_style_record_identity) {
            m_owned_computed_values = nullptr;
        } else {
            m_style_record_identity = style_record_identity;
            m_owned_computed_values = computed_values;
        }
    } else {
        auto style_record_identity = document().style_computer().intern_anonymous_layout_style(*computed_values);
        m_style_record_identity = style_record_identity;
        m_owned_computed_values = computed_values;
    }
    set_flag(RustFFI::NodeFlag::HasAnchorNames, !computed_values->anchor_names().is_empty());
    set_flag(RustFFI::NodeFlag::InsetsUseAnchorFunctions, computed_values->inset_properties_contain_anchor_functions());
    set_flag(RustFFI::NodeFlag::HasAnimatedOpacityOrTransform, false);
    publish_style_record_to_node_data();
    set_flag(RustFFI::NodeFlag::HasPreserve3dTransformStyle, transform_style() == CSS::TransformStyle::Preserve3d);
    enroll_for_arena_replaced_content_facts_sync_if_eligible();
    if (m_owned_computed_values)
        pin_style_record_for_cxx_consumers();

    if (changes_layout_affecting_style) {
        bump_fragment_cache_epoch_of_self_and_ancestors();
        reset_cached_intrinsic_sizes_of_self_and_ancestors(*this);
    }

    for (auto* child = first_child_ptr(); child; child = child->next_sibling_ptr()) {
        if (auto* text_child = as_if<TextNode>(*child))
            text_child->enroll_for_arena_text_content_sync();
    }
}

void NodeWithStyle::set_style_record_identity(CSS::StyleRecordID style_record_identity)
{
    // A detached or layout-derived record is independent of its DOM target's record. A
    // rendering consequence replaces and re-derives it explicitly through apply_style().
    if (m_owned_computed_values)
        return;
    if (m_style_record_identity == style_record_identity) {
        publish_style_record_to_node_data();
        return;
    }

    bool should_repin_style_record = m_style_record_owner;
    auto new_record_view = document().style_computer().computed_style_record_view(style_record_identity);
    VERIFY(new_record_view);
    auto old_record_view = computed_style_record_view();
    bool changes_layout_affecting_style = !old_record_view
        || CSS::ComputedValues::either_carries_animated_overlay(*old_record_view, *new_record_view)
        || new_record_view->differs_in_any_layout_affecting_group_payload_from(*old_record_view);

    release_pinned_style_record();
    m_background_layers.clear();
    m_mask_layers.clear();
    m_border_image.clear();
    m_list_style_image.clear();
    m_owned_computed_values = nullptr;
    m_style_record_identity = style_record_identity;
    set_flag(RustFFI::NodeFlag::HasAnchorNames, !new_record_view->anchor_names().is_empty());
    set_flag(RustFFI::NodeFlag::InsetsUseAnchorFunctions, new_record_view->inset_properties_contain_anchor_functions());
    set_flag(RustFFI::NodeFlag::HasAnimatedOpacityOrTransform, false);
    publish_style_record_to_node_data();
    set_flag(RustFFI::NodeFlag::HasPreserve3dTransformStyle, transform_style() == CSS::TransformStyle::Preserve3d);
    enroll_for_arena_replaced_content_facts_sync_if_eligible();
    if (should_repin_style_record)
        pin_style_record_for_cxx_consumers();

    if (changes_layout_affecting_style) {
        bump_fragment_cache_epoch_of_self_and_ancestors();
        reset_cached_intrinsic_sizes_of_self_and_ancestors(*this);
    }
}

void NodeWithStyle::pin_style_record_for_cxx_consumers()
{
    if (m_style_record_owner)
        return;

    VERIFY(m_style_record_identity);
    auto& owner = document();
    owner.style_computer().pin_style_record(m_style_record_identity);
    m_style_record_owner = owner;
}

void NodeWithStyle::release_pinned_style_record()
{
    if (!m_style_record_owner)
        return;
    m_style_record_owner->style_computer().unpin_style_record(m_style_record_identity);
    m_style_record_owner = {};
}

void NodeWithStyle::bind_generated_style_record(CSS::StyleRecordID target_style_record_identity)
{
    VERIFY(is_generated_for_pseudo_element());
    if (!m_owned_computed_values) {
        set_style_record_identity(target_style_record_identity);
        return;
    }
    if (m_style_record_identity != target_style_record_identity)
        return;
    m_owned_computed_values = nullptr;
    publish_style_record_to_node_data();
}

static Node const* scroll_snap_container_of(NodeWithStyle const& node)
{
    // The scroll snap properties specified on the root element apply to the viewport rather than to its own box.
    if (node.is_viewport() || (node.dom_node() && node.dom_node() == node.document().document_element()))
        return node.document().unsafe_layout_node();
    if (!node.is_scroll_container())
        return nullptr;
    return &node;
}

void NodeWithStyle::publish_style_record_to_node_data()
{
    auto const* payloads = document().style_computer().style_engine().style_record_payloads(m_style_record_identity);
    VERIFY(payloads);
    node_data().style = payloads;
    if (content_visibility() == CSS::ContentVisibility::Auto)
        document().note_content_visibility_auto_style();

    if (scroll_snap_type().strictness != CSS::ScrollSnapStrictness::None)
        document().set_may_have_scroll_snap_areas();

    // NB: The root element's style can be published before the layout tree gives the document a viewport to snap
    //     with, and is published again once building the layout tree binds this node's style record.
    auto const* snap_container = scroll_snap_container_of(*this);
    if (!snap_container)
        return;

    // A style change can make a box a snap container without the paint tree being built again, so the box registers
    // itself here as well as when it is built.
    if (Painting::is_scroll_snap_container(*snap_container)) {
        document().register_scroll_snap_container(*snap_container);
        return;
    }

    // A box that does not snap is snapped to no snap areas, so that a scroll it is given while it does not snap is not
    // undone by a re-snap once it snaps again.
    document().forget_snapped_areas_of_scroll_container(*snap_container);
}

bool NodeWithStyle::synchronize_table_span_data()
{
    u16 column_span = 1;
    u16 row_span = 1;
    u32 raw_column_span = 1;
    if (auto const* node = dom_node()) {
        if (auto const* cell = as_if<HTML::HTMLTableCellElement>(*node)) {
            column_span = static_cast<u16>(cell->col_span());
            row_span = static_cast<u16>(cell->row_span());
        } else if (auto const* column = as_if<HTML::HTMLTableColElement>(*node)) {
            column_span = static_cast<u16>(column->span());
            // The raw span keeps the unclamped attribute value; its only consumer is the
            // table formatting context's column handling, so other elements' span
            // attributes stay out of the arena map.
            raw_column_span = column->get_attribute_value(HTML::AttributeNames::span).to_number<u32>().value_or(1);
        }
    }
    bool effective_spans_changed = node_data().table_column_span != column_span
        || node_data().table_row_span != row_span;
    node_data().table_column_span = column_span;
    node_data().table_row_span = row_span;
    u32 previous_raw_column_span = RustFFI::layout_arena_set_raw_table_column_span(arena_handle(), slot_id(this), raw_column_span);
    return effective_spans_changed || previous_raw_column_span != raw_column_span;
}

void NodeWithStyle::set_display(CSS::Display display)
{
    modify_computed_values([&](auto& values) {
        values.set_display(display);
    });
}

void NodeWithStyle::set_content(CSS::ContentData const& content)
{
    m_content = content;
}

void NodeWithStyle::set_overflow(CSS::Overflow overflow_x, CSS::Overflow overflow_y)
{
    modify_computed_values([&](auto& values) {
        values.set_overflow_x(overflow_x);
        values.set_overflow_y(overflow_y);
    });
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
        values.reset_grid_placements_to_auto();
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
        values.set_vertical_align(CSS::InitialValues::vertical_align());
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
    builder->set_position(position());
    builder->set_position_anchor(position_anchor_value());
    builder->set_inset(inset());
    builder->set_float(float_());
    builder->set_clear(clear());
    // CSS 2 moves table-root positioning and margins to the wrapper. The wrapper is also the grid item for
    // display:table, so grid placement, self-alignment, and order need to move there as well.
    builder->copy_grid_placements_from(style_group<CSS::ComputedValues::GridValues>());
    builder->set_align_self(align_self());
    builder->set_justify_self(justify_self());
    builder->set_order(order());
    builder->set_margin(margin());
    // AD-HOC:
    // To match other browsers, z-index needs to be moved to the wrapper box as well,
    // even if the spec does not mention that: https://github.com/w3c/csswg-drafts/issues/11689
    // Note that there may be more properties that need to be added to this list.
    builder->set_z_index(z_index());
    // "clip" only takes effect on absolutely-positioned elements; the table box isn't one — the wrapper is.
    builder->set_clip(clip());
    // AD-HOC: The wrapper box participates in inline layout in place of the table box, so vertical-align
    //         must be moved to the wrapper to have any effect.
    builder->set_vertical_align(vertical_align());

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

    return overflow_value_makes_box_a_scroll_container(overflow_x())
        || overflow_value_makes_box_a_scroll_container(overflow_y());
}

void Node::clear_committed_box()
{
    invalidate_paint_caches(*this);
    RustFFI::layout_arena_paintable_cleared_from_node(arena_handle(), slot_id(this));
}

DOM::Node const* Node::dom_node() const
{
    if (is_anonymous())
        return nullptr;
    VERIFY(m_dom_node);
    return m_dom_node.ptr();
}

DOM::Node* Node::dom_node()
{
    if (is_anonymous())
        return nullptr;
    VERIFY(m_dom_node);
    return m_dom_node.ptr();
}

GC::Ptr<DOM::Element const> Node::pseudo_element_generator() const
{
    VERIFY(is_generated_for_pseudo_element());
    VERIFY(m_pseudo_element_generator);
    return m_pseudo_element_generator.ptr();
}

GC::Ptr<DOM::Element> Node::pseudo_element_generator()
{
    VERIFY(is_generated_for_pseudo_element());
    VERIFY(m_pseudo_element_generator);
    return m_pseudo_element_generator.ptr();
}

void Node::set_generated_for(CSS::PseudoElement type, DOM::Element& element)
{
    static_assert(encode_generated_for(CSS::PseudoElement::After) == RustFFI::GENERATED_FOR_AFTER);
    static_assert(encode_generated_for(CSS::PseudoElement::Marker) == RustFFI::GENERATED_FOR_MARKER);
    m_data->generated_for = encode_generated_for(type);
    m_pseudo_element_generator = element;
    if (auto* node_with_style = as_if<NodeWithStyle>(*this))
        node_with_style->bind_generated_style_record(element.style_record_identity(type));
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

    auto const* style_source = as_if<NodeWithStyle>(*this);
    if (!style_source)
        style_source = parent();
    auto computed_value = style_source->user_select();
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

    if (contain().size_containment)
        return true;

    if (container_type().is_size_container)
        return true;

    return false;
}
// https://drafts.csswg.org/css-contain-2/#containment-layout
bool NodeWithStyle::has_layout_containment() const
{
    auto has_layout_containment = contain().layout_containment;

    // https://drafts.csswg.org/css-contain-2/#valdef-content-visibility-auto
    // Changes the used value of the 'contain' property so as to turn on layout containment, style containment, and
    // paint containment for the element.
    has_layout_containment = has_layout_containment || content_visibility() == CSS::ContentVisibility::Auto;
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

    if (contain().style_containment)
        return true;

    if (container_type().is_size_container || container_type().is_inline_size_container)
        return true;

    // https://drafts.csswg.org/css-contain-2/#valdef-content-visibility-auto
    // Changes the used value of the 'contain' property so as to turn on layout containment, style containment, and
    // paint containment for the element.
    if (content_visibility() == CSS::ContentVisibility::Auto)
        return true;

    return false;
}
// https://drafts.csswg.org/css-contain-2/#containment-paint
bool NodeWithStyle::has_paint_containment() const
{
    auto has_paint_containment = contain().paint_containment;

    // https://drafts.csswg.org/css-contain-2/#valdef-content-visibility-auto
    // Changes the used value of the 'contain' property so as to turn on layout containment, style containment, and
    // paint containment for the element.
    has_paint_containment = has_paint_containment || content_visibility() == CSS::ContentVisibility::Auto;
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

void Node::set_needs_layout_update(DOM::SetNeedsLayoutReason reason, LayoutUpdatePropagation propagation)
{
    // Bumped before the already-dirty early return below: a dirty node does not imply its
    // whole ancestor chain was bumped for the current epoch values, and over-bumping is free.
    bump_fragment_cache_epoch_of_self_and_ancestors();

    if (needs_layout_update() && propagation == LayoutUpdatePropagation::ThroughAncestors) {
        // A dirty node normally implies dirty ancestors, but the walk that marked a partial
        // relayout boundary stopped there and left its ancestors clean, so a through-ancestors
        // invalidation arriving on the boundary itself must still walk and mark them.
        auto* box = as_if<Box>(this);
        if (!box || !box->is_partial_relayout_boundary())
            return;
    }

    if (!needs_layout_update()) {
        if constexpr (UPDATE_LAYOUT_DEBUG) {
            // NOTE: We check some conditions here to avoid debug spam in documents that don't do layout.
            auto navigable = this->navigable();
            if (navigable && navigable->active_document() == GC::Ptr { &document() })
                dbgln_if(UPDATE_LAYOUT_DEBUG, "NEED LAYOUT {}", DOM::to_string(reason));
        }

        set_flag(RustFFI::NodeFlag::NeedsLayoutUpdate, true);
        // Relayout may rebuild an identical fragment whose cached paint output the commit diff
        // then keeps, even when what this node paints changed (its image data arrived).
        invalidate_paint_caches(*this);
    }

    if (auto* box = as_if<Box>(this))
        box->reset_cached_intrinsic_sizes();

    // Mark any anonymous children generated by this node for layout update.
    // NOTE: if this node generated an anonymous parent, all ancestors are indiscriminately marked below.
    for_each_child_of_type<Box>([&](Box& child) {
        if (child.is_anonymous() && !child.is_table_wrapper()) {
            child.bump_fragment_cache_epoch();
            child.set_flag(RustFFI::NodeFlag::NeedsLayoutUpdate, true);
            child.reset_cached_intrinsic_sizes();
        }
        return IterationDecision::Continue;
    });

    if (propagation == LayoutUpdatePropagation::BoundarySelfOnly) {
        document().partial_relayout_invalidation().record_boundary(as<Box>(*this));
        return;
    }

    for (auto* ancestor = parent(); ancestor; ancestor = ancestor->parent()) {
        if (ancestor->needs_layout_update())
            break;
        ancestor->set_flag(RustFFI::NodeFlag::NeedsLayoutUpdate, true);
        if (auto* box = as_if<Box>(ancestor); box && box->is_partial_relayout_boundary()) {
            document().partial_relayout_invalidation().record_boundary(*box);
            break;
        }
    }

    reset_cached_intrinsic_sizes_of_ancestors(*this);
}

}
