/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/FormattingContext.h>
#include <LibWeb/Layout/InlineFormattingContext.h>

namespace Web::Layout {

class LineBuilder;

// https://www.w3.org/TR/css-display/#block-formatting-context
class BlockFormattingContext : public FormattingContext {
public:
    explicit BlockFormattingContext(LayoutState&, LayoutMode layout_mode, BlockContainer const&, FormattingContext* parent);
    ~BlockFormattingContext();

    virtual void run(AvailableSpace const&) override;
    virtual CSSPixels automatic_content_width() const override;
    virtual CSSPixels automatic_content_height() const override;

    auto const& left_side_floats() const { return m_left_floats; }
    auto const& right_side_floats() const { return m_right_floats; }

    bool box_should_avoid_floats_because_it_establishes_fc(Box const&);
    void compute_width(Box const&, AvailableSpace const&);
    void avoid_float_intrusions(Box const&, AvailableSpace const&);

    // https://www.w3.org/TR/css-display/#block-formatting-context-root
    BlockContainer const& root() const { return static_cast<BlockContainer const&>(context_box()); }

    virtual void parent_context_did_dimension_child_root_box() override;

    void resolve_used_height_if_not_treated_as_auto(Box const&, AvailableSpace const&);
    void resolve_used_height_if_treated_as_auto(Box const&, AvailableSpace const&, FormattingContext const* box_formatting_context = nullptr);

    template<typename Callback>
    void for_each_floating_box(Callback callback)
    {
        for (auto const& floating_box : m_left_floats.all_boxes) {
            if (callback(*floating_box) == IterationDecision::Break)
                return;
        }
        for (auto const& floating_box : m_right_floats.all_boxes) {
            if (callback(*floating_box) == IterationDecision::Break)
                return;
        }
    }

    SpaceUsedAndContainingMarginForFloats space_used_and_containing_margin_for_floats(CSSPixels y) const;
    [[nodiscard]] SpaceUsedByFloats intrusion_by_floats_into_box(Box const&, CSSPixels y_in_box) const;
    [[nodiscard]] SpaceUsedByFloats intrusion_by_floats_into_box(LayoutState::UsedValues const&, CSSPixels y_in_box) const;

    virtual CSSPixels greatest_child_width(Box const&) const override;

    void layout_floating_box(Box const& child, BlockContainer const& containing_block, AvailableSpace const&, CSSPixels y, LineBuilder* = nullptr);

    void layout_block_level_box(Box const&, BlockContainer const&, CSSPixels& bottom_of_lowest_margin_box, AvailableSpace const&);

    void resolve_vertical_box_model_metrics(Box const&, CSSPixels width_of_containing_block);

    enum class DidIntroduceClearance {
        Yes,
        No,
    };

    [[nodiscard]] DidIntroduceClearance clear_floating_boxes(Node const& child_box, Optional<InlineFormattingContext&> inline_formatting_context);

    void reset_margin_state() { m_margin_state.reset(); }

    struct FloatingBox {
        GC::Ref<Box const> box;

        LayoutState::UsedValues& used_values;

        // Offset from left/right edge to the left content edge of `box`.
        CSSPixels offset_from_edge { 0 };

        // Top margin edge of `box`.
        CSSPixels top_margin_edge { 0 };

        // Bottom margin edge of `box`.
        CSSPixels bottom_margin_edge { 0 };

        CSSPixelRect margin_box_rect_in_root_coordinate_space;
    };

    Optional<FloatingBox&> last_inserted_float() { return m_last_inserted_float; }

private:
    CSSPixels compute_auto_height_for_block_level_element(Box const&, AvailableSpace const&);

    void compute_width_for_floating_box(Box const&, AvailableSpace const&);

    void compute_width_for_block_level_replaced_element_in_normal_flow(Box const&, AvailableSpace const&);

    void layout_viewport(AvailableSpace const&);

