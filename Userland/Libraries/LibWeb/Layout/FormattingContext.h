/*
 * Copyright (c) 2020-2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Layout/AvailableSpace.h>
#include <LibWeb/Layout/LayoutState.h>

namespace Web::Layout {

// NOTE: We use a custom clamping function here instead of AK::clamp(), since the AK version
//       will VERIFY(max >= min) and CSS explicitly allows that (see css-values-4.)
template<typename T>
[[nodiscard]] constexpr T css_clamp(T const& value, T const& min, T const& max)
{
    return ::max(min, ::min(value, max));
}

class FormattingContext {
public:
    virtual ~FormattingContext();

    enum class Type {
        Block,
        Inline,
        Flex,
        Grid,
        Table,
        SVG,
        InternalReplaced, // Internal hack formatting context for replaced elements. FIXME: Get rid of this.
        InternalDummy,    // Internal hack formatting context for unimplemented things. FIXME: Get rid of this.
    };

    virtual void run(AvailableSpace const&) = 0;

    // This function returns the automatic content height of the context's root box.
    virtual CSSPixels automatic_content_width() const = 0;

    // This function returns the automatic content height of the context's root box.
    virtual CSSPixels automatic_content_height() const = 0;

    Box const& context_box() const { return m_context_box; }

    FormattingContext* parent() { return m_parent; }
    FormattingContext const* parent() const { return m_parent; }

    Type type() const { return m_type; }
    bool is_block_formatting_context() const { return type() == Type::Block; }

    virtual bool inhibits_floating() const { return false; }

    [[nodiscard]] static Optional<Type> formatting_context_type_created_by_box(Box const&);

    static bool creates_block_formatting_context(Box const&);

    CSSPixels compute_table_box_width_inside_table_wrapper(Box const&, AvailableSpace const&);
    CSSPixels compute_table_box_height_inside_table_wrapper(Box const&, AvailableSpace const&);

    CSSPixels compute_width_for_replaced_element(Box const&, AvailableSpace const&) const;
    CSSPixels compute_height_for_replaced_element(Box const&, AvailableSpace const&) const;

    OwnPtr<FormattingContext> create_independent_formatting_context_if_needed(LayoutState&, LayoutMode, Box const& child_box);

    virtual void parent_context_did_dimension_child_root_box() { }

    CSSPixels calculate_min_content_width(Layout::Box const&) const;
    CSSPixels calculate_max_content_width(Layout::Box const&) const;
    CSSPixels calculate_min_content_height(Layout::Box const&, CSSPixels width) const;
    CSSPixels calculate_max_content_height(Layout::Box const&, CSSPixels width) const;

    CSSPixels calculate_fit_content_height(Layout::Box const&, AvailableSpace const&) const;
    CSSPixels calculate_fit_content_width(Layout::Box const&, AvailableSpace const&) const;

    CSSPixels calculate_inner_width(Layout::Box const&, AvailableSize const&, CSS::Size const& width) const;
    CSSPixels calculate_inner_height(Layout::Box const&, AvailableSize const&, CSS::Size const& height) const;

    virtual CSSPixels greatest_child_width(Box const&) const;

    [[nodiscard]] CSSPixelRect absolute_content_rect(Box const&) const;
    [[nodiscard]] CSSPixelRect absolute_content_rect(LayoutState::UsedValues const&) const;
    [[nodiscard]] CSSPixelRect margin_box_rect(Box const&) const;
    [[nodiscard]] CSSPixelRect margin_box_rect_in_ancestor_coordinate_space(Box const&, Box const& ancestor_box) const;
    [[nodiscard]] CSSPixelRect margin_box_rect(LayoutState::UsedValues const&) const;
    [[nodiscard]] CSSPixelRect margin_box_rect_in_ancestor_coordinate_space(LayoutState::UsedValues const&, Box const& ancestor_box) const;
    [[nodiscard]] CSSPixelRect border_box_rect(Box const&) const;
    [[nodiscard]] CSSPixelRect border_box_rect(LayoutState::UsedValues const&) const;
    [[nodiscard]] CSSPixelRect border_box_rect_in_ancestor_coordinate_space(Box const&, Box const& ancestor_box) const;
    [[nodiscard]] CSSPixelRect border_box_rect_in_ancestor_coordinate_space(LayoutState::UsedValues const&, Box const& ancestor_box) const;
    [[nodiscard]] CSSPixelRect content_box_rect(Box const&) const;
    [[nodiscard]] CSSPixelRect content_box_rect(LayoutState::UsedValues const&) const;
    [[nodiscard]] CSSPixelRect content_box_rect_in_ancestor_coordinate_space(Box const&, Box const& ancestor_box) const;
    [[nodiscard]] CSSPixelRect content_box_rect_in_ancestor_coordinate_space(LayoutState::UsedValues const&, Box const& ancestor_box) const;
    [[nodiscard]] CSSPixels box_baseline(Box const&) const;
    [[nodiscard]] CSSPixelRect content_box_rect_in_static_position_ancestor_coordinate_space(Box const&, Box const& ancestor_box) const;

    [[nodiscard]] CSSPixels containing_block_width_for(NodeWithStyleAndBoxModelMetrics const&) const;
    [[nodiscard]] CSSPixels containing_block_height_for(NodeWithStyleAndBoxModelMetrics const&) const;

    [[nodiscard]] AvailableSize containing_block_width_as_available_size(NodeWithStyleAndBoxModelMetrics const&) const;
    [[nodiscard]] AvailableSize containing_block_height_as_available_size(NodeWithStyleAndBoxModelMetrics const&) const;

    [[nodiscard]] CSSPixels calculate_stretch_fit_width(Box const&, AvailableSize const&) const;
    [[nodiscard]] CSSPixels calculate_stretch_fit_height(Box const&, AvailableSize const&) const;

    virtual CSSPixelPoint calculate_static_position(Box const&) const;
    bool can_skip_is_anonymous_text_run(Box&);

    void compute_inset(NodeWithStyleAndBoxModelMetrics const&);

protected:
    FormattingContext(Type, LayoutMode, LayoutState&, Box const&, FormattingContext* parent = nullptr);

    static bool should_treat_width_as_auto(Box const&, AvailableSpace const&);
    static bool should_treat_height_as_auto(Box const&, AvailableSpace const&);

    [[nodiscard]] bool should_treat_max_width_as_none(Box const&, AvailableSize const&) const;
    [[nodiscard]] bool should_treat_max_height_as_none(Box const&, AvailableSize const&) const;

    OwnPtr<FormattingContext> layout_inside(Box const&, LayoutMode, AvailableSpace const&);

    struct SpaceUsedByFloats {
        CSSPixels left { 0 };
        CSSPixels right { 0 };
    };

    struct SpaceUsedAndContainingMarginForFloats {
        // Width for left / right floats, including their own margins.
        CSSPixels left_used_space;
        CSSPixels right_used_space;
        // Left / right total margins from the outermost containing block to the floating element.
        // Each block in the containing chain adds its own margin and we store the total here.
        CSSPixels left_total_containing_margin;
        CSSPixels right_total_containing_margin;
        JS::GCPtr<Box const> matching_left_float_box;
    };

    struct ShrinkToFitResult {
        CSSPixels preferred_width { 0 };
        CSSPixels preferred_minimum_width { 0 };
    };

    CSSPixels tentative_width_for_replaced_element(Box const&, CSS::Size const& computed_width, AvailableSpace const&) const;
    CSSPixels tentative_height_for_replaced_element(Box const&, CSS::Size const& computed_height, AvailableSpace const&) const;
    CSSPixels compute_auto_height_for_block_formatting_context_root(Box const&) const;

    [[nodiscard]] CSSPixelSize solve_replaced_size_constraint(CSSPixels input_width, CSSPixels input_height, Box const&, AvailableSpace const&) const;

    ShrinkToFitResult calculate_shrink_to_fit_widths(Box const&);

    void layout_absolutely_positioned_element(Box const&, AvailableSpace const&);
    void compute_width_for_absolutely_positioned_element(Box const&, AvailableSpace const&);
    void compute_width_for_absolutely_positioned_non_replaced_element(Box const&, AvailableSpace const&);
    void compute_width_for_absolutely_positioned_replaced_element(Box const&, AvailableSpace const&);

    enum class BeforeOrAfterInsideLayout {
        Before,
        After,
    };
    void compute_height_for_absolutely_positioned_element(Box const&, AvailableSpace const&, BeforeOrAfterInsideLayout);
    void compute_height_for_absolutely_positioned_non_replaced_element(Box const&, AvailableSpace const&, BeforeOrAfterInsideLayout);
    void compute_height_for_absolutely_positioned_replaced_element(Box const&, AvailableSpace const&, BeforeOrAfterInsideLayout);

    [[nodiscard]] Optional<CSSPixels> compute_auto_height_for_absolutely_positioned_element(Box const&, AvailableSpace const&, BeforeOrAfterInsideLayout) const;

    [[nodiscard]] Box const* box_child_to_derive_baseline_from(Box const&) const;

    Type m_type {};
    LayoutMode m_layout_mode;

    FormattingContext* m_parent { nullptr };
    JS::NonnullGCPtr<Box const> m_context_box;

    LayoutState& m_state;
};

bool box_is_sized_as_replaced_element(Box const&);

}
