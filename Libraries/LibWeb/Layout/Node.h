/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/OwnPtr.h>
#include <AK/RefCounted.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <AK/Weakable.h>
#include <AK/kmalloc.h>
#include <LibGC/Cell.h>
#include <LibGC/Root.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/StyleEngineIdentifiers.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/DOM/NodeIdentity.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Layout/NodeArena.h>
#include <LibWeb/Layout/TreeBuilderRustFFI.h>
#include <LibWeb/Painting/PaintFacts.h>
#include <LibWeb/TreeTraversal.h>

namespace Web::CSS {

class InstalledStyle;

}

namespace Web::Layout {

static_assert(sizeof(Compositing::RustFFI::NodeSlotId) == sizeof(u32));
static_assert(offsetof(Compositing::RustFFI::NodeSlotId, index) == 0);

static_assert(sizeof(RustFFI::NodeKind) == sizeof(u8));
static_assert(sizeof(RustFFI::NodeFlag) == sizeof(u32));

enum class LayoutUpdatePropagation : u8 {
    ThroughAncestors,
    BoundarySelfOnly,
};

enum class BindToPreparedArenaSlot {
    Yes,
};

class WEB_API Node : public Weakable<Node> {
public:
    AK_ALLOC_WITH_KMALLOC_PARTITION(HeapPartition::Layout);

    virtual ~Node();
    static void delete_arena_owned_shell(Node&);
    StringView class_name() const;

    static Compositing::RustFFI::NodeSlotId slot_id(Node const*);
    RustFFI::NodeKind kind() const { return m_kind; }
    u32 arena_slot_index() const { return m_slot.index; }
    NodeArena& node_arena() const { return *m_arena; }
    RustFFI::DocumentHost* document_host() const;
    // The read this node was reached in, which it lends the calls the host makes about its document: a layout node is
    // reached only through an entry that takes a read, which took the document's frame in.
    BegunRead const& held_read() const { return *RustFFI::layout_row_read_of_held_node(document_host()); }

    Node* parent_ptr() { return linked_node(RustFFI::FfiNodeLink::Parent); }
    Node const* parent_ptr() const { return linked_node(RustFFI::FfiNodeLink::Parent); }
    Node* first_child_ptr() { return linked_node(RustFFI::FfiNodeLink::FirstChild); }
    Node const* first_child_ptr() const { return linked_node(RustFFI::FfiNodeLink::FirstChild); }
    Node* last_child_ptr() { return linked_node(RustFFI::FfiNodeLink::LastChild); }
    Node* next_sibling_ptr() { return linked_node(RustFFI::FfiNodeLink::NextSibling); }
    Node const* next_sibling_ptr() const { return linked_node(RustFFI::FfiNodeLink::NextSibling); }
    Node* previous_sibling_ptr() { return linked_node(RustFFI::FfiNodeLink::PreviousSibling); }
    bool has_children() const { return first_child_ptr() != nullptr; }

    Node* first_child() { return first_child_ptr(); }
    Node const* first_child() const { return first_child_ptr(); }
    Node* next_sibling() { return next_sibling_ptr(); }
    Node const* next_sibling() const { return next_sibling_ptr(); }
    Node* previous_sibling() { return previous_sibling_ptr(); }
    Node const* previous_sibling() const { return const_cast<Node*>(this)->previous_sibling_ptr(); }

    template<typename Callback>
    TraversalDecision for_each_in_inclusive_subtree(Callback callback) const
    {
        return traverse_preorder(*this, IncludeTraversalRoot::Yes, callback);
    }

    template<typename Callback>
    TraversalDecision for_each_in_inclusive_subtree(Callback callback)
    {
        return traverse_preorder(*this, IncludeTraversalRoot::Yes, callback);
    }

    template<typename U, typename Callback>
    TraversalDecision for_each_in_inclusive_subtree_of_type(Callback callback)
    {
        return for_each_in_inclusive_subtree([callback = move(callback)](Node& node) {
            if (auto* node_of_type = as_if<U>(node))
                return callback(*node_of_type);
            return TraversalDecision::Continue;
        });
    }

