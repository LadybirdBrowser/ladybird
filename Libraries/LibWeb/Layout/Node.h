/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <AK/Weakable.h>
#include <AK/kmalloc.h>
#include <LibGC/Cell.h>
#include <LibGC/Root.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Layout/TreeBuilderRustFFI.h>
#include <LibWeb/Painting/DisplayListRecordingContext.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/RefCountedTreeNode.h>

namespace Web::Layout {

static_assert(sizeof(RustFFI::NodeSlotId) == sizeof(u32));
static_assert(offsetof(RustFFI::NodeSlotId, index) == 0);

static_assert(sizeof(RustFFI::NodeData) == 64);
static_assert(offsetof(RustFFI::NodeData, parent) == 0);
static_assert(offsetof(RustFFI::NodeData, first_child) == 4);
static_assert(offsetof(RustFFI::NodeData, last_child) == 8);
static_assert(offsetof(RustFFI::NodeData, previous_sibling) == 12);
static_assert(offsetof(RustFFI::NodeData, next_sibling) == 16);
static_assert(offsetof(RustFFI::NodeData, containing_block) == 20);
static_assert(offsetof(RustFFI::NodeData, inline_containing_block) == 24);
static_assert(offsetof(RustFFI::NodeData, kind) == 28);
static_assert(offsetof(RustFFI::NodeData, generated_for) == 29);
static_assert(offsetof(RustFFI::NodeData, intrinsic_cache_epoch) == 30);
static_assert(offsetof(RustFFI::NodeData, flags) == 32);
static_assert(offsetof(RustFFI::NodeData, fragment_cache_epoch) == 36);
static_assert(offsetof(RustFFI::NodeData, slot_generation) == 40);
static_assert(offsetof(RustFFI::NodeData, table_column_span) == 42);
static_assert(offsetof(RustFFI::NodeData, table_row_span) == 44);
static_assert(offsetof(RustFFI::NodeData, style) == 48);
static_assert(offsetof(RustFFI::NodeData, shell) == 56);

static_assert(sizeof(RustFFI::NodeKind) == sizeof(u8));
static_assert(sizeof(RustFFI::NodeFlag) == sizeof(u32));

enum class LayoutUpdatePropagation : u8 {
    ThroughAncestors,
    BoundarySelfOnly,
};

class NodeArenaAllocation {
protected:
    explicit NodeArenaAllocation(DOM::Document&);
    ~NodeArenaAllocation();

