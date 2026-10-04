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
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Dump.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/NodeArena.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/PaintFacts.h>
#include <LibWeb/Painting/ScrollSnap.h>
#include <LibWeb/SVG/SVGClipPathElement.h>
#include <LibWeb/SVG/SVGFilterElement.h>
#include <LibWeb/SVG/SVGGradientElement.h>
#include <LibWeb/SVG/SVGPatternElement.h>
#include <LibWeb/SVG/SVGTextContentElement.h>

namespace Web::Layout {

u8 Node::dom_paint_facts_of(DOM::Node const* node)
{
    if (!node)
        return 0;
    u8 facts = 0;
    if (node->is_inert())
        facts |= static_cast<u8>(RustFFI::DomPaintFact::Inert);
    if (node->is_editable_or_editing_host())
        facts |= static_cast<u8>(RustFFI::DomPaintFact::EditableOrEditingHost);
    if (node->inside_blocking_wheel_event_handler())
        facts |= static_cast<u8>(RustFFI::DomPaintFact::InsideBlockingWheelEventHandler);
    if (auto const* navigable_container = as_if<HTML::NavigableContainer>(*node); navigable_container && navigable_container->content_navigable())
        facts |= static_cast<u8>(RustFFI::DomPaintFact::NestedNavigableContainer);
    return facts;
}

// The StyleNodeID a row bound to this DOM node records: an element's or a text node's.
CSS::StyleNodeID Node::style_node_of(DOM::Node const* node)
{
    if (auto const* element = as_if<DOM::Element>(node))
        return element->style_node_id();
    if (auto const* text = as_if<DOM::Text>(node))
        return text->style_node_id();
    return {};
}

// The build stamps a row out of its node's identity, binds it to the node, and makes the layout node for it here. A row
// stamped for no node at all is an anonymous box.
Node::Node(DOM::Document& document, BindToPreparedArenaSlot, Compositing::RustFFI::NodeSlotId slot, RustFFI::NodeKind kind)
    : m_arena(document.layout_node_arena())
    , m_slot(slot)
    , m_kind(kind)
{
    RustFFI::document_host_attach_shell(m_arena->host(), m_slot, this);
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

Compositing::RustFFI::NodeSlotId Node::slot_id(Node const* node)
{
    return node ? node->m_slot : Compositing::RustFFI::NodeSlotId_INVALID;
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

void Node::reset_cached_intrinsic_sizes_of_self_and_ancestors()
{
    RustFFI::render_state_reset_cached_intrinsic_sizes_of_self_and_ancestors(document_host(), slot_id(this));
}

RustFFI::DocumentHost* Node::document_host() const
{
    return m_arena->host();
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
    RustFFI::render_state_prepare_node_for_detach(document_host(), slot_id(this));
}

void Node::prepare_subtree_for_detach_from_layout_tree()
{
    RustFFI::render_state_prepare_subtree_for_detach(document_host(), slot_id(this));
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

// The flag is set on the box a pseudo-element is bound to and cleared when that binding moves, so
// it answers without resolving the generator on the DOM side.
bool Node::is_pseudo_element_principal_box() const
{
    return has_flag(RustFFI::NodeFlag::IsPseudoElementPrincipalBox);
}

bool NodeWithStyle::establishes_an_absolute_positioning_containing_block() const
{
    return RustFFI::layout_row_establishes_an_absolute_positioning_containing_block(document_host(), Node::slot_id(this));
}

bool NodeWithStyle::establishes_a_fixed_positioning_containing_block() const
{
    return RustFFI::layout_row_establishes_a_fixed_positioning_containing_block(document_host(), Node::slot_id(this));
}

bool NodeWithStyle::has_css_transform() const
{
    return RustFFI::layout_row_has_css_transform(document_host(), Node::slot_id(this));
}

GC::Ptr<HTML::LocalNavigable> Node::navigable() const
{
    return document().navigable();
}

Viewport& Node::root()
{
    auto const& read = held_read();
    // NB: Called during layout, which is in progress.
    VERIFY(document().unsafe_layout_node(read));
    return *document().unsafe_layout_node(read);
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

// The build stamped the row with its style, which this layout node reads off the row, and told the document what that
// style asks for. A layout node is made whenever something first asks for it, so it does nothing but bind.
NodeWithStyle::NodeWithStyle(DOM::Document& document, BindToPreparedArenaSlot bind, Compositing::RustFFI::NodeSlotId slot, RustFFI::NodeKind kind)
    : Node(document, bind, slot, kind)
{
    m_style_record_identity = CSS::StyleRecordID { RustFFI::layout_row_style_record(document_host(), slot) };
    VERIFY(m_style_record_identity);
    // The layout node reads its style through the row's payloads, which the row's live record keeps.
    m_style_payloads = RustFFI::layout_row_style_payloads(document_host(), slot);
    VERIFY(m_style_payloads);
    m_has_layout_derived_style = RustFFI::layout_row_style_is_derived(document_host(), slot);
}

NonnullRefPtr<CSS::ComputedValues const> NodeWithStyle::copy_computed_values() const
{
    auto record_view = computed_style_record_view();
    VERIFY(record_view);
    return CSS::ComputedValues::Builder { *record_view }.build();
}

CSS::ComputedStyleRecordView NodeWithStyle::computed_style_record_view() const
{
    auto const& read = held_read();
    VERIFY(m_style_record_identity);
    return document().style_computer().computed_style_record_view(read, m_style_record_identity);
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
    Painting::push_layer_image_paint_facts(*m_owner);
}

NodeWithStyle::~NodeWithStyle()
{
    // NB: The arena destroys a shell only after it has freed the shell's row, which destroyed the
    //     row's image observers and released its host-pinned style record. Nothing is left to drop
    //     by slot, and asking would reach whichever row holds the slot next.
}

void NodeWithStyle::clear_image_observers()
{
    delete static_cast<ImageObserverSlots*>(RustFFI::document_host_replace_image_observers(document_host(), slot_id(this), nullptr));
}

void NodeWithStyle::rebuild_image_observers(Vector<RefPtr<CSS::CursorStyleValue const>> cursor_style_values)
{
    auto observer_for = [&](CSS::AbstractImageStyleValue const* abstract_image) -> OwnPtr<ImageObserver> {
        if (!abstract_image)
            return nullptr;
        auto const* image_to_observe = abstract_image->selected_image_style_value();
        if (!image_to_observe)
            return nullptr;
        return make<ImageObserver>(*this, *image_to_observe);
    };

    auto new_observers = make<ImageObserverSlots>();
    for (auto const& layer : background_layers())
        new_observers->background_layers.append(observer_for(layer.background_image.ptr()));
    for (auto const& layer : mask_layers())
        new_observers->mask_layers.append(observer_for(layer.background_image.ptr()));
    for (auto const& cursor_style_value : cursor_style_values)
        new_observers->cursors.append(cursor_style_value ? observer_for(&cursor_style_value->image()) : nullptr);
    new_observers->border_image_source = observer_for(border_image().source.ptr());
    new_observers->list_style_image = observer_for(list_style_image());
    new_observers->cursor_style_values = move(cursor_style_values);
    // TODO: Observe other <image> accepting properties once we support them.

    // Register the new observers before the old ones unregister so a shared resource is never dropped and refetched.
    delete static_cast<ImageObserverSlots*>(RustFFI::document_host_replace_image_observers(document_host(), slot_id(this), new_observers.leak_ptr()));
}

NodeWithStyle::ImageObserverSlots const* NodeWithStyle::image_observers() const
{
    return static_cast<ImageObserverSlots const*>(RustFFI::document_host_image_observers(document_host(), slot_id(this)));
}

ReadonlySpan<RefPtr<CSS::CursorStyleValue const>> NodeWithStyle::cursor_style_values() const
{
    if (auto const* observers = image_observers())
        return observers->cursor_style_values;
    return {};
}

static NodeWithStyle::ImageObserver const* image_observer_at(Vector<OwnPtr<NodeWithStyle::ImageObserver>> const& observers, size_t index)
{
    if (index >= observers.size())
        return nullptr;
    return observers[index].ptr();
}

NodeWithStyle::ImageObserver const* NodeWithStyle::background_image_observer(size_t layer_index) const
{
    auto const* observers = image_observers();
    return observers ? image_observer_at(observers->background_layers, layer_index) : nullptr;
}

NodeWithStyle::ImageObserver const* NodeWithStyle::mask_image_observer(size_t layer_index) const
{
    auto const* observers = image_observers();
    return observers ? image_observer_at(observers->mask_layers, layer_index) : nullptr;
}

NodeWithStyle::ImageObserver const* NodeWithStyle::cursor_image_observer(size_t cursor_index) const
{
    auto const* observers = image_observers();
    return observers ? image_observer_at(observers->cursors, cursor_index) : nullptr;
}

NodeWithStyle::ImageObserver const* NodeWithStyle::border_image_source_observer() const
{
    auto const* observers = image_observers();
    return observers ? observers->border_image_source.ptr() : nullptr;
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
    m_style_record_identity = style_record_identity;
    publish_style_record_to_node_data();
    set_flag(RustFFI::NodeFlag::HasAnimatedOpacityOrTransform, false);
    // A style change can introduce the properties that make a node carry replaced-content facts,
    // such as size containment arriving on a kept layout node.
    RustFFI::render_state_reinherit_anonymous_descendants(document_host(), slot_id(this));
    attach_style_resources();
    // A pseudo layout node can outlive replacement of the DOM pseudo's record until the layout
    // tree is rebuilt. Root its record across that gap, including metadata-only style changes that
    // keep the existing layout node.
    if (is_generated_for_pseudo_element())
        pin_style_record_for_cxx_consumers();
}

static bool style_record_holds_image_values(Layout::BegunRead const& read, CSS::StyleEngine const& style_engine, CSS::StyleRecordID style_record)
{
    return has_flag(style_engine.style_record_dependency_flags(read, style_record), CSS::StyleRecordDependencyFlag::HoldsImageValues);
}

void NodeWithStyle::attach_style_resources()
{
    auto const& read = held_read();
    // The style engine notes at publication whether a record holds an <image> anywhere this node would load and
    // observe one. Nearly every style holds none, and that answer is one flag read; the walk below stays for the
    // styles that do.
    if (!style_record_holds_image_values(read, document().style_computer().style_engine(), m_style_record_identity)) {
        clear_image_observers();
        Painting::push_paint_facts_after_style_attach(*this, Painting::StyleHoldsImageValues::No);
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
    Vector<RefPtr<CSS::CursorStyleValue const>> cursor_style_values;
    cursor_style_values.ensure_capacity(cursor().size());
    for (auto const& cursor_data : cursor()) {
        auto cursor_style_value = CSS::ComputedValues::InheritedUIValues::cursor_style_value(cursor_data);
        if (cursor_style_value)
            load_image(&cursor_style_value->image());
        cursor_style_values.unchecked_append(move(cursor_style_value));
    }
    load_image(list_style_image());

    rebuild_image_observers(move(cursor_style_values));
    Painting::push_paint_facts_after_style_attach(*this, Painting::StyleHoldsImageValues::Yes);
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

void NodeWithStyle::refresh_style_from_arena(CSS::StyleRecordID record, void const* payloads, bool derived, bool should_attach_resources)
{
    release_pinned_style_record();
    m_style_record_identity = record;
    m_style_payloads = payloads;
    m_has_layout_derived_style = derived;
    m_background_layers.clear();
    m_mask_layers.clear();
    m_border_image.clear();
    m_list_style_type.clear();
    m_list_style_image.clear();
    did_update_style_record();
    if (should_attach_resources)
        attach_style_resources();
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
    return RustFFI::layout_row_is_atomic_inline(document_host(), slot_id(this));
}

bool Node::is_fragmented_inline() const
{
    return RustFFI::layout_row_is_fragmented_inline(document_host(), slot_id(this));
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

void NodeWithStyle::set_computed_values(Layout::BegunRead const& read, NonnullRefPtr<CSS::ComputedValues const> computed_values)
{
    VERIFY(!RustFFI::render_state_layout_pass_is_running(document_host()));
    CSS::StyleRecordID record;
    if (is_generated_for_pseudo_element())
        record = document().style_computer().intern_computed_style_inputs(read, { *pseudo_element_generator(), generated_for_pseudo_element() }, *computed_values);
    else if (auto* element = as_if<DOM::Element>(dom_node()))
        record = document().style_computer().intern_computed_style_inputs(read, { *element }, *computed_values);
    else
        record = document().style_computer().intern_anonymous_layout_style(read, *computed_values);
    RustFFI::render_state_adopt_derived_node_style(document_host(), slot_id(this), record.value());
    m_has_layout_derived_style = true;
}

void NodeWithStyle::set_style_record_identity(CSS::StyleRecordID style_record_identity)
{
    auto const& read = held_read();
    // A detached or layout-derived record is independent of its DOM target's record. A
    // rendering consequence replaces and re-derives it explicitly through apply_style().
    if (m_has_layout_derived_style)
        return;
    if (m_style_record_identity == style_record_identity) {
        publish_style_record_to_node_data();
        return;
    }

    bool should_repin_style_record = m_style_record_pinned_for_cxx_consumers;
    // Both answers come from the records themselves, so neither side needs a ComputedValues built
    // for it. An overlay record borrows different payloads than its base, so carrying one is already
    // reason enough to treat the style as layout-affecting.
    auto const& style_engine = document().style_computer().style_engine();
    auto const new_record_view = style_engine.style_record_view(read, style_record_identity);
    VERIFY(new_record_view.present);
    CSS::StyleEngine::StyleRecordView old_record_view {};
    if (!!m_style_record_identity)
        old_record_view = style_engine.style_record_view(read, m_style_record_identity);
    bool changes_layout_affecting_style = !old_record_view.present
        || old_record_view.animation_overlay_identity != 0
        || new_record_view.animation_overlay_identity != 0
        || CSS::ComputedValues::layout_affecting_group_payloads_differ(old_record_view.payloads, new_record_view.payloads);

    release_pinned_style_record();
    m_background_layers.clear();
    m_mask_layers.clear();
    m_border_image.clear();
    m_list_style_type.clear();
    m_list_style_image.clear();
    m_style_record_identity = style_record_identity;
    publish_style_record_to_node_data();
    if (should_repin_style_record)
        pin_style_record_for_cxx_consumers();

    if (changes_layout_affecting_style)
        reset_cached_intrinsic_sizes_of_self_and_ancestors();
}

void NodeWithStyle::pin_style_record_for_cxx_consumers()
{
    VERIFY(m_style_record_identity);
    // A row holds one such pin, so pinning again keeps the record pinned first.
    if (m_style_record_pinned_for_cxx_consumers)
        return;
    RustFFI::render_state_pin_node_style_record_for_host(document_host(), slot_id(this), m_style_record_identity.value());
    m_style_record_pinned_for_cxx_consumers = true;
}

void NodeWithStyle::release_pinned_style_record()
{
    if (!m_style_record_pinned_for_cxx_consumers)
        return;
    RustFFI::render_state_release_node_style_record_pin_for_host(document_host(), slot_id(this));
    m_style_record_pinned_for_cxx_consumers = false;
}

static Node const* scroll_snap_container_of(NodeWithStyle const& node)
{
    auto const& read = node.held_read();
    // The scroll snap properties specified on the root element apply to the viewport rather than to its own box.
    if (node.is_viewport() || (node.dom_node() && node.dom_node() == node.document().document_element()))
        return node.document().unsafe_layout_node(read);
    if (!node.is_scroll_container())
        return nullptr;
    return &node;
}

void NodeWithStyle::publish_style_record_to_node_data()
{
    auto const view = document().style_computer().style_engine().style_record_view(held_read(), m_style_record_identity);
    VERIFY(view.present);
    auto const* payloads = view.payloads;
    m_style_payloads = payloads;
    RustFFI::render_state_set_node_style(document_host(), slot_id(this), m_style_record_identity.value(), payloads);
    m_has_layout_derived_style = false;
    did_update_style_record();
}

void NodeWithStyle::did_update_style_record()
{
    if (auto const* element = as_if<DOM::Element>(dom_node()); element && (element->has_style(CSS::PseudoElement::Selection) || element->has_style(CSS::PseudoElement::SearchText)))
        Painting::push_highlight_pseudo_styles(*element);

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

void NodeWithStyle::synchronize_table_span_data()
{
    RustFFI::render_state_restamp_table_spans(document_host(), slot_id(this));
}

void NodeWithStyle::set_display(CSS::Display display)
{
    VERIFY(!RustFFI::render_state_layout_pass_is_running(document_host()));
    RustFFI::render_state_set_layout_display(document_host(), slot_id(this), bit_cast<u32>(display));
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

void Node::prepare_subtree_for_removal()
{
    RustFFI::render_state_prepare_subtree_for_removal(document_host(), slot_id(this));
}

DOM::Node const* Node::dom_node() const
{
    return const_cast<Node*>(this)->dom_node();
}

DOM::Node* Node::dom_node()
{
    // NB: The document outlives every live row of its arena.
    auto* document = m_arena->document();
    VERIFY(document);
    return dom_node_identity().resolve(*document).ptr();
}

DOM::NodeIdentity Node::dom_node_identity() const
{
    if (is_anonymous())
        return {};
    // The document has no StyleNodeID; its row is the viewport.
    if (m_kind == RustFFI::NodeKind::Viewport)
        return DOM::NodeIdentity::of_document();
    // A row kept after its node was removed has a StyleNodeID of 0 and names nothing.
    return DOM::NodeIdentity::of_style_node(style_node_id());
}

GC::Ptr<DOM::Element const> Node::pseudo_element_generator() const
{
    return const_cast<Node*>(this)->pseudo_element_generator();
}

GC::Ptr<DOM::Element> Node::pseudo_element_generator()
{
    auto* document = m_arena->document();
    VERIFY(document);
    return as_if<DOM::Element>(pseudo_element_generator_identity().resolve(*document).ptr());
}

DOM::NodeIdentity Node::pseudo_element_generator_identity() const
{
    VERIFY(is_generated_for_pseudo_element());
    // A stale row's StyleNodeID is 0 once its generator disconnects, so it names nothing.
    return DOM::NodeIdentity::of_style_node(style_node_id());
}

static_assert(Node::encode_generated_for(CSS::PseudoElement::After) == RustFFI::GENERATED_FOR_AFTER);
static_assert(Node::encode_generated_for(CSS::PseudoElement::Backdrop) == RustFFI::GENERATED_FOR_BACKDROP);
static_assert(Node::encode_generated_for(CSS::PseudoElement::Before) == RustFFI::GENERATED_FOR_BEFORE);
static_assert(Node::encode_generated_for(CSS::PseudoElement::FirstLetter) == RustFFI::GENERATED_FOR_FIRST_LETTER);
static_assert(Node::encode_generated_for(CSS::PseudoElement::Marker) == RustFFI::GENERATED_FOR_MARKER);
static_assert(Node::encode_generated_for(CSS::first_synthetic_pseudo_element) == RustFFI::GENERATED_FOR_AFTER);
static_assert(Node::encode_generated_for(CSS::last_synthetic_pseudo_element) == RustFFI::GENERATED_FOR_LAST_SYNTHETIC);
static_assert(Node::encode_generated_for(CSS::PseudoElement::Selection) == RustFFI::SELECTION_PSEUDO_KIND + 1);
static_assert(Node::encode_generated_for(CSS::PseudoElement::SearchText) == RustFFI::SEARCH_TEXT_PSEUDO_KIND + 1);

void Node::dom_node_style_node_changed(DOM::Node& dom_node, CSS::StyleNodeID old_style_node)
{
    auto* arena = dom_node.document().layout_node_arena_if_created();
    if (!arena)
        return;
    auto new_style_node = Node::style_node_of(&dom_node);
    // The node's rows, and those of its pseudo-elements, take its new identity along with their bindings. A retired
    // identity may be reused, so it then leaves every row carrying it, including rows of a removed subtree that outlive
    // the disconnection. Nor does a layout tree update mark the new identity's previous holder left stay: the marks are
    // keyed by the identity alone.
    Vector<u8, 4> generated_for;
    if (auto* element = as_if<DOM::Element>(dom_node); element && old_style_node != 0 && new_style_node != 0) {
        element->for_each_synthetic_pseudo_element([&](CSS::PseudoElement pseudo_element, DOM::SyntheticPseudoElement const&) {
            generated_for.append(encode_generated_for(pseudo_element));
        });
    }
    RustFFI::render_state_style_node_changed(arena->host(), old_style_node.value(), new_style_node.value(), generated_for.data(), generated_for.size());
    // The arena tells a node about a change to its layout node through its StyleNodeID, so a node whose StyleNodeID
    // changes is one it cannot name. Its box presence is committed again here instead. A node that had no StyleNodeID
    // had no layout node either, and a fresh StyleNodeID has none bound yet, so it has nothing to commit.
    if (old_style_node != 0)
        arena->commit_box_presence(dom_node);
}

CSS::StyleNodeID Node::style_node_id() const
{
    return RustFFI::layout_row_style_node(document_host(), m_slot);
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
    RustFFI::render_state_set_needs_layout_update(document_host(), slot_id(this),
        propagation == LayoutUpdatePropagation::ThroughAncestors);
}

}