    template<typename U, typename Callback>
    TraversalDecision for_each_in_inclusive_subtree_of_type(Callback callback) const
    {
        return for_each_in_inclusive_subtree([callback = move(callback)](Node const& node) {
            if (auto const* node_of_type = as_if<U>(node))
                return callback(*node_of_type);
            return TraversalDecision::Continue;
        });
    }

    template<typename Callback>
    void for_each_child(Callback callback) const
    {
        return const_cast<Node&>(*this).for_each_child(move(callback));
    }

    template<typename Callback>
    void for_each_child(Callback callback)
    {
        for (auto* node = first_child_ptr(); node; node = node->next_sibling_ptr()) {
            if (callback(*node) == IterationDecision::Break)
                return;
        }
    }

    template<typename U, typename Callback>
    void for_each_child_of_type(Callback callback)
    {
        for (auto* node = first_child_ptr(); node; node = node->next_sibling_ptr()) {
            auto* node_of_type = as_if<U>(*node);
            if (!node_of_type)
                continue;
            if (callback(*node_of_type) == IterationDecision::Break)
                return;
        }
    }

    template<typename U, typename Callback>
    void for_each_child_of_type(Callback callback) const
    {
        return const_cast<Node&>(*this).template for_each_child_of_type<U>(move(callback));
    }

    Node* next_in_pre_order()
    {
        if (auto* child = first_child_ptr())
            return child;
        for (auto* node = this; node; node = node->parent_ptr()) {
            if (auto* next = node->next_sibling_ptr())
                return next;
        }
        return nullptr;
    }

    Node const* next_in_pre_order() const
    {
        return const_cast<Node*>(this)->next_in_pre_order();
    }

    Node* previous_in_pre_order()
    {
        if (auto* node = previous_sibling_ptr()) {
            while (auto* last_child = node->last_child_ptr())
                node = last_child;
            return node;
        }
        return parent_ptr();
    }

    Node const* previous_in_pre_order() const
    {
        return const_cast<Node*>(this)->previous_in_pre_order();
    }

    bool is_before(Node const& other) const
    {
        if (this == &other)
            return false;
        for (auto const* node = this; node; node = node->next_in_pre_order()) {
            if (node == &other)
                return true;
        }
        return false;
    }

    bool is_anonymous() const { return has_identity_flag<RustFFI::NodeFlag::Anonymous>(); }
    bool is_document_element() const { return has_identity_flag<RustFFI::NodeFlag::IsDocumentElement>(); }
    DOM::Node const* dom_node() const;
    DOM::Node* dom_node();
    // The identity of the DOM node this row belongs to, which names nothing for an anonymous row
    // and for a row whose node has left the tree.
    DOM::NodeIdentity dom_node_identity() const;

    GC::Ptr<DOM::Element const> pseudo_element_generator() const;
    GC::Ptr<DOM::Element> pseudo_element_generator();
    DOM::NodeIdentity pseudo_element_generator_identity() const;

    bool needs_layout_update() const { return has_flag(RustFFI::NodeFlag::NeedsLayoutUpdate); }
    bool retains_compositor_animated_content() const { return has_flag(RustFFI::NodeFlag::HasAnimatedOpacityOrTransform); }
    void set_retains_compositor_animated_content(bool value) { set_flag(RustFFI::NodeFlag::HasAnimatedOpacityOrTransform, value); }
    bool needs_compositor_effects_layer() const { return has_compositor_animation_frame(RustFFI::CompositorAnimationFrameKind::Opacity); }
    void set_needs_compositor_effects_layer(bool value) { set_needs_compositor_animation_frame(RustFFI::CompositorAnimationFrameKind::Opacity, value); }
    bool needs_compositor_background_color_frame() const { return has_compositor_animation_frame(RustFFI::CompositorAnimationFrameKind::BackgroundColor); }
    void set_needs_compositor_background_color_frame(bool value) { set_needs_compositor_animation_frame(RustFFI::CompositorAnimationFrameKind::BackgroundColor, value); }