    NonnullRefPtr<NodeArena> m_arena;
    RustFFI::NodeSlotId m_slot {};
    RustFFI::NodeData* m_data { nullptr };
    u32 m_slot_generation { 0 };
};

class WEB_API Node
    : public RefCounted<Node>
    , public Weakable<Node>
    , private NodeArenaAllocation
    , public RefCountedTreeNode<Node> {

public:
    AK_ALLOC_WITH_KMALLOC_PARTITION(HeapPartition::Layout);

    using Base = RefCountedTreeNode<Node>;

    virtual ~Node();
    StringView class_name() const;

    static RustFFI::NodeSlotId slot_id(Node const*);
    RustFFI::NodeKind kind() const { return m_data->kind; }
    u32 arena_slot_index() const { return m_slot.index; }
    void* arena_handle() const;
    NodeArena& node_arena() const { return *m_arena; }

    bool is_anonymous() const { return has_flag(RustFFI::NodeFlag::Anonymous); }
    bool insets_use_anchor_functions() const { return has_flag(RustFFI::NodeFlag::InsetsUseAnchorFunctions); }
    DOM::Node const* dom_node() const;
    DOM::Node* dom_node();

    GC::Ptr<DOM::Element const> pseudo_element_generator() const;
    GC::Ptr<DOM::Element> pseudo_element_generator();

    bool needs_layout_update() const { return has_flag(RustFFI::NodeFlag::NeedsLayoutUpdate); }

    // The formatting-context run cache (LADYBIRD_FC_RUN_CACHE) validates its entries against
    // these epochs; with the cache disabled nothing reads them, so the walks no-op.
    static bool fragment_cache_epochs_enabled();

    void bump_fragment_cache_epoch();

    // Any invalidation or restructuring below a node must reach every ancestor's epoch: cached
    // runs capture subtree structure, and unlike intrinsic-size invalidation there is no
    // absolutely-positioned or SVG boundary — those descendants' fragments live in ancestor
    // run trees. Layout tree restructuring in particular never funnels through
    // set_needs_layout_update (a full pass lays out everything), so the tree mutation
    // primitives call this on the parent of every structural change.
    void bump_fragment_cache_epoch_of_self_and_ancestors();

    // Set when a style change altered geometry-determining properties of this node itself, so
    // a partial relayout must re-resolve its own size and position instead of reusing them.
    bool needs_own_geometry_update() const { return has_flag(RustFFI::NodeFlag::NeedsOwnGeometryUpdate); }
    void set_needs_own_geometry_update() { set_flag(RustFFI::NodeFlag::NeedsOwnGeometryUpdate, true); }
    void set_needs_layout_update(DOM::SetNeedsLayoutReason, LayoutUpdatePropagation = LayoutUpdatePropagation::ThroughAncestors);
    void reset_needs_layout_update()
    {
        set_flag(RustFFI::NodeFlag::NeedsLayoutUpdate, false);
        set_flag(RustFFI::NodeFlag::NeedsOwnGeometryUpdate, false);
    }

    bool is_generated_for_pseudo_element() const { return m_data->generated_for != 0; }
    Optional<CSS::PseudoElement> generated_for_pseudo_element() const
    {
        if (!is_generated_for_pseudo_element())
            return {};
        return static_cast<CSS::PseudoElement>(m_data->generated_for - 1);
    }
    // The principal box of a pseudo-element has no DOM node, but unlike an anonymous wrapper it has its own
    // computed style.
    bool is_pseudo_element_principal_box() const;
    bool is_generated_for_before_pseudo_element() const { return m_data->generated_for == encode_generated_for(CSS::PseudoElement::Before); }
    bool is_generated_for_after_pseudo_element() const { return m_data->generated_for == encode_generated_for(CSS::PseudoElement::After); }
    bool is_generated_for_backdrop_pseudo_element() const { return m_data->generated_for == encode_generated_for(CSS::PseudoElement::Backdrop); }
    void set_generated_for(CSS::PseudoElement type, DOM::Element&);

    RefPtr<Painting::Paintable> paintable() { return m_paintable; }
    RefPtr<Painting::Paintable const> paintable() const { return m_paintable; }
    Painting::Paintable* paintable_ptr() { return m_paintable.ptr(); }
    Painting::Paintable const* paintable_ptr() const { return m_paintable.ptr(); }
    void set_paintable(RefPtr<Painting::Paintable>);
    void clear_paintable();
    void prepare_for_detach_from_layout_tree();
    void prepare_subtree_for_detach_from_layout_tree();
    void pin_style_record_for_detachment();

    // Returns the direct viewport child above this node (the node itself or its outermost
    // anonymous table-fixup wrapper), or null when the node is not placed as a top layer box.
    Node* topmost_layout_node_of_top_layer_placement();

    RefPtr<Painting::Paintable> create_paintable() const;

    DOM::Document& document();
    DOM::Document const& document() const;

    GC::Ptr<HTML::LocalNavigable> navigable() const;

    Viewport const& root() const;
    Viewport& root();

    bool is_root_element() const;

    String debug_description() const;

    bool has_style() const { return has_flag(RustFFI::NodeFlag::HasStyle); }
    bool has_style_or_parent_with_style() const;

    bool can_have_children() const;

    bool is_inline() const;

    bool is_replaced_element() const;
    bool is_atomic_inline() const;
    bool is_fragmented_inline() const;
    NodeWithStyle const* nearest_fragmented_inline_ancestor() const;

    // An element is called out of flow if it is floated, absolutely positioned, or is the root element.
    // https://www.w3.org/TR/CSS22/visuren.html#positioning-scheme
    bool is_out_of_flow() const;

    // An element is called in-flow if it is not out-of-flow.
    // https://www.w3.org/TR/CSS22/visuren.html#positioning-scheme
    bool is_in_flow() const { return !is_out_of_flow(); }

    // These optimize hot is<T> variants for the surviving layout classes where dynamic_cast is too slow.
    virtual bool is_box() const { return false; }
    virtual bool is_block_container() const { return false; }
    virtual bool is_text_node() const { return false; }
    virtual bool is_text_slice_node() const { return false; }
    virtual bool is_viewport() const { return false; }
    virtual bool is_node_with_style() const { return false; }

    bool is_inline_node() const { return kind() == RustFFI::NodeKind::InlineNode; }
    bool is_break_node() const { return kind() == RustFFI::NodeKind::BreakNode; }
    bool is_svg_box() const { return RustFFI::layout_node_kind_is_svg_box(kind()); }
    bool is_svg_geometry_box() const { return kind() == RustFFI::NodeKind::SVGGeometryBox; }
    bool is_svg_clip_box() const { return kind() == RustFFI::NodeKind::SVGClipBox; }
    bool is_svg_mask_box() const { return kind() == RustFFI::NodeKind::SVGMaskBox; }
    bool is_svg_pattern_box() const { return kind() == RustFFI::NodeKind::SVGPatternBox; }
    bool is_svg_svg_box() const { return kind() == RustFFI::NodeKind::SVGSVGBox; }
    bool is_svg_graphics_box() const { return RustFFI::layout_node_kind_is_svg_graphics_box(kind()); }
    bool is_svg_foreign_object_box() const { return kind() == RustFFI::NodeKind::SVGForeignObjectBox; }
    bool is_replaced_box() const { return RustFFI::layout_node_kind_is_replaced_box(kind()); }
    bool is_list_item_box() const { return kind() == RustFFI::NodeKind::ListItemBox; }
    bool is_list_item_marker_box() const { return kind() == RustFFI::NodeKind::ListItemMarkerBox; }
    bool is_fieldset_box() const { return kind() == RustFFI::NodeKind::FieldSetBox; }
    bool is_legend_box() const { return kind() == RustFFI::NodeKind::LegendBox; }
    bool is_table_wrapper() const { return kind() == RustFFI::NodeKind::TableWrapper; }

    bool is_replaced_box_with_children() const { return is_replaced_box() && can_have_children(); }

    template<typename T>
    bool fast_is() const = delete;

    bool is_flex_item() const { return has_flag(RustFFI::NodeFlag::IsFlexItem); }

    bool is_grid_item() const { return has_flag(RustFFI::NodeFlag::IsGridItem); }

    bool vertical_align_applies() const
    {
        // https://drafts.csswg.org/css-flexbox/#flex-containers
        // "vertical-align has no effect on a flex item"
        if (is_flex_item())
            return false;
        // https://drafts.csswg.org/css-grid-1/#grid-container
        // "vertical-align has no effect on a grid item"
        if (is_grid_item())
            return false;
        // FIXME: Per-spec, vertical-align only applies to inline-level boxes and table cells; this should be narrowed
        //        to that — rather than only excluding flex and grid items.
        return true;
    }

    [[nodiscard]] Box const* containing_block() const { return m_containing_block; }
    [[nodiscard]] Box* containing_block() { return m_containing_block; }

    // Returns the inline node that actually establishes the containing block for this absolutely
    // positioned element, if applicable. This is needed because m_containing_block can only hold
    // a Box*, but CSS allows inline elements (like a <span> with position:relative) to establish
    // containing blocks for their absolutely positioned descendants.
    [[nodiscard]] NodeWithStyle const* inline_containing_block_if_applicable() const { return m_inline_containing_block_if_applicable; }

    void recompute_containing_block(Badge<DOM::Document>);

    // Closest non-anonymous ancestor box, to be used when resolving percentage values.
    // Anonymous block boxes are ignored when resolving percentage values that would refer to it:
    // the closest non-anonymous ancestor box is used instead.
    // https://www.w3.org/TR/CSS22/visuren.html#anonymous-block-level
    Box const* non_anonymous_containing_block() const;

    Gfx::Font const& first_available_font() const;
    Gfx::Font const& font(DisplayListRecordingContext&) const;
    Gfx::Font const& font(float scale_factor) const;

    NodeWithStyle* parent();
    NodeWithStyle const* parent() const;

    bool children_are_inline() const { return has_flag(RustFFI::NodeFlag::ChildrenAreInline); }
    void set_children_are_inline(bool value) { set_flag(RustFFI::NodeFlag::ChildrenAreInline, value); }

    void set_list_marker_is_inside(bool value) { set_flag(RustFFI::NodeFlag::ListMarkerIsInside, value); }

    bool is_editing_host() const { return has_flag(RustFFI::NodeFlag::IsEditingHost); }
    void set_is_editing_host(bool value) { set_flag(RustFFI::NodeFlag::IsEditingHost, value); }

    // https://drafts.csswg.org/css-ui/#propdef-user-select
    CSS::UserSelect user_select_used_value() const;

    [[nodiscard]] bool has_been_wrapped_in_table_wrapper() const { return has_flag(RustFFI::NodeFlag::HasBeenWrappedInTableWrapper); }
    void set_has_been_wrapped_in_table_wrapper(bool value) { set_flag(RustFFI::NodeFlag::HasBeenWrappedInTableWrapper, value); }

    enum class AttachToDOMNode {
        No,
        Yes,
    };

protected:
    Node(DOM::Document&, GC::Ptr<DOM::Node>, AttachToDOMNode = AttachToDOMNode::Yes);

    bool has_flag(RustFFI::NodeFlag flag) const
    {
        return (m_data->flags & static_cast<u32>(flag)) != 0;
    }

    void set_flag(RustFFI::NodeFlag flag, bool value)
    {
        if (value)
            m_data->flags |= static_cast<u32>(flag);
        else
            m_data->flags &= ~static_cast<u32>(flag);
    }

    RustFFI::NodeData& node_data() { return *m_data; }
    RustFFI::NodeData const& node_data() const { return *m_data; }

private:
    friend class NodeWithStyle;
    friend class RefCountedTreeNode<Node>;

    static constexpr u8 encode_generated_for(CSS::PseudoElement pseudo_element)
    {
        static_assert(static_cast<u8>(CSS::PseudoElement::UnknownWebKit) < 0xff);
        return static_cast<u8>(pseudo_element) + 1;
    }

    void set_containing_block(Box*);
    void set_inline_containing_block(NodeWithStyle const*);
    void synchronize_topology();

protected:
    void set_node_kind(RustFFI::NodeKind);
    void enroll_for_arena_replaced_content_facts_sync_if_eligible();

private:
    // A DOM mutation can disconnect a node before the next layout-tree update. Keep the DOM node alive until this
    // layout node is destroyed so detach hooks never observe a collected image provider or other element state.
    GC::Root<DOM::Node> m_dom_node;
    RefPtr<Painting::Paintable> m_paintable;

    Box* m_containing_block { nullptr };

    // For absolutely positioned elements, if there's an inline element (like a <span> with
    // position:relative) that should be the containing block but can't be stored in m_containing_block
    // (because it's not a Box), we store it here. This happens when a block element is inside an
    // inline element - the layout tree restructures so the block becomes a sibling of the inline,
    // but the CSS containing block relationship is based on the DOM structure.
    NodeWithStyle const* m_inline_containing_block_if_applicable { nullptr };

    GC::Weak<DOM::Element> m_pseudo_element_generator;

    bool m_enrolled_for_arena_replaced_content_facts_sync { false };
};

class WEB_API NodeWithStyle : public Node {
public:
    NodeWithStyle(DOM::Document&, GC::Ptr<DOM::Node>, CSS::LayoutStyle, RustFFI::NodeKind = RustFFI::NodeKind::NodeWithStyle);

