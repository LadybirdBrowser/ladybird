/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/Layout/AnonymousBoxStyle.h>

namespace Web::Layout {

static void transfer_table_box_style_to_wrapper(CSS::ComputedValues const& table_box_values, CSS::ComputedValues::Builder& wrapper)
{
    // The computed values of properties 'position', 'float', 'margin-*', 'top', 'right', 'bottom', and 'left' on the table element are used on the table wrapper box and not the table box;
    // all other values of non-inheritable properties are used on the table box and not the table wrapper box.
    // (Where the table element's values are not used on the table and table wrapper boxes, the initial values are used instead.)
    if (table_box_values.display().is_inline_outside())
        wrapper->set_display(CSS::Display::from_short(CSS::Display::Short::InlineBlock));
    else
        wrapper->set_display(CSS::Display::from_short(CSS::Display::Short::FlowRoot));
    wrapper->set_position(table_box_values.position());
    wrapper->set_position_anchor(table_box_values.position_anchor_value());
    wrapper->set_inset(table_box_values.inset());
    wrapper->set_float(table_box_values.float_());
    wrapper->set_clear(table_box_values.clear());
    // CSS 2 moves table-root positioning and margins to the wrapper. The wrapper is also the grid item for
    // display:table, so grid placement, self-alignment, and order need to move there as well.
    wrapper->copy_grid_placements_from(table_box_values);
    wrapper->set_align_self(table_box_values.align_self());
    wrapper->set_justify_self(table_box_values.justify_self());
    wrapper->set_order(table_box_values.order());
    wrapper->set_margin(table_box_values.margin());
    // AD-HOC:
    // To match other browsers, z-index needs to be moved to the wrapper box as well,
    // even if the spec does not mention that: https://github.com/w3c/csswg-drafts/issues/11689
    // Note that there may be more properties that need to be added to this list.
    wrapper->set_z_index(table_box_values.z_index());
    // "clip" only takes effect on absolutely-positioned elements; the table box isn't one — the wrapper is.
    wrapper->set_clip(table_box_values.clip());
    // AD-HOC: The wrapper box participates in inline layout in place of the table box, so vertical-align
    //         must be moved to the wrapper to have any effect.
    wrapper->set_vertical_align(table_box_values.vertical_align());
}

static CSS::StyleRecordID intern_and_pin_anonymous_box_style(CSS::StyleComputer const& style_computer, CSS::ComputedValues::Builder&& builder)
{
    auto style_record = style_computer.intern_anonymous_layout_style(*move(builder).build());
    style_computer.pin_style_record(style_record);
    return style_record;
}

CSS::StyleRecordID derive_pinned_anonymous_box_style_record(CSS::StyleComputer const& style_computer, CSS::StyleRecordID parent_style_record, RustFFI::FfiAnonymousStyleKind kind, RustFFI::FfiAnonymousStyleOverrides const& overrides)
{
    auto parent_values = style_computer.computed_style_record_view(parent_style_record);
    VERIFY(parent_values);
    auto builder = kind == RustFFI::FfiAnonymousStyleKind::InlineStyleWrapper
        ? CSS::ComputedValues::Builder { *parent_values }
        : CSS::ComputedValues::Builder::create_inheriting_from(*parent_values);
    auto block_flow_or_inline_block = overrides.inline_block_wrapper
        ? CSS::Display::from_short(CSS::Display::Short::InlineBlock)
        : CSS::Display(CSS::DisplayOutside::Block, CSS::DisplayInside::Flow);
    switch (kind) {
    case RustFFI::FfiAnonymousStyleKind::Wrapper:
        builder->set_display(block_flow_or_inline_block);
        break;
    case RustFFI::FfiAnonymousStyleKind::TableRow:
        builder->set_display(CSS::Display { CSS::DisplayInternal::TableRow });
        break;
    case RustFFI::FfiAnonymousStyleKind::TableCell:
        builder->set_display(CSS::Display { CSS::DisplayInternal::TableCell });
        break;
    case RustFFI::FfiAnonymousStyleKind::Table:
        builder->set_display(CSS::Display::from_short(CSS::Display::Short::Table));
        break;
    case RustFFI::FfiAnonymousStyleKind::InlineTable:
        builder->set_display(CSS::Display::from_short(CSS::Display::Short::InlineTable));
        break;
    case RustFFI::FfiAnonymousStyleKind::MissingTableCell:
        builder->set_display(CSS::Display { CSS::DisplayInternal::TableCell });
        // Ensure that the cell (with zero content height) will have the same height as the row by setting vertical-align to middle.
        builder->set_vertical_align(CSS::VerticalAlign::Middle);
        break;
    case RustFFI::FfiAnonymousStyleKind::TableWrapper:
        transfer_table_box_style_to_wrapper(*parent_values, builder);
        break;
    case RustFFI::FfiAnonymousStyleKind::ButtonFlexWrapper:
        // A full-height flex column that centers the button contents vertically.
        builder->set_display(CSS::Display { CSS::DisplayOutside::Block, CSS::DisplayInside::Flex });
        builder->set_justify_content(CSS::JustifyContent::Center);
        builder->set_flex_direction(CSS::FlexDirection::Column);
        builder->set_height(CSS::Size::make_percentage(CSS::Percentage(100)));
        break;
    case RustFFI::FfiAnonymousStyleKind::ButtonContentBox:
        // Let percentage-sized descendants shrink to fixed-height buttons instead of the flex
        // item's automatic minimum size.
        builder->set_display(block_flow_or_inline_block);
        builder->set_min_height(CSS::Size::make_px(CSSPixels(0)));
        break;
    case RustFFI::FfiAnonymousStyleKind::FieldsetContentWrapper:
        builder->set_display(CSS::Display::from_short(CSS::Display::Short::FlowRoot));
        builder->set_overflow_x(static_cast<CSS::Overflow>(overrides.overflow_x));
        builder->set_overflow_y(static_cast<CSS::Overflow>(overrides.overflow_y));
        break;
    case RustFFI::FfiAnonymousStyleKind::InlineStyleWrapper:
        builder->set_display(CSS::Display(CSS::DisplayOutside::Inline, CSS::DisplayInside::Flow));
        break;
    }
    return intern_and_pin_anonymous_box_style(style_computer, move(builder));
}

CSS::StyleRecordID reinherit_pinned_anonymous_box_style_record(CSS::StyleComputer const& style_computer, CSS::StyleRecordID style_record, CSS::StyleRecordID parent_style_record)
{
    auto values = style_computer.computed_style_record_view(style_record);
    VERIFY(values);
    auto parent_values = style_computer.computed_style_record_view(parent_style_record);
    VERIFY(parent_values);
    CSS::ComputedValues::Builder builder { *values };
    builder->inherit_from(*parent_values);
    return intern_and_pin_anonymous_box_style(style_computer, move(builder));
}

}