    // Any invalidation below a node must reach every ancestor's epoch: cached runs capture
    // subtree structure, and unlike intrinsic-size invalidation there is no absolutely-positioned
    // or SVG boundary — those descendants' fragments live in ancestor run trees. The arena runs
    // the same walk for every structural change; this serves content changes that never
    // restructure the tree.
    void reset_cached_intrinsic_sizes_of_self_and_ancestors();

    // Set when a style change altered geometry-determining properties of this node itself, so
    // a partial relayout must re-resolve its own size and position instead of reusing them.
    void set_needs_own_geometry_update() { set_flag(RustFFI::NodeFlag::NeedsOwnGeometryUpdate, true); }
    void set_needs_layout_update(DOM::SetNeedsLayoutReason, LayoutUpdatePropagation = LayoutUpdatePropagation::ThroughAncestors);

    bool is_generated_for_pseudo_element() const { return generated_for() != 0; }
    Optional<CSS::PseudoElement> generated_for_pseudo_element() const
    {
        if (!is_generated_for_pseudo_element())
            return {};
        return static_cast<CSS::PseudoElement>(generated_for() - 1);
    }
    bool is_generated_for_before_pseudo_element() const { return generated_for() == encode_generated_for(CSS::PseudoElement::Before); }
    bool is_generated_for_after_pseudo_element() const { return generated_for() == encode_generated_for(CSS::PseudoElement::After); }
    bool is_generated_for_backdrop_pseudo_element() const { return generated_for() == encode_generated_for(CSS::PseudoElement::Backdrop); }
    static constexpr u8 encode_generated_for(CSS::PseudoElement pseudo_element)
    {
        static_assert(static_cast<u8>(CSS::PseudoElement::UnknownWebKit) < 0xff);
        return static_cast<u8>(pseudo_element) + 1;
    }

    // The StyleNodeID of the element or text node this row is bound to, or of the element it is
    // generated for, or 0.
    CSS::StyleNodeID style_node_id() const;
    // The StyleNodeID a row bound to this DOM node records, or 0 for a node that has none.
    static CSS::StyleNodeID style_node_of(DOM::Node const*);
    static void dom_node_style_node_changed(DOM::Node&, CSS::StyleNodeID old_style_node);

    void prepare_subtree_for_detach_from_layout_tree();
    void pin_style_record_for_detachment();

    // Returns the direct viewport child above this node (the node itself or its outermost
    // anonymous table-fixup wrapper), or null when the node is not placed as a top layer box.
    Node* topmost_layout_node_of_top_layer_placement();

    DOM::Document& document();
    DOM::Document const& document() const;

    GC::Ptr<HTML::LocalNavigable> navigable() const;

    Viewport& root();

    String debug_description() const;

    bool has_style() const { return has_flag(RustFFI::NodeFlag::HasStyle); }
    bool has_style_or_parent_with_style() const;

    bool is_atomic_inline() const;
    bool is_fragmented_inline() const;

    // These optimize hot is<T> variants for the surviving layout classes where dynamic_cast is too slow.
    virtual bool is_box() const { return false; }
    virtual bool is_text_node() const { return false; }
    virtual bool is_viewport() const { return false; }
    virtual bool is_node_with_style() const { return false; }

    bool is_inline_node() const { return kind() == RustFFI::NodeKind::InlineNode; }
    bool is_svg_box() const { return RustFFI::layout_node_kind_is_svg_box(kind()); }
    bool is_svg_pattern_box() const { return kind() == RustFFI::NodeKind::SVGPatternBox; }
    bool is_replaced_box() const { return RustFFI::layout_node_kind_is_replaced_box(kind()); }
    bool is_table_wrapper() const { return kind() == RustFFI::NodeKind::TableWrapper; }

    template<typename T>
    bool fast_is() const = delete;

    // The arena finds the containing block by walking up the layout tree; it is always a Box or null.
    [[nodiscard]] Box const* containing_block() const;
    [[nodiscard]] Box* containing_block();

    Gfx::Font const& first_available_font() const;

