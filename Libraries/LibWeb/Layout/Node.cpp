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

static RustFFI::FfiNodeConstructionFacts build_node_construction_facts(DOM::Document& document, GC::Ptr<DOM::Node> node, RustFFI::NodeKind kind, void* shell)
{
    return {
        .kind = kind,
        .shell = shell,
        .dom_node = node.ptr(),
        .is_anonymous = node == nullptr,
        .is_html_input_element = node && is<HTML::HTMLInputElement>(*node),
        .is_html_html_element = node && node->is_html_html_element(),
        .is_document_element = node && node.ptr() == document.document_element(),
        .is_in_user_agent_shadow_tree = node && node->containing_shadow_root() && node->containing_shadow_root()->is_user_agent_internal(),
        .uses_button_layout = node && is<HTML::HTMLElement>(*node) && static_cast<HTML::HTMLElement const&>(*node).uses_button_layout(),
        .is_editing_host = node && node->is_editing_host(),
        .is_body = node && node == GC::Ptr { document.body() },
    };
}

Node::Node(DOM::Document& document, GC::Ptr<DOM::Node> node, RustFFI::NodeKind kind, AttachToDOMNode attach_to_dom_node)
    : m_arena(document.layout_node_arena())
    , m_slot(m_arena->allocate(build_node_construction_facts(document, node, kind, this)))
    , m_dom_node(node)
    , m_kind(kind)
{
    VERIFY(RustFFI::layout_arena_node_dom_node(m_arena->handle(), m_slot) == m_dom_node.ptr());
    update_has_scroll_offset_flag();

    if (node && attach_to_dom_node == AttachToDOMNode::Yes)
        node->set_layout_node({}, *this);
}

Node::Node(DOM::Document& document, BindToPreparedArenaSlot, RustFFI::NodeSlotId slot, RustFFI::NodeKind kind)
    : m_arena(document.layout_node_arena())
    , m_slot(slot)
    , m_kind(kind)
{
    VERIFY(RustFFI::layout_arena_node_dom_node(m_arena->handle(), m_slot) == nullptr);
    RustFFI::layout_arena_attach_shell(m_arena->handle(), m_slot, this);
}

Node::~Node()
{
    VERIFY(m_arena_is_destroying_shell);
}

void Node::delete_arena_owned_shell(Node& node)
{
    node.m_arena_is_destroying_shell = true;
    delete &node;
}

RustFFI::NodeSlotId Node::slot_id(Node const* node)
{
    return node ? node->m_slot : RustFFI::NodeSlotId_INVALID;
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
        LAYOUT_NODE_KIND_NAME_CASE(VideoBox)
        LAYOUT_NODE_KIND_NAME_CASE(Viewport)
    case RustFFI::NodeKind::Unset:
        break;
    }
#undef LAYOUT_NODE_KIND_NAME_CASE
    VERIFY_NOT_REACHED();
}

void Node::bump_fragment_cache_epoch_of_self_and_ancestors()
{
    RustFFI::layout_arena_bump_fragment_cache_epoch_of_self_and_ancestors(arena_handle(), slot_id(this));
}

void* Node::arena_handle() const
{
    return m_arena->handle();
}

Box const* Node::containing_block() const
{
    return static_cast<Box const*>(containing_block_node_if_live());
}

Box* Node::containing_block()
{
    return static_cast<Box*>(containing_block_node_if_live());
}

void Node::pin_style_record_for_detachment()
{
    if (auto* node_with_style = as_if<NodeWithStyle>(*this))
        node_with_style->pin_style_record_for_cxx_consumers();
}