    void layout_block_level_children(BlockContainer const&, AvailableSpace const&);
    void layout_inline_children(BlockContainer const&, AvailableSpace const&);

    void place_block_level_element_in_normal_flow_horizontally(Box const& child_box, AvailableSpace const&);
    void place_block_level_element_in_normal_flow_vertically(Box const&, CSSPixels y);

    void ensure_sizes_correct_for_left_offset_calculation(ListItemBox const&);
    void layout_list_item_marker(ListItemBox const&, CSSPixels const& left_space_before_list_item_elements_formatted);

    void measure_scrollable_overflow(Box const&, CSSPixels& bottom_edge, CSSPixels& right_edge) const;

    // https://drafts.csswg.org/css-multicol/#pseudo-algorithm
    Optional<int> determine_used_value_for_column_count(CSSPixels const& U) const;
    CSSPixels determine_used_value_for_column_width(CSSPixels const& U, int N) const;

    CSSPixels get_column_gap_used_value_for_multicol(CSSPixels const& U) const;

    enum class FloatSide {
        Left,
        Right,
    };

    struct FloatSideData {
        // Floating boxes currently accumulating on this side.
        Vector<FloatingBox&> current_boxes;

        // Combined width of boxes currently accumulating on this side.
        // This is the innermost margin of the innermost floating box.
        CSSPixels current_width { 0 };

        // Highest value of `m_current_width` we've seen.
        CSSPixels max_width { 0 };

        // All floating boxes encountered thus far within this BFC.
        Vector<NonnullOwnPtr<FloatingBox>> all_boxes;

        // Current Y offset from BFC root top.
        CSSPixels y_offset { 0 };

        void clear()
        {
            current_boxes.clear();
            current_width = 0;
        }
    };

    class BlockMarginState {
    public:
        void add_margin(CSSPixels margin)
        {
            if (margin < 0) {
                m_current_negative_collapsible_margin = min(margin, m_current_negative_collapsible_margin);
            } else {
                m_current_positive_collapsible_margin = max(margin, m_current_positive_collapsible_margin);
            }
        }

        void register_block_container_y_position_update_callback(ESCAPING Function<void(CSSPixels)> callback)
        {
            m_block_container_y_position_update_callback = move(callback);
        }

        CSSPixels current_collapsed_margin() const
        {
            return m_current_positive_collapsible_margin + m_current_negative_collapsible_margin;
        }

        bool has_block_container_waiting_for_final_y_position() const
        {
            return static_cast<bool>(m_block_container_y_position_update_callback);
        }

        void update_block_waiting_for_final_y_position() const
        {
            if (m_block_container_y_position_update_callback) {
                CSSPixels collapsed_margin = current_collapsed_margin();
                m_block_container_y_position_update_callback(collapsed_margin);
            }
        }

        void reset()
        {
            m_block_container_y_position_update_callback = {};
            m_current_negative_collapsible_margin = 0;
            m_current_positive_collapsible_margin = 0;
        }

        bool box_last_in_flow_child_margin_bottom_collapsed() const { return m_box_last_in_flow_child_margin_bottom_collapsed; }
        void set_box_last_in_flow_child_margin_bottom_collapsed(bool v) { m_box_last_in_flow_child_margin_bottom_collapsed = v; }

    private:
        CSSPixels m_current_positive_collapsible_margin;
        CSSPixels m_current_negative_collapsible_margin;
        Function<void(CSSPixels)> m_block_container_y_position_update_callback;
        bool m_box_last_in_flow_child_margin_bottom_collapsed { false };
    };

    Optional<CSSPixels> m_y_offset_of_current_block_container;

    BlockMarginState m_margin_state;

    FloatSideData m_left_floats;
    FloatSideData m_right_floats;
    Optional<FloatingBox&> m_last_inserted_float;

    bool m_was_notified_after_parent_dimensioned_my_root_box { false };
};

}