    NodeWithStyle* parent();
    NodeWithStyle const* parent() const;

    bool children_are_inline() const { return has_flag(RustFFI::NodeFlag::ChildrenAreInline); }

    bool is_editing_host() const { return has_flag(RustFFI::NodeFlag::IsEditingHost); }
    static u8 dom_paint_facts_of(DOM::Node const*);

    // https://drafts.csswg.org/css-ui/#propdef-user-select
    CSS::UserSelect user_select_used_value() const;

protected:
    Node(DOM::Document&, BindToPreparedArenaSlot, Compositing::RustFFI::NodeSlotId, RustFFI::NodeKind);

    bool has_flag(RustFFI::NodeFlag flag) const
    {
        return (RustFFI::layout_row_flags(document_host(), m_slot) & static_cast<u32>(flag)) != 0;
    }

    // A flag that says what node the row stands for, which installing a style never changes, so reading it waits for
    // no rows to be published again after one.
    template<RustFFI::NodeFlag flag>
    bool has_identity_flag() const
    {
        static_assert(flag == RustFFI::NodeFlag::Anonymous || flag == RustFFI::NodeFlag::IsBody || flag == RustFFI::NodeFlag::IsDocumentElement);
        return (RustFFI::layout_row_identity_flags(document_host(), m_slot) & static_cast<u32>(flag)) != 0;
    }

    bool has_compositor_animation_frame(RustFFI::CompositorAnimationFrameKind kind) const
    {
        return RustFFI::layout_row_has_compositor_animation_frame(document_host(), m_slot, kind);
    }

    void set_needs_compositor_animation_frame(RustFFI::CompositorAnimationFrameKind kind, bool value)
    {
        RustFFI::render_state_set_node_needs_compositor_animation_frame(m_arena->host(), m_slot, kind, value);
    }

    void set_flag(RustFFI::NodeFlag flag, bool value)
    {
        RustFFI::render_state_set_node_flag(document_host(), m_slot, flag, value);
    }

private:
    friend class NodeWithStyle;

    Node* linked_node(RustFFI::FfiNodeLink link) const
    {
        return static_cast<Node*>(RustFFI::layout_row_link_shell(document_host(), m_slot, link));
    }

    Node* containing_block_node_if_live() const
    {
        return static_cast<Node*>(RustFFI::layout_row_containing_block_shell_if_live(document_host(), m_slot));
    }

    u8 generated_for() const { return RustFFI::layout_row_generated_for(document_host(), m_slot); }

    NonnullRefPtr<NodeArena> m_arena;
    Compositing::RustFFI::NodeSlotId m_slot;
    RustFFI::NodeKind m_kind { RustFFI::NodeKind::Unset };
    bool m_arena_is_destroying_shell { false };
};

template<typename T, typename... Args>
T& allocate_layout_node(Args&&... args)
{
    return *new T(forward<Args>(args)...);
}

class WEB_API NodeWithStyle : public Node {
public:
    NodeWithStyle(DOM::Document&, BindToPreparedArenaSlot, Compositing::RustFFI::NodeSlotId, RustFFI::NodeKind);

    virtual ~NodeWithStyle() override;

    class ImageObserver final : public CSS::ImageStyleValue::Client {
    public:
        AK_ALLOC_WITH_KMALLOC;

        ImageObserver(NodeWithStyle&, NonnullRefPtr<CSS::ImageStyleValue const> image);
        virtual ~ImageObserver() override;

        virtual void image_style_value_did_update(CSS::ImageStyleValue&) override;

    private:
        WeakPtr<NodeWithStyle> m_owner;
        NonnullRefPtr<CSS::ImageStyleValue const> m_image;
    };

    // The image resources a node's style asks for, which the layout arena's host tables hold by the
    // node's slot, and delete with its row if the node did not drop them before.
    struct ImageObserverSlots {
        AK_ALLOC_WITH_KMALLOC;

