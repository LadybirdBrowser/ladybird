/*
 * Copyright (c) 2020-2022, Andreas Kling <kling@serenityos.org>
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
    explicit BlockFormattingContext(LayoutState&, BlockContainer const&, FormattingContext* parent);
    ~BlockFormattingContext();

    virtual void run(Box const&, LayoutMode, AvailableSpace const&) override;
    virtual CSSPixels automatic_content_width() const override;
    virtual CSSPixels automatic_content_height() const override;

    auto const& left_side_floats() const { return m_left_floats; }
    auto const& right_side_floats() const { return m_right_floats; }

    bool box_should_avoid_floats_because_it_establishes_fc(Box const&);
    void compute_width(Box const&, AvailableSpace const&, LayoutMode = LayoutMode::Normal);

    // https://www.w3.org/TR/css-display/#block-formatting-context-root
    BlockContainer const& root() const { return static_cast<BlockContainer const&>(context_box()); }

    virtual void parent_context_did_dimension_child_root_box() override;

    void compute_height(Box const&, AvailableSpace const&);

    void add_absolutely_positioned_box(Box const& box) { m_absolutely_positioned_boxes.append(box); }

    SpaceUsedAndContainingMarginForFloats space_used_and_containing_margin_for_floats(CSSPixels y) const;
    [[nodiscard]] SpaceUsedByFloats intrusion_by_floats_into_box(Box const&, CSSPixels y_in_box) const;
    [[nodiscard]] SpaceUsedByFloats intrusion_by_floats_into_box(LayoutState::UsedValues const&, CSSPixels y_in_box) const;

    virtual CSSPixels greatest_child_width(Box const&) const override;

    void layout_floating_box(Box const& child, BlockContainer const& containing_block, LayoutMode, AvailableSpace const&, CSSPixels y, LineBuilder* = nullptr);

    void layout_block_level_box(Box const&, BlockContainer const&, LayoutMode, CSSPixels& bottom_of_lowest_margin_box, AvailableSpace const&);

    void resolve_vertical_box_model_metrics(Box const&);

    enum class DidIntroduceClearance {
        Yes,
        No,
    };

    [[nodiscard]] DidIntroduceClearance clear_floating_boxes(Node const& child_box, Optional<InlineFormattingContext&> inline_formatting_context);

    void reset_margin_state() { m_margin_state.reset(); }

private:
    CSSPixels compute_auto_height_for_block_level_element(Box const&, AvailableSpace const&);

    void compute_width_for_floating_box(Box const&, AvailableSpace const&);

    void compute_width_for_block_level_replaced_element_in_normal_flow(Box const&, AvailableSpace const&);

    void layout_viewport(LayoutMode, AvailableSpace const&);

    void layout_block_level_children(BlockContainer const&, LayoutMode, AvailableSpace const&);
    void layout_inline_children(BlockContainer const&, LayoutMode, AvailableSpace const&);

    void place_block_level_element_in_normal_flow_horizontally(Box const& child_box, AvailableSpace const&);
    void place_block_level_element_in_normal_flow_vertically(Box const&, CSSPixels y);

    void ensure_sizes_correct_for_left_offset_calculation(ListItemBox const&);
    void layout_list_item_marker(ListItemBox const&, CSSPixels const& left_space_before_list_item_elements_formatted);

    void measure_scrollable_overflow(Box const&, CSSPixels& bottom_edge, CSSPixels& right_edge) const;

    enum class FloatSide {
        Left,
        Right,
    };

    struct FloatingBox {
        JS::NonnullGCPtr<Box const> box;

        LayoutState::UsedValues& used_values;

        // Offset from left/right edge to the left content edge of `box`.
        CSSPixels offset_from_edge { 0 };

        // Top margin edge of `box`.
        CSSPixels top_margin_edge { 0 };

        // Bottom margin edge of `box`.
        CSSPixels bottom_margin_edge { 0 };
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

    struct BlockMarginState {
        Vector<CSSPixels> current_collapsible_margins;
        Function<void(CSSPixels)> block_container_y_position_update_callback;
        bool box_last_in_flow_child_margin_bottom_collapsed { false };

        void add_margin(CSSPixels margin)
        {
            current_collapsible_margins.append(margin);
        }

        void register_block_container_y_position_update_callback(ESCAPING Function<void(CSSPixels)> callback)
        {
            block_container_y_position_update_callback = move(callback);
        }

        CSSPixels current_collapsed_margin() const;

        bool has_block_container_waiting_for_final_y_position() const
        {
            return static_cast<bool>(block_container_y_position_update_callback);
        }

        void update_block_waiting_for_final_y_position() const
        {
            if (block_container_y_position_update_callback) {
                CSSPixels collapsed_margin = current_collapsed_margin();
                block_container_y_position_update_callback(collapsed_margin);
            }
        }

        void reset()
        {
            block_container_y_position_update_callback = {};
            current_collapsible_margins.clear();
        }
    };

    Optional<CSSPixels> m_y_offset_of_current_block_container;

    BlockMarginState m_margin_state;

    FloatSideData m_left_floats;
    FloatSideData m_right_floats;

    Vector<JS::NonnullGCPtr<Box const>> m_absolutely_positioned_boxes;

    bool m_was_notified_after_parent_dimensioned_my_root_box { false };
};

}