void Node::prepare_for_detach_from_layout_tree()
{
    pin_style_record_for_detachment();
    Painting::invalidate_paint_cache(*this);
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

bool NodeWithStyle::establishes_an_absolute_positioning_containing_block() const
{
    return RustFFI::layout_arena_node_establishes_an_absolute_positioning_containing_block(arena_handle(), Node::slot_id(this));
}

bool NodeWithStyle::establishes_a_fixed_positioning_containing_block() const
{
    return RustFFI::layout_arena_node_establishes_a_fixed_positioning_containing_block(arena_handle(), Node::slot_id(this));
}

bool NodeWithStyle::has_css_transform() const
{
    return RustFFI::layout_arena_node_has_css_transform(arena_handle(), Node::slot_id(this));
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
    : Node(document, node, kind)
{
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
    initialize_from_style_record();
    if (m_owned_computed_values)
        pin_style_record_for_cxx_consumers();
}

NodeWithStyle::NodeWithStyle(DOM::Document& document, BindToPreparedArenaSlot bind, RustFFI::NodeSlotId slot, RustFFI::NodeKind kind)
    : Node(document, bind, slot, kind)
{
    m_style_record_identity = CSS::StyleRecordID { RustFFI::layout_arena_node_style_record(arena_handle(), slot) };
    VERIFY(m_style_record_identity);
    m_style_payloads = RustFFI::layout_arena_node_style_payloads(arena_handle(), slot);
    VERIFY(m_style_payloads);
}

void NodeWithStyle::initialize_from_style_record()
{
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
    m_list_style_type.clear();
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
    RustFFI::layout_arena_reinherit_anonymous_descendants(arena_handle(), slot_id(this));
    attach_style_resources();
    // A pseudo layout node can outlive replacement of the DOM pseudo's record until the layout
    // tree is rebuilt. Root its record across that gap, including metadata-only style changes that
    // keep the existing layout node.
    if (is_generated_for_pseudo_element())
        pin_style_record_for_cxx_consumers();
}

void NodeWithStyle::attach_style_resources()
{
    // The style engine notes at publication whether a record holds an <image> anywhere this node would load and
    // observe one. Nearly every style holds none, and that answer is one flag read; the walk below stays for the
    // styles that do.
    auto dependency_flags = document().style_computer().style_engine().style_record_dependency_flags(m_style_record_identity);
    if (!(dependency_flags & to_underlying(CSS::StyleRecordDependencyFlag::HoldsImageValues))) {
        m_cursor_style_values.clear();
        clear_image_observers();
        return;
    }

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

void NodeWithStyle::refresh_style_from_arena()
{
    m_style_record_identity = CSS::StyleRecordID { RustFFI::layout_arena_node_style_record(arena_handle(), slot_id(this)) };
    VERIFY(m_style_record_identity);
    m_background_layers.clear();
    m_mask_layers.clear();
    m_border_image.clear();
    m_list_style_type.clear();
    m_list_style_image.clear();
    auto record_view = computed_style_record_view();
    VERIFY(record_view);
    set_flag(RustFFI::NodeFlag::HasAnchorNames, !record_view->anchor_names().is_empty());
    set_flag(RustFFI::NodeFlag::InsetsUseAnchorFunctions, record_view->inset_properties_contain_anchor_functions());
    set_flag(RustFFI::NodeFlag::HasAnimatedOpacityOrTransform, false);
    publish_style_record_to_node_data();
    set_flag(RustFFI::NodeFlag::HasPreserve3dTransformStyle, transform_style() == CSS::TransformStyle::Preserve3d);
}

bool NodeWithStyle::reinherit_owned_computed_values_from(CSS::StyleRecordID parent_style_record_identity)
{
    // NB: The principal box of a pseudo-element (::before, ::after, ::marker, etc) has its own computed
    //     style, which is applied to it separately. Don't clobber that style with inherited values from
    //     the parent.
    if (is_pseudo_element_principal_box())
        return false;
    // A box generated for a pseudo-element's content (a marker's image box) was created
    // on the pseudo-element's own record rather than on inherited values: it follows
    // its principal box's record.
    if (is_generated_for_pseudo_element() && !m_owned_computed_values) {
        auto* parent = this->parent();
        if (parent && parent->is_pseudo_element_principal_box() && parent->generated_for_pseudo_element() == generated_for_pseudo_element())
            apply_style(parent_style_record_identity);
        return false;
    }
    auto parent_record_view = document().style_computer().computed_style_record_view(parent_style_record_identity);
    VERIFY(parent_record_view);
    CSS::ComputedValues::Builder builder(owned_computed_values());
    builder->inherit_from(*parent_record_view);
    set_computed_values(move(builder).build());
    return true;
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

bool Node::is_atomic_inline() const
{
    return RustFFI::layout_arena_node_is_atomic_inline(arena_handle(), slot_id(this));
}

bool Node::is_fragmented_inline() const
{
    return RustFFI::layout_arena_node_is_fragmented_inline(arena_handle(), slot_id(this));
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
    m_list_style_type.clear();
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
    if (m_owned_computed_values)
        pin_style_record_for_cxx_consumers();

    if (changes_layout_affecting_style) {
        bump_fragment_cache_epoch_of_self_and_ancestors();
        RustFFI::layout_arena_reset_cached_intrinsic_sizes_of_self_and_ancestors(arena_handle(), slot_id(this));
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

    bool should_repin_style_record = m_style_record_pinned;
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
    m_list_style_type.clear();
    m_list_style_image.clear();
    m_owned_computed_values = nullptr;
    m_style_record_identity = style_record_identity;
    set_flag(RustFFI::NodeFlag::HasAnchorNames, !new_record_view->anchor_names().is_empty());
    set_flag(RustFFI::NodeFlag::InsetsUseAnchorFunctions, new_record_view->inset_properties_contain_anchor_functions());
    set_flag(RustFFI::NodeFlag::HasAnimatedOpacityOrTransform, false);
    publish_style_record_to_node_data();
    set_flag(RustFFI::NodeFlag::HasPreserve3dTransformStyle, transform_style() == CSS::TransformStyle::Preserve3d);
    if (should_repin_style_record)
        pin_style_record_for_cxx_consumers();

    if (changes_layout_affecting_style) {
        bump_fragment_cache_epoch_of_self_and_ancestors();
        RustFFI::layout_arena_reset_cached_intrinsic_sizes_of_self_and_ancestors(arena_handle(), slot_id(this));
    }
}

void NodeWithStyle::pin_style_record_for_cxx_consumers()
{
    if (m_style_record_pinned)
        return;

    VERIFY(m_style_record_identity);
    document().style_computer().pin_style_record(m_style_record_identity);
    m_style_record_pinned = true;
}

void NodeWithStyle::release_pinned_style_record()
{
    if (!m_style_record_pinned)
        return;
    document().style_computer().unpin_style_record(m_style_record_identity);
    m_style_record_pinned = false;
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
    m_style_payloads = payloads;
    RustFFI::layout_arena_set_node_style(arena_handle(), slot_id(this), m_style_record_identity.value(), payloads);
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
    return RustFFI::layout_arena_set_table_spans(arena_handle(), slot_id(this), column_span, row_span, raw_column_span);
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
    static_assert(encode_generated_for(CSS::PseudoElement::FirstLetter) == RustFFI::GENERATED_FOR_FIRST_LETTER);
    static_assert(encode_generated_for(CSS::PseudoElement::Marker) == RustFFI::GENERATED_FOR_MARKER);
    RustFFI::layout_arena_set_node_generated_for(arena_handle(), slot_id(this), encode_generated_for(type));
    m_pseudo_element_generator = element;
    if (auto* node_with_style = as_if<NodeWithStyle>(*this))
        node_with_style->bind_generated_style_record(element.style_record_identity(type));
}

// An element's box holds the element's scroll offset. Everything generated for a pseudo-element
// names it as generator, but only the pseudo-element's own box is what scrolls, so only that box
// holds the pseudo-element's offset; the generated content inside it holds none.
bool Node::dom_target_stores_scroll_offset() const
{
    if (auto pseudo_element = generated_for_pseudo_element(); pseudo_element.has_value()) {
        auto synthetic_pseudo_element = pseudo_element_generator()->get_synthetic_pseudo_element(*pseudo_element);
        return synthetic_pseudo_element.has_value()
            && synthetic_pseudo_element->unsafe_layout_node() == this
            && !synthetic_pseudo_element->scroll_offset().is_zero();
    }
    if (auto const* element = as_if<DOM::Element>(dom_node()))
        return !element->scroll_offset({}).is_zero();
    return false;
}

void Node::update_has_scroll_offset_flag()
{
    set_flag(RustFFI::NodeFlag::HasScrollOffset, dom_target_stores_scroll_offset());
}

void Node::verify_has_scroll_offset_flag() const
{
    VERIFY(has_flag(RustFFI::NodeFlag::HasScrollOffset) == dom_target_stores_scroll_offset());
}

DOM::Document& Node::document()
{
    VERIFY(m_arena->document());
    return *m_arena->document();
}

DOM::Document const& Node::document() const
{
    VERIFY(m_arena->document());
    return *m_arena->document();
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
void Node::set_needs_layout_update(DOM::SetNeedsLayoutReason reason, LayoutUpdatePropagation propagation)
{
    if constexpr (UPDATE_LAYOUT_DEBUG) {
        // NOTE: We check some conditions here to avoid debug spam in documents that don't do layout.
        if (!needs_layout_update()) {
            auto navigable = this->navigable();
            if (navigable && navigable->active_document() == GC::Ptr { &document() })
                dbgln_if(UPDATE_LAYOUT_DEBUG, "NEED LAYOUT {}", DOM::to_string(reason));
        }
    }
    RustFFI::layout_arena_set_needs_layout_update(arena_handle(), slot_id(this),
        propagation == LayoutUpdatePropagation::ThroughAncestors);
}

}