        Vector<RefPtr<CSS::CursorStyleValue const>> cursor_style_values;
        Vector<OwnPtr<ImageObserver>> background_layers;
        Vector<OwnPtr<ImageObserver>> mask_layers;
        Vector<OwnPtr<ImageObserver>> cursors;
        OwnPtr<ImageObserver> border_image_source;
        OwnPtr<ImageObserver> list_style_image;
    };

    ImageObserver const* background_image_observer(size_t layer_index) const;
    ImageObserver const* mask_image_observer(size_t layer_index) const;
    ImageObserver const* cursor_image_observer(size_t cursor_index) const;
    ImageObserver const* border_image_source_observer() const;

    CSS::StyleRecordID style_record_identity() const { return m_style_record_identity; }

    template<typename StyleGroup>
    StyleGroup const& style_group() const
    {
        VERIFY(m_style_payloads);
        auto const* payloads = static_cast<void const* const*>(m_style_payloads);
        auto const* payload = payloads[StyleGroup::style_group_index];
        VERIFY(payload);
        return *static_cast<StyleGroup const*>(payload);
    }

    static CSS::LengthPercentageOrAuto length_percentage_or_auto(CSS::ComputedValuesFFI::ComputedLengthPercentageOrAuto const& value)
    {
        if (value.is_auto)
            return CSS::LengthPercentageOrAuto::make_auto();
        return CSS::LengthPercentage::view(value.value);
    }

    static CSS::LengthBox length_box(CSS::ComputedValuesFFI::ComputedLengthBox const& box)
    {
        return {
            length_percentage_or_auto(box.top),
            length_percentage_or_auto(box.right),
            length_percentage_or_auto(box.bottom),
            length_percentage_or_auto(box.left),
        };
    }