    virtual ~NodeWithStyle() override;

    class ImageObserver final : public CSS::ImageStyleValue::Client {
    public:
        ImageObserver(NodeWithStyle&, NonnullRefPtr<CSS::ImageStyleValue const> image);
        virtual ~ImageObserver() override;

        virtual void image_style_value_did_update(CSS::ImageStyleValue&) override;

    private:
        WeakPtr<NodeWithStyle> m_owner;
        NonnullRefPtr<CSS::ImageStyleValue const> m_image;
    };

    NonnullRefPtr<CSS::ComputedValues const> copy_computed_values() const;
    CSS::ComputedStyleRecordView computed_style_record_view() const;
    CSS::StyleRecordID style_record_identity() const { return m_style_record_identity; }

    template<typename StyleGroup>
    StyleGroup const& style_group() const
    {
        VERIFY(node_data().style);
        auto const* payloads = static_cast<void const* const*>(node_data().style);
        auto const* payload = payloads[StyleGroup::style_group_index];
        VERIFY(payload);
        return *static_cast<StyleGroup const*>(payload);
    }

    template<typename Callback>
    void modify_computed_values(Callback callback)
    {
        auto record_view = computed_style_record_view();
        CSS::ComputedValues::Builder builder(m_owned_computed_values ? *m_owned_computed_values : *record_view);
        callback(*builder.operator->());
        set_computed_values(move(builder).build());
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
    CSS::Display display_before_box_type_transformation() const { return CSS::display_from_ffi_display(style_group<CSS::ComputedValues::BoxValues>().display_before_box_type_transformation); }
    CSS::Float float_() const { return static_cast<CSS::Float>(style_group<CSS::ComputedValues::BoxValues>().float_); }
    CSS::Clear clear() const { return static_cast<CSS::Clear>(style_group<CSS::ComputedValues::BoxValues>().clear); }
    CSS::Positioning position() const { return static_cast<CSS::Positioning>(style_group<CSS::ComputedValues::BoxValues>().position); }
    CSS::BoxSizing box_sizing() const { return static_cast<CSS::BoxSizing>(style_group<CSS::ComputedValues::BoxValues>().box_sizing); }
    CSS::Overflow overflow_x() const { return static_cast<CSS::Overflow>(style_group<CSS::ComputedValues::BoxValues>().overflow_x); }
    CSS::Overflow overflow_y() const { return static_cast<CSS::Overflow>(style_group<CSS::ComputedValues::BoxValues>().overflow_y); }
    CSS::Resize resize() const { return static_cast<CSS::Resize>(style_group<CSS::ComputedValues::BoxValues>().resize); }
    Variant<CSS::VerticalAlign, CSS::LengthPercentage> vertical_align() const
    {
        auto const& value = style_group<CSS::ComputedValues::BoxValues>().vertical_align;
        if (value.is_keyword)
            return static_cast<CSS::VerticalAlign>(value.keyword);
        return CSS::LengthPercentage::view(value.value);
    }
    Optional<int> z_index() const
    {
        auto const& values = style_group<CSS::ComputedValues::BoxValues>();
        if (!values.has_z_index)
            return {};
        return values.z_index;
    }
    CSS::AspectRatio aspect_ratio() const
    {
        auto const& value = style_group<CSS::ComputedValues::BoxValues>().aspect_ratio;
        return {
            value.use_natural_aspect_ratio_if_available,
            value.has_preferred_ratio ? Optional<CSS::Ratio> { CSS::Ratio { value.preferred_ratio_numerator, value.preferred_ratio_denominator } } : OptionalNone {},
            value.computed_use_natural_aspect_ratio_if_available,
            value.has_computed_ratio ? Optional<CSS::Ratio> { CSS::Ratio { value.computed_ratio_numerator, value.computed_ratio_denominator } } : OptionalNone {},
        };
    }
    CSS::Containment contain() const
    {
        auto const& values = style_group<CSS::ComputedValues::BoxValues>();
        return { values.size_containment, values.inline_size_containment, values.layout_containment, values.style_containment, values.paint_containment };
    }
    CSS::ContainerType container_type() const
    {
        auto const& values = style_group<CSS::ComputedValues::BoxValues>();
        return { values.is_size_container, values.is_inline_size_container, values.is_scroll_state_container };
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
    bool block_axis_is_reverse() const
    {
        switch (writing_mode()) {
        case CSS::WritingMode::HorizontalTb:
        case CSS::WritingMode::VerticalLr:
        case CSS::WritingMode::SidewaysLr:
            return false;
        case CSS::WritingMode::VerticalRl:
        case CSS::WritingMode::SidewaysRl:
            return true;
        }
        VERIFY_NOT_REACHED();
    }
    CSS::Visibility visibility() const { return static_cast<CSS::Visibility>(style_group<CSS::ComputedValues::InheritedBoxValues>().visibility); }
    CSS::ImageRendering image_rendering() const { return static_cast<CSS::ImageRendering>(style_group<CSS::ComputedValues::InheritedBoxValues>().image_rendering); }
    Color caret_color() const { return style_group<CSS::ComputedValues::InheritedUIValues>().caret_color_value(); }
    Optional<Color> accent_color() const { return style_group<CSS::ComputedValues::InheritedUIValues>().accent_color_value(); }
    CSS::PreferredColorScheme color_scheme() const { return style_group<CSS::ComputedValues::InheritedUIValues>().color_scheme_value(); }
    ReadonlySpan<Utf16FlyString> color_schemes() const { return style_group<CSS::ComputedValues::InheritedUIValues>().color_schemes_span(); }
    ReadonlySpan<CSS::ComputedValuesFFI::ComputedCursor> cursor() const { return style_group<CSS::ComputedValues::InheritedUIValues>().cursor_span(); }
    ReadonlySpan<RefPtr<CSS::CursorStyleValue const>> cursor_style_values() const { return m_cursor_style_values; }
    CSS::PointerEvents pointer_events() const { return style_group<CSS::ComputedValues::InheritedUIValues>().pointer_events_value(); }
    CSS::ScrollbarColorData scrollbar_color() const { return style_group<CSS::ComputedValues::InheritedUIValues>().scrollbar_color_value(); }
    CSS::Appearance appearance() const { return static_cast<CSS::Appearance>(style_group<CSS::ComputedValues::MiscResetValues>().appearance); }
    CSS::ObjectFit object_fit() const { return static_cast<CSS::ObjectFit>(style_group<CSS::ComputedValues::MiscResetValues>().object_fit); }
    CSS::Position object_position() const { return style_group<CSS::ComputedValues::MiscResetValues>().object_position_value(); }
    CSS::OverflowClipMarginData overflow_clip_margin() const { return style_group<CSS::ComputedValues::MiscResetValues>().overflow_clip_margin_value(); }
    CSS::LengthBox scroll_padding() const { return length_box(style_group<CSS::ComputedValues::MiscResetValues>().scroll_padding); }
    CSS::ScrollbarWidth scrollbar_width() const { return static_cast<CSS::ScrollbarWidth>(style_group<CSS::ComputedValues::MiscResetValues>().scrollbar_width); }
    CSS::UserSelect user_select() const { return static_cast<CSS::UserSelect>(style_group<CSS::ComputedValues::MiscResetValues>().user_select); }
    CSS::WillChange will_change() const { return style_group<CSS::ComputedValues::MiscResetValues>().will_change_value(); }
    Optional<Utf16FlyString> view_transition_name() const { return style_group<CSS::ComputedValues::MiscResetValues>().view_transition_name_value(); }
    Color outline_color() const { return Color::from_bgra(style_group<CSS::ComputedValues::MiscResetValues>().outline_color); }
    CSSPixels outline_offset() const { return CSSPixels::from_raw(style_group<CSS::ComputedValues::MiscResetValues>().outline_offset); }
    CSS::OutlineStyle outline_style() const { return static_cast<CSS::OutlineStyle>(style_group<CSS::ComputedValues::MiscResetValues>().outline_style); }
    CSSPixels outline_width() const { return CSSPixels::from_raw(style_group<CSS::ComputedValues::MiscResetValues>().outline_width); }
    Color background_color() const { return style_group<CSS::ComputedValues::BackgroundValues>().background_color_value(); }
    CSS::BackgroundBox background_color_clip() const { return style_group<CSS::ComputedValues::BackgroundValues>().background_color_clip_value(); }
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
    RefPtr<CSS::AbstractImageStyleValue const> mask_image() const { return mask_layers().first().background_image; }
    CSS::ListStyleType const& list_style_type() const
    {
        if (!m_list_style_type.has_value()) {
            m_list_style_type = style_group<CSS::ComputedValues::InheritedListValues>().list_style_type_value(style_scope());
        }
        return *m_list_style_type;
    }
    CSS::ListStylePosition list_style_position() const { return static_cast<CSS::ListStylePosition>(style_group<CSS::ComputedValues::InheritedListValues>().list_style_position); }
    CSS::AbstractImageStyleValue const* list_style_image() const
    {
        if (!m_list_style_image.has_value())
            m_list_style_image = style_group<CSS::ComputedValues::InheritedListValues>().list_style_image_value();
        return m_list_style_image->ptr();
    }
    CSS::ComputedFilterView backdrop_filter() const { return style_group<CSS::ComputedValues::EffectsValues>().backdrop_filter_value(); }
    CSS::Clip clip() const { return style_group<CSS::ComputedValues::EffectsValues>().clip_value(); }
    CSS::ComputedFilterView filter() const { return style_group<CSS::ComputedValues::EffectsValues>().filter_value(); }
    CSS::Isolation isolation() const { return style_group<CSS::ComputedValues::EffectsValues>().isolation_value(); }
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
    bool has_noninitial_border_radii() const { return style_group<CSS::ComputedValues::BorderValues>().has_noninitial_border_radii_value(); }
    CSS::BorderRadiusData border_bottom_left_radius() const { return style_group<CSS::ComputedValues::BorderValues>().border_bottom_left_radius_value(); }
    CSS::BorderRadiusData border_bottom_right_radius() const { return style_group<CSS::ComputedValues::BorderValues>().border_bottom_right_radius_value(); }
    CSS::BorderRadiusData border_top_left_radius() const { return style_group<CSS::ComputedValues::BorderValues>().border_top_left_radius_value(); }
    CSS::BorderRadiusData border_top_right_radius() const { return style_group<CSS::ComputedValues::BorderValues>().border_top_right_radius_value(); }
    CSS::BorderCollapse border_collapse() const { return static_cast<CSS::BorderCollapse>(style_group<CSS::ComputedValues::InheritedTableValues>().border_collapse); }
    CSS::EmptyCells empty_cells() const { return static_cast<CSS::EmptyCells>(style_group<CSS::ComputedValues::InheritedTableValues>().empty_cells); }
    Color color() const { return style_group<CSS::ComputedValues::InheritedTextValues>().color_value(); }
    Color webkit_text_fill_color() const { return style_group<CSS::ComputedValues::InheritedTextValues>().webkit_text_fill_color_value(); }
    CSSPixels letter_spacing() const { return style_group<CSS::ComputedValues::InheritedTextValues>().letter_spacing_value(); }
    ReadonlySpan<CSS::ShadowData> text_shadow() const { return style_group<CSS::ComputedValues::InheritedTextValues>().text_shadow_span(); }
    CSS::TextTransform text_transform() const { return style_group<CSS::ComputedValues::InheritedTextValues>().text_transform_value(); }
    CSS::WhiteSpaceCollapse white_space_collapse() const { return style_group<CSS::ComputedValues::InheritedTextValues>().white_space_collapse_value(); }
    CSS::TextDecorationSkipInk text_decoration_skip_ink() const { return style_group<CSS::ComputedValues::InheritedTextValues>().text_decoration_skip_ink_value(); }
    CSSPixels text_underline_offset() const { return style_group<CSS::ComputedValues::InheritedTextValues>().text_underline_offset_value(); }
    CSS::TextUnderlinePosition text_underline_position() const { return style_group<CSS::ComputedValues::InheritedTextValues>().text_underline_position_value(); }
    ReadonlySpan<CSS::TextDecorationLine> text_decoration_line() const { return style_group<CSS::ComputedValues::TextResetValues>().decoration_lines(); }
    CSS::TextDecorationThickness text_decoration_thickness() const { return style_group<CSS::ComputedValues::TextResetValues>().decoration_thickness(); }
    Color text_decoration_color() const { return Color::from_bgra(style_group<CSS::ComputedValues::TextResetValues>().text_decoration_color); }
    CSS::TextDecorationStyle text_decoration_style() const { return static_cast<CSS::TextDecorationStyle>(style_group<CSS::ComputedValues::TextResetValues>().text_decoration_style); }
    Optional<CSS::ContentData> const& content() const { return m_content; }
    CSSPixels line_height() const { return CSSPixels::from_raw(style_group<CSS::ComputedValues::FontValues>().line_height_used); }
    CSSPixels font_size() const { return CSSPixels::from_raw(style_group<CSS::ComputedValues::FontValues>().font_size); }
    Gfx::FontCascadeList const& font_list() const { return style_group<CSS::ComputedValues::FontValues>().font_list_value(); }
    CSS::FlexBasis flex_basis() const
    {
        auto const& value = style_group<CSS::ComputedValues::AlignmentValues>().flex_basis;
        if (value.is_content)
            return CSS::FlexBasisContent {};
        return CSS::Size::view(value.size);
    }
    CSS::FlexDirection flex_direction() const { return static_cast<CSS::FlexDirection>(style_group<CSS::ComputedValues::AlignmentValues>().flex_direction); }
    CSS::FlexWrap flex_wrap() const { return static_cast<CSS::FlexWrap>(style_group<CSS::ComputedValues::AlignmentValues>().flex_wrap); }
    CSS::AlignSelf align_self() const { return static_cast<CSS::AlignSelf>(style_group<CSS::ComputedValues::AlignmentValues>().align_self); }
    CSS::JustifySelf justify_self() const { return static_cast<CSS::JustifySelf>(style_group<CSS::ComputedValues::AlignmentValues>().justify_self); }
    i32 order() const { return style_group<CSS::ComputedValues::AlignmentValues>().order; }
    CSS::Size const& width() const { return CSS::Size::view(style_group<CSS::ComputedValues::SizingValues>().width); }
    CSS::Size const& min_width() const { return CSS::Size::view(style_group<CSS::ComputedValues::SizingValues>().min_width); }
    CSS::Size const& max_width() const { return CSS::Size::view(style_group<CSS::ComputedValues::SizingValues>().max_width); }
    CSS::Size const& height() const { return CSS::Size::view(style_group<CSS::ComputedValues::SizingValues>().height); }
    CSS::Size const& min_height() const { return CSS::Size::view(style_group<CSS::ComputedValues::SizingValues>().min_height); }
    CSS::Size const& max_height() const { return CSS::Size::view(style_group<CSS::ComputedValues::SizingValues>().max_height); }
    CSS::LengthBox inset() const { return length_box(style_group<CSS::ComputedValues::SurroundValues>().inset); }
    CSS::LengthBox margin() const { return length_box(style_group<CSS::ComputedValues::SurroundValues>().margin); }
    CSS::LengthBox padding() const { return length_box(style_group<CSS::ComputedValues::SurroundValues>().padding); }
    CSS::PositionAnchor position_anchor_value() const { return style_group<CSS::ComputedValues::AnchorValues>().position_anchor_value(); }
    bool has_transformations() const { return style_group<CSS::ComputedValues::TransformValues>().has_transformations(); }
    template<typename Callback>
    void for_each_transformation(Callback callback) const
    {
        style_group<CSS::ComputedValues::TransformValues>().for_each_transformation(callback);
    }
    bool has_resolved_transforms() const { return style_group<CSS::ComputedValues::TransformValues>().has_resolved_transforms(); }
    template<typename Callback>
    void for_each_resolved_transform(Callback callback) const
    {
        style_group<CSS::ComputedValues::TransformValues>().for_each_resolved_transform(callback);
    }
    CSS::TransformBox transform_box() const { return style_group<CSS::ComputedValues::TransformValues>().transform_box_value(); }
    CSS::TransformOrigin transform_origin() const { return style_group<CSS::ComputedValues::TransformValues>().transform_origin_value(); }
    CSS::TransformStyle transform_style() const { return style_group<CSS::ComputedValues::TransformValues>().transform_style_value(); }
    RefPtr<CSS::TransformationStyleValue const> rotate() const { return style_group<CSS::ComputedValues::TransformValues>().rotate_value(); }
    RefPtr<CSS::TransformationStyleValue const> translate() const { return style_group<CSS::ComputedValues::TransformValues>().translate_value(); }
    RefPtr<CSS::TransformationStyleValue const> scale() const { return style_group<CSS::ComputedValues::TransformValues>().scale_value(); }
    bool has_rotate() const { return rotate() != nullptr; }
    bool has_translate() const { return translate() != nullptr; }
    bool has_scale() const { return scale() != nullptr; }
    Optional<CSSPixels> perspective() const { return style_group<CSS::ComputedValues::TransformValues>().perspective_value(); }
    CSS::Position perspective_origin() const { return style_group<CSS::ComputedValues::TransformValues>().perspective_origin_value(); }
    Optional<CSS::MaskReference> mask() const { return style_group<CSS::ComputedValues::MaskValues>().mask_value(); }
    CSS::MaskType mask_type() const { return style_group<CSS::ComputedValues::MaskValues>().mask_type_value(); }
    Optional<CSS::ClipPathReference> clip_path() const { return style_group<CSS::ComputedValues::MaskValues>().clip_path_value(); }
    Optional<CSS::BaselineMetric> dominant_baseline() const { return style_group<CSS::ComputedValues::InheritedSVGValues>().dominant_baseline_value(); }
    Optional<CSS::SVGPaint> fill() const { return style_group<CSS::ComputedValues::InheritedSVGValues>().fill_value(); }
    CSS::FillRule fill_rule() const { return style_group<CSS::ComputedValues::InheritedSVGValues>().fill_rule_value(); }
    Optional<CSS::SVGPaint> stroke() const { return style_group<CSS::ComputedValues::InheritedSVGValues>().stroke_value(); }
    float fill_opacity() const { return style_group<CSS::ComputedValues::InheritedSVGValues>().fill_opacity; }
    ReadonlySpan<CSS::ComputedValuesFFI::ComputedSvgDash> stroke_dasharray() const { return style_group<CSS::ComputedValues::InheritedSVGValues>().stroke_dasharray_span(); }
    CSS::LengthPercentage const& stroke_dashoffset() const { return style_group<CSS::ComputedValues::InheritedSVGValues>().stroke_dashoffset_value(); }
    CSS::StrokeLinecap stroke_linecap() const { return style_group<CSS::ComputedValues::InheritedSVGValues>().stroke_linecap_value(); }
    CSS::StrokeLinejoin stroke_linejoin() const { return style_group<CSS::ComputedValues::InheritedSVGValues>().stroke_linejoin_value(); }
    double stroke_miterlimit() const { return style_group<CSS::ComputedValues::InheritedSVGValues>().stroke_miterlimit; }
    float stroke_opacity() const { return style_group<CSS::ComputedValues::InheritedSVGValues>().stroke_opacity; }
    CSS::LengthPercentage const& stroke_width() const { return style_group<CSS::ComputedValues::InheritedSVGValues>().stroke_width_value(); }
    CSS::ClipRule clip_rule() const { return style_group<CSS::ComputedValues::InheritedSVGValues>().clip_rule_value(); }
    CSS::PaintOrderList paint_order() const { return style_group<CSS::ComputedValues::InheritedSVGValues>().paint_order_value(); }
    CSS::TextAnchor text_anchor() const { return style_group<CSS::ComputedValues::InheritedSVGValues>().text_anchor_value(); }
    CSS::ShapeRendering shape_rendering() const { return style_group<CSS::ComputedValues::InheritedSVGValues>().shape_rendering_value(); }
    CSS::LengthPercentage const& cx() const { return CSS::LengthPercentage::view(style_group<CSS::ComputedValues::SVGResetValues>().cx); }
    CSS::LengthPercentage const& cy() const { return CSS::LengthPercentage::view(style_group<CSS::ComputedValues::SVGResetValues>().cy); }
    CSS::LengthPercentage const& r() const { return CSS::LengthPercentage::view(style_group<CSS::ComputedValues::SVGResetValues>().r); }
    CSS::LengthPercentage const& x() const { return CSS::LengthPercentage::view(style_group<CSS::ComputedValues::SVGResetValues>().x); }
    CSS::LengthPercentage const& y() const { return CSS::LengthPercentage::view(style_group<CSS::ComputedValues::SVGResetValues>().y); }
    CSS::VectorEffect vector_effect() const { return static_cast<CSS::VectorEffect>(style_group<CSS::ComputedValues::SVGResetValues>().vector_effect); }
    bool is_inline_block() const;
    bool is_inline_table() const;
    bool has_replaced_element_table_display_adjustment() const;
    bool is_transformable() const;
    Gfx::AffineTransform used_svg_element_transform() const;
    CSS::TransformStyle used_transform_style() const;
    bool establishes_or_extends_a_3d_rendering_context() const;
    bool participates_in_a_3d_rendering_context() const;

    bool is_floating() const;
    bool is_positioned() const;
    bool is_absolutely_positioned() const;
    bool is_fixed_position() const;
    bool is_sticky_position() const;

    bool is_text_decoration_propagation_boundary() const;

    // An element is called out of flow if it is floated, absolutely positioned, or is the root element.
    // https://www.w3.org/TR/CSS22/visuren.html#positioning-scheme
    bool is_out_of_flow() const { return is_floating() || is_absolutely_positioned(); }

    // An element is called in-flow if it is not out-of-flow.
    // https://www.w3.org/TR/CSS22/visuren.html#positioning-scheme
    bool is_in_flow() const { return !is_out_of_flow(); }

    bool establishes_stacking_context() const;
    bool style_establishes_absolute_positioning_containing_block() const;
    bool establishes_an_absolute_positioning_containing_block() const;
    bool establishes_a_fixed_positioning_containing_block() const;

    struct PositioningContainingBlockEstablishment {
        bool absolute;
        bool fixed;
    };
    PositioningContainingBlockEstablishment establishes_positioning_containing_blocks() const;

    // https://drafts.csswg.org/css-contain-2/#containment-types
    bool has_size_containment() const;
    bool has_inline_size_containment() const;
    bool has_layout_containment() const;
    bool has_style_containment() const;
    bool has_paint_containment() const;

    [[nodiscard]] bool has_css_transform() const
    {
        auto has_transform = has_transformations() || has_rotate() || has_translate() || has_scale();
        return has_transform && is_transformable();
    }

    void clear_image_observers();
    void apply_style(CSS::StyleRecordID);
    void attach_style_resources();
    bool synchronize_table_span_data();

    Gfx::Font const& first_available_font() const;
    CSS::StyleScope const& style_scope() const;

    NonnullRefPtr<NodeWithStyle> create_anonymous_wrapper() const;

    void transfer_table_box_computed_values_to_wrapper_computed_values(CSS::ComputedValues::Builder& wrapper_computed_values);

    bool is_body() const { return has_flag(RustFFI::NodeFlag::IsBody); }
    bool is_scroll_container() const;

    void set_computed_values(NonnullRefPtr<CSS::ComputedValues const>);
    void set_style_record_identity(CSS::StyleRecordID);
    void pin_style_record_for_cxx_consumers();
    void release_pinned_style_record();
    void bind_generated_style_record(CSS::StyleRecordID);

    void set_display(CSS::Display);
    void set_content(CSS::ContentData const&);
    void set_overflow(CSS::Overflow overflow_x, CSS::Overflow overflow_y);

private:
    virtual bool is_node_with_style() const final { return true; }

    void reset_table_box_computed_values_used_by_wrapper_to_init_values();
    void propagate_style_to_anonymous_wrappers();
    void publish_style_record_to_node_data();

    void rebuild_image_observers();
    CSS::ComputedValues const& owned_computed_values() const;
    RefPtr<CSS::ComputedValues const> m_owned_computed_values;
    CSS::StyleRecordID m_style_record_identity;
    GC::Root<DOM::Document> m_style_record_owner;
    Vector<NonnullOwnPtr<ImageObserver>> m_image_observers;
    Vector<RefPtr<CSS::CursorStyleValue const>> m_cursor_style_values;
    mutable Optional<Vector<CSS::BackgroundLayerData>> m_background_layers;
    mutable Optional<Vector<CSS::BackgroundLayerData>> m_mask_layers;
    mutable Optional<CSS::BorderImageData> m_border_image;
    mutable Optional<CSS::ListStyleType> m_list_style_type;
    mutable Optional<RefPtr<CSS::AbstractImageStyleValue const>> m_list_style_image;
    Optional<CSS::ContentData> m_content;
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

inline Gfx::Font const& Node::font(DisplayListRecordingContext& context) const
{
    return font(context.device_pixels_per_css_pixel());
}

inline Gfx::Font const& Node::font(float scale_factor) const
{
    auto const& font = first_available_font();
    return font.with_size(font.point_size() * scale_factor);
}

inline NodeWithStyle const* Node::parent() const
{
    return static_cast<NodeWithStyle const*>(Base::parent_ptr());
}

inline NodeWithStyle* Node::parent()
{
    return static_cast<NodeWithStyle*>(Base::parent_ptr());
}

inline Gfx::Font const& NodeWithStyle::first_available_font() const
{
    // https://drafts.csswg.org/css-fonts/#first-available-font
    // First font for which the character U+0020 (space) is not excluded by a unicode-range
    return font_list().font_for_code_point(' ');
}

bool overflow_value_makes_box_a_scroll_container(CSS::Overflow overflow);

}