    CSS::Display display() const { return CSS::display_from_ffi_display(style_group<CSS::ComputedValues::BoxValues>().display); }
    CSS::Float float_() const { return static_cast<CSS::Float>(style_group<CSS::ComputedValues::BoxValues>().float_); }
    CSS::Positioning position() const { return static_cast<CSS::Positioning>(style_group<CSS::ComputedValues::BoxValues>().position); }
    CSS::BoxSizing box_sizing() const { return static_cast<CSS::BoxSizing>(style_group<CSS::ComputedValues::BoxValues>().box_sizing); }
    CSS::Overflow overflow_x() const { return static_cast<CSS::Overflow>(style_group<CSS::ComputedValues::BoxValues>().overflow_x); }
    CSS::Overflow overflow_y() const { return static_cast<CSS::Overflow>(style_group<CSS::ComputedValues::BoxValues>().overflow_y); }
    CSS::Resize resize() const { return static_cast<CSS::Resize>(style_group<CSS::ComputedValues::BoxValues>().resize); }
    CSS::Containment contain() const
    {
        auto const& values = style_group<CSS::ComputedValues::BoxValues>();
        return { values.size_containment, values.inline_size_containment, values.layout_containment, values.style_containment, values.paint_containment };
    }
    CSS::ContentVisibility content_visibility() const { return static_cast<CSS::ContentVisibility>(style_group<CSS::ComputedValues::InheritedBoxValues>().content_visibility); }
    CSS::Direction direction() const { return static_cast<CSS::Direction>(style_group<CSS::ComputedValues::InheritedBoxValues>().direction); }
    CSS::WritingMode writing_mode() const { return static_cast<CSS::WritingMode>(style_group<CSS::ComputedValues::InheritedBoxValues>().writing_mode); }
    bool inline_axis_is_reverse() const
    {
        switch (writing_mode()) {
        case CSS::WritingMode::HorizontalTb:
        case CSS::WritingMode::VerticalRl:
        case CSS::WritingMode::VerticalLr:
        case CSS::WritingMode::SidewaysRl:
            return direction() == CSS::Direction::Rtl;
        case CSS::WritingMode::SidewaysLr:
            return direction() == CSS::Direction::Ltr;
        }
        VERIFY_NOT_REACHED();
    }
    CSS::Visibility visibility() const { return static_cast<CSS::Visibility>(style_group<CSS::ComputedValues::InheritedBoxValues>().visibility); }
    CSS::ImageRendering image_rendering() const { return static_cast<CSS::ImageRendering>(style_group<CSS::ComputedValues::InheritedBoxValues>().image_rendering); }
    Color caret_color() const { return style_group<CSS::ComputedValues::InheritedUIValues>().caret_color_value(); }
    CSS::PreferredColorScheme color_scheme() const { return style_group<CSS::ComputedValues::InheritedUIValues>().color_scheme_value(); }
    ReadonlySpan<Utf16FlyString> color_schemes() const { return style_group<CSS::ComputedValues::InheritedUIValues>().color_schemes_span(); }
    bool color_scheme_only() const { return style_group<CSS::ComputedValues::InheritedUIValues>().color_scheme_only; }
    ReadonlySpan<CSS::ComputedValuesFFI::ComputedCursor> cursor() const { return style_group<CSS::ComputedValues::InheritedUIValues>().cursor_span(); }
    ReadonlySpan<RefPtr<CSS::CursorStyleValue const>> cursor_style_values() const;
    CSS::PointerEvents pointer_events() const { return style_group<CSS::ComputedValues::InheritedUIValues>().pointer_events_value(); }
    CSS::ScrollSnapType scroll_snap_type() const { return style_group<CSS::ComputedValues::MiscResetValues>().scroll_snap_type_value(); }
    CSS::UserSelect user_select() const { return static_cast<CSS::UserSelect>(style_group<CSS::ComputedValues::MiscResetValues>().user_select); }
    Optional<Utf16FlyString> view_transition_name() const { return style_group<CSS::ComputedValues::MiscResetValues>().view_transition_name_value(); }
    Color outline_color() const { return Color::from_bgra(style_group<CSS::ComputedValues::MiscResetValues>().outline_color); }
    Color column_rule_color() const { return Color::from_bgra(style_group<CSS::ComputedValues::MiscResetValues>().column_rule_color); }
    Color background_color() const { return style_group<CSS::ComputedValues::BackgroundValues>().background_color_value(); }
    Vector<CSS::BackgroundLayerData> const& background_layers() const
    {
        if (!m_background_layers.has_value())
            m_background_layers = style_group<CSS::ComputedValues::BackgroundValues>().background_layers_value();
        return *m_background_layers;
    }
    Vector<CSS::BackgroundLayerData> const& mask_layers() const
    {
        if (!m_mask_layers.has_value())
            m_mask_layers = style_group<CSS::ComputedValues::MaskValues>().mask_layers_value();
        return *m_mask_layers;
    }
    CSS::ListStyleType const& list_style_type() const
    {
        if (!m_list_style_type.has_value()) {
            m_list_style_type = style_group<CSS::ComputedValues::InheritedListValues>().list_style_type_value(style_scope());
        }
        return *m_list_style_type;
    }
    CSS::AbstractImageStyleValue const* list_style_image() const
    {
        if (!m_list_style_image.has_value())
            m_list_style_image = style_group<CSS::ComputedValues::InheritedListValues>().list_style_image_value();
        return m_list_style_image->ptr();
    }
    CSS::ComputedFilterView backdrop_filter() const { return style_group<CSS::ComputedValues::EffectsValues>().backdrop_filter_value(); }
    CSS::ComputedFilterView filter() const { return style_group<CSS::ComputedValues::EffectsValues>().filter_value(); }
    CSS::MixBlendMode mix_blend_mode() const { return style_group<CSS::ComputedValues::EffectsValues>().mix_blend_mode_value(); }
    float opacity() const { return style_group<CSS::ComputedValues::EffectsValues>().opacity; }
    ReadonlySpan<CSS::ShadowData> box_shadow() const { return style_group<CSS::ComputedValues::EffectsValues>().box_shadow_span(); }
    CSS::BorderData const& border_left() const { return style_group<CSS::ComputedValues::BorderValues>().border_left_value(); }
    CSS::BorderData const& border_top() const { return style_group<CSS::ComputedValues::BorderValues>().border_top_value(); }
    CSS::BorderData const& border_right() const { return style_group<CSS::ComputedValues::BorderValues>().border_right_value(); }
    CSS::BorderData const& border_bottom() const { return style_group<CSS::ComputedValues::BorderValues>().border_bottom_value(); }
    CSS::BorderImageData const& border_image() const
    {
        if (!m_border_image.has_value())
            m_border_image = style_group<CSS::ComputedValues::BorderValues>().border_image_value();
        return *m_border_image;
    }
    Color color() const { return style_group<CSS::ComputedValues::InheritedTextValues>().color_value(); }
    Color webkit_text_fill_color() const { return style_group<CSS::ComputedValues::InheritedTextValues>().webkit_text_fill_color_value(); }
    CSSPixels letter_spacing() const { return style_group<CSS::ComputedValues::InheritedTextValues>().letter_spacing_value(); }
    ReadonlySpan<CSS::ShadowData> text_shadow() const { return style_group<CSS::ComputedValues::InheritedTextValues>().text_shadow_span(); }
    Color text_decoration_color() const { return Color::from_bgra(style_group<CSS::ComputedValues::TextResetValues>().text_decoration_color); }
    CSSPixels line_height() const { return style_group<CSS::ComputedValues::FontValues>().line_height_used; }
    CSSPixels font_size() const { return style_group<CSS::ComputedValues::FontValues>().font_size; }
    Gfx::FontCascadeList const& font_list() const { return style_group<CSS::ComputedValues::FontValues>().font_list_value(); }
    CSS::Size const& width() const { return CSS::Size::view(style_group<CSS::ComputedValues::SizingValues>().width); }
    CSS::Size const& min_width() const { return CSS::Size::view(style_group<CSS::ComputedValues::SizingValues>().min_width); }
    CSS::Size const& max_width() const { return CSS::Size::view(style_group<CSS::ComputedValues::SizingValues>().max_width); }
    CSS::Size const& height() const { return CSS::Size::view(style_group<CSS::ComputedValues::SizingValues>().height); }
    CSS::Size const& min_height() const { return CSS::Size::view(style_group<CSS::ComputedValues::SizingValues>().min_height); }
    CSS::Size const& max_height() const { return CSS::Size::view(style_group<CSS::ComputedValues::SizingValues>().max_height); }
    CSS::LengthBox inset() const { return length_box(style_group<CSS::ComputedValues::SurroundValues>().inset); }
    CSS::LengthBox margin() const { return length_box(style_group<CSS::ComputedValues::SurroundValues>().margin); }
    CSS::LengthBox padding() const { return length_box(style_group<CSS::ComputedValues::SurroundValues>().padding); }
    bool has_transformations() const { return style_group<CSS::ComputedValues::TransformValues>().has_transformations(); }
    template<typename Callback>
    void for_each_transformation(Callback callback) const
    {
        style_group<CSS::ComputedValues::TransformValues>().for_each_transformation(callback);
    }
    template<typename Callback>
    void for_each_resolved_transform(Callback callback) const
    {
        style_group<CSS::ComputedValues::TransformValues>().for_each_resolved_transform(callback);
    }
    CSS::TransformOrigin transform_origin() const { return style_group<CSS::ComputedValues::TransformValues>().transform_origin_value(); }
    RefPtr<CSS::TransformationStyleValue const> rotate() const { return style_group<CSS::ComputedValues::TransformValues>().rotate_value(); }
    RefPtr<CSS::TransformationStyleValue const> translate() const { return style_group<CSS::ComputedValues::TransformValues>().translate_value(); }
    RefPtr<CSS::TransformationStyleValue const> scale() const { return style_group<CSS::ComputedValues::TransformValues>().scale_value(); }
    bool has_rotate() const { return rotate() != nullptr; }
    bool has_translate() const { return translate() != nullptr; }
    bool has_scale() const { return scale() != nullptr; }
    Optional<CSSPixels> perspective() const { return style_group<CSS::ComputedValues::TransformValues>().perspective_value(); }
    Optional<CSS::SVGPaint> fill() const { return style_group<CSS::ComputedValues::InheritedSVGValues>().fill_value(); }
    Optional<CSS::SVGPaint> stroke() const { return style_group<CSS::ComputedValues::InheritedSVGValues>().stroke_value(); }
    Gfx::AffineTransform used_svg_element_transform() const;

    bool is_positioned() const;
    bool is_absolutely_positioned() const;
    bool is_fixed_position() const;
    bool is_sticky_position() const;

    bool establishes_an_absolute_positioning_containing_block() const;
    bool establishes_a_fixed_positioning_containing_block() const;

    [[nodiscard]] bool has_css_transform() const;

    void clear_image_observers();
    void apply_style(CSS::StyleRecordID);
    void attach_style_resources();
    // Like attach_style_resources(), where the style engine answered whether `style_record` holds image values.
    void attach_style_resources(CSS::StyleRecordID style_record, Painting::StyleHoldsImageValues);
    // Gives the row the spans its element published again; where they moved, the row lays out again.
    void synchronize_table_span_data();

    Gfx::Font const& first_available_font() const;
    CSS::StyleScope const& style_scope() const;

    bool is_scroll_container() const;

    void set_computed_values(Layout::BegunRead const& read, NonnullRefPtr<CSS::ComputedValues const>);
    // Takes the record its DOM target installed, `installed`, where it followed the target's record. `held_before` is
    // the record the target held before and still holds, or none.
    void set_style_record_identity(CSS::InstalledStyle const& installed, CSS::InstalledStyle const& held_before);
    void refresh_style_from_arena(CSS::StyleRecordID, void const* payloads, bool derived, bool should_attach_resources);
    // The pin lives on the node's arena row and is released with it, so
    // Document::tear_down_layout_tree() must free the layout root before the document's style
    // engine goes away. Every document destruction path goes through that teardown.
    void pin_style_record_for_cxx_consumers();
    void release_pinned_style_record();

private:
    virtual bool is_node_with_style() const final { return true; }

    void publish_style_record_to_node_data();
    void did_update_style_record();

    void rebuild_image_observers(Vector<RefPtr<CSS::CursorStyleValue const>>);
    ImageObserverSlots const* image_observers() const;
    void const* m_style_payloads { nullptr };
    // Whether the row holds a pin of its style record for C++'s readers, which only this layout node takes and drops.
    bool m_style_record_pinned_for_cxx_consumers { false };
    // Whether the arena derived the row's style record. Only this layout node publishes a record of its node to the row
    // or has the arena adopt one it derived, and a job that derives one tells the layout node.
    bool m_has_layout_derived_style { false };
    CSS::StyleRecordID m_style_record_identity;
    mutable Optional<Vector<CSS::BackgroundLayerData>> m_background_layers;
    mutable Optional<Vector<CSS::BackgroundLayerData>> m_mask_layers;
    mutable Optional<CSS::BorderImageData> m_border_image;
    mutable Optional<CSS::ListStyleType> m_list_style_type;
    mutable Optional<RefPtr<CSS::AbstractImageStyleValue const>> m_list_style_image;
};

template<>
inline bool Node::fast_is<NodeWithStyle>() const { return is_node_with_style(); }

inline bool Node::has_style_or_parent_with_style() const
{
    return has_style() || (parent() != nullptr && parent()->has_style_or_parent_with_style());
}

inline Gfx::Font const& Node::first_available_font() const
{
    VERIFY(has_style_or_parent_with_style());
    if (has_style())
        return static_cast<NodeWithStyle const*>(this)->first_available_font();
    return parent()->first_available_font();
}

inline NodeWithStyle const* Node::parent() const
{
    return static_cast<NodeWithStyle const*>(parent_ptr());
}

inline NodeWithStyle* Node::parent()
{
    return static_cast<NodeWithStyle*>(parent_ptr());
}

inline Gfx::Font const& NodeWithStyle::first_available_font() const
{
    return font_list().first_available_font();
}

bool overflow_value_makes_box_a_scroll_container(CSS::Overflow overflow);

}
