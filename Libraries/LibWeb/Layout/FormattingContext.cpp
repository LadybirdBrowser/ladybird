/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Dump.h>
#include <LibWeb/Layout/BlockFormattingContext.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/FlexFormattingContext.h>
#include <LibWeb/Layout/FormattingContext.h>
#include <LibWeb/Layout/GridFormattingContext.h>
#include <LibWeb/Layout/ReplacedBox.h>
#include <LibWeb/Layout/SVGFormattingContext.h>
#include <LibWeb/Layout/SVGSVGBox.h>
#include <LibWeb/Layout/TableFormattingContext.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Layout/Viewport.h>

namespace Web::Layout {

FormattingContext::FormattingContext(Type type, LayoutMode layout_mode, LayoutState& state, Box const& context_box, FormattingContext* parent)
    : m_type(type)
    , m_layout_mode(layout_mode)
    , m_parent(parent)
    , m_context_box(context_box)
    , m_state(state)
{
}

FormattingContext::~FormattingContext() = default;

// https://developer.mozilla.org/en-US/docs/Web/Guide/CSS/Block_formatting_context
bool FormattingContext::creates_block_formatting_context(Box const& box)
{
    // NOTE: Replaced elements never create a BFC.
    if (box.is_replaced_box())
        return false;

    // display: table
    if (box.display().is_table_inside()) {
        return false;
    }

    // display: flex
    if (box.display().is_flex_inside()) {
        return false;
    }

    // display: grid
    if (box.display().is_grid_inside()) {
        return false;
    }

    // NOTE: This function uses MDN as a reference, not because it's authoritative,
    //       but because they've gathered all the conditions in one convenient location.

    // The root element of the document (<html>).
    if (box.is_root_element())
        return true;

    // Floats (elements where float isn't none).
    if (box.is_floating())
        return true;

    // Absolutely positioned elements (elements where position is absolute or fixed).
    if (box.is_absolutely_positioned())
        return true;

    // Inline-blocks (elements with display: inline-block).
    if (box.display().is_inline_block())
        return true;

    // Table cells (elements with display: table-cell, which is the default for HTML table cells).
    if (box.display().is_table_cell())
        return true;

    // Table captions (elements with display: table-caption, which is the default for HTML table captions).
    if (box.display().is_table_caption())
        return true;

    // FIXME: Anonymous table cells implicitly created by the elements with display: table, table-row, table-row-group, table-header-group, table-footer-group
    //        (which is the default for HTML tables, table rows, table bodies, table headers, and table footers, respectively), or inline-table.

    // Block elements where overflow has a value other than visible and clip.
    CSS::Overflow overflow_x = box.computed_values().overflow_x();
    if ((overflow_x != CSS::Overflow::Visible) && (overflow_x != CSS::Overflow::Clip))
        return true;
    CSS::Overflow overflow_y = box.computed_values().overflow_y();
    if ((overflow_y != CSS::Overflow::Visible) && (overflow_y != CSS::Overflow::Clip))
        return true;

    // display: flow-root.
    if (box.display().is_flow_root_inside())
        return true;

    // https://drafts.csswg.org/css-contain-2/#containment-types
    // 1. The layout containment box establishes an independent formatting context.
    // 4. The paint containment box establishes an independent formatting context.
    if (box.dom_node() && box.dom_node()->is_element()) {
        auto element = as<DOM::Element>(box.dom_node());
        if (element->has_layout_containment() || element->has_paint_containment())
            return true;
    }

    if (box.parent()) {
        auto parent_display = box.parent()->display();

        // Flex items (direct children of the element with display: flex or inline-flex) if they are neither flex nor grid nor table containers themselves.
        if (parent_display.is_flex_inside())
            return true;
        // Grid items (direct children of the element with display: grid or inline-grid) if they are neither flex nor grid nor table containers themselves.
        if (parent_display.is_grid_inside())
            return true;
    }

    // FIXME: Multicol containers (elements where column-count or column-width isn't auto, including elements with column-count: 1).

    // FIXME: column-span: all should always create a new formatting context, even when the column-span: all element isn't contained by a multicol container (Spec change, Chrome bug).

    // https://html.spec.whatwg.org/multipage/rendering.html#the-fieldset-and-legend-elements
    if (box.is_fieldset_box())
        // The fieldset element, when it generates a CSS box, is expected to act as follows:
        // The element is expected to establish a new block formatting context.
        return true;

    return false;
}

Optional<FormattingContext::Type> FormattingContext::formatting_context_type_created_by_box(Box const& box)
{
    if (box.is_replaced_box() && !box.can_have_children()) {
        return Type::InternalReplaced;
    }

    if (!box.can_have_children())
        return {};

    if (is<SVGSVGBox>(box))
        return Type::SVG;

    auto display = box.display();

    if (display.is_flex_inside())
        return Type::Flex;

    if (display.is_table_inside())
        return Type::Table;

    if (display.is_grid_inside())
        return Type::Grid;

    if (display.is_math_inside())
        // HACK: Instead of crashing, create a dummy formatting context that does nothing.
        return Type::InternalDummy;

    if (creates_block_formatting_context(box))
        return Type::Block;

    if (box.children_are_inline())
        return {};

    if (display.is_table_column() || display.is_table_row_group() || display.is_table_header_group() || display.is_table_footer_group() || display.is_table_row() || display.is_table_column_group())
        return {};

    // The box is a block container that doesn't create its own BFC.
    // It will be formatted by the containing BFC.
    if (!display.is_flow_inside()) {
        // HACK: Instead of crashing, create a dummy formatting context that does nothing.
        // FIXME: We need this for <math> elements
        return Type::InternalDummy;
    }
    return {};
}

// FIXME: This is a hack. Get rid of it.
struct ReplacedFormattingContext : public FormattingContext {
    ReplacedFormattingContext(LayoutState& state, LayoutMode layout_mode, Box const& box)
        : FormattingContext(Type::Block, layout_mode, state, box)
    {
    }
    virtual CSSPixels automatic_content_width() const override { return 0; }
    virtual CSSPixels automatic_content_height() const override { return 0; }
    virtual void run(AvailableSpace const&) override { }
};

// FIXME: This is a hack. Get rid of it.
struct DummyFormattingContext : public FormattingContext {
    DummyFormattingContext(LayoutState& state, LayoutMode layout_mode, Box const& box)
        : FormattingContext(Type::Block, layout_mode, state, box)
    {
    }
    virtual CSSPixels automatic_content_width() const override { return 0; }
    virtual CSSPixels automatic_content_height() const override { return 0; }
    virtual void run(AvailableSpace const&) override { }
};

OwnPtr<FormattingContext> FormattingContext::create_independent_formatting_context_if_needed(LayoutState& state, LayoutMode layout_mode, Box const& child_box)
{
    auto type = formatting_context_type_created_by_box(child_box);
    if (!type.has_value())
        return nullptr;

    switch (type.value()) {
    case Type::Block:
        return make<BlockFormattingContext>(state, layout_mode, as<BlockContainer>(child_box), this);
    case Type::SVG:
        return make<SVGFormattingContext>(state, layout_mode, child_box, this);
    case Type::Flex:
        return make<FlexFormattingContext>(state, layout_mode, child_box, this);
    case Type::Grid:
        return make<GridFormattingContext>(state, layout_mode, child_box, this);
    case Type::Table:
        return make<TableFormattingContext>(state, layout_mode, child_box, this);
    case Type::InternalReplaced:
        return make<ReplacedFormattingContext>(state, layout_mode, child_box);
    case Type::InternalDummy:
        return make<DummyFormattingContext>(state, layout_mode, child_box);
    case Type::Inline:
        // IFC should always be created by a parent BFC directly.
        VERIFY_NOT_REACHED();
        break;
    default:
        VERIFY_NOT_REACHED();
    }
}

OwnPtr<FormattingContext> FormattingContext::layout_inside(Box const& child_box, LayoutMode layout_mode, AvailableSpace const& available_space)
{
    {
        // OPTIMIZATION: If we're doing intrinsic sizing and `child_box` has definite size in both axes,
        //               we don't need to layout its insides. The size is resolvable without learning
        //               the metrics of whatever's inside the box.
        auto const& used_values = m_state.get(child_box);
        if (layout_mode == LayoutMode::IntrinsicSizing
            && used_values.width_constraint == SizeConstraint::None
            && used_values.height_constraint == SizeConstraint::None
            && used_values.has_definite_width()
            && used_values.has_definite_height()) {
            return nullptr;
        }
    }

    if (!child_box.can_have_children())
        return {};

    auto independent_formatting_context = create_independent_formatting_context_if_needed(m_state, layout_mode, child_box);
    if (independent_formatting_context)
        independent_formatting_context->run(available_space);
    else
        run(available_space);

    return independent_formatting_context;
}

CSSPixels FormattingContext::greatest_child_width(Box const& box) const
{
    CSSPixels max_width = 0;
    if (box.children_are_inline()) {
        for (auto& line_box : m_state.get(box).line_boxes) {
            max_width = max(max_width, line_box.width());
        }
    } else {
        box.for_each_child_of_type<Box>([&](Box const& child) {
            if (!child.is_absolutely_positioned())
                max_width = max(max_width, m_state.get(child).margin_box_width());
            return IterationDecision::Continue;
        });
    }
    return max_width;
}

FormattingContext::ShrinkToFitResult FormattingContext::calculate_shrink_to_fit_widths(Box const& box)
{
    return {
        .preferred_width = calculate_max_content_width(box),
        .preferred_minimum_width = calculate_min_content_width(box),
    };
}

CSSPixelSize FormattingContext::solve_replaced_size_constraint(CSSPixels input_width, CSSPixels input_height, Box const& box, AvailableSpace const& available_space) const
{
    // 10.4 Minimum and maximum widths: 'min-width' and 'max-width'
    // https://www.w3.org/TR/CSS22/visudet.html#min-max-widths

    auto const& containing_block = *box.non_anonymous_containing_block();
    auto const& containing_block_state = m_state.get(containing_block);
    auto width_of_containing_block = containing_block_state.content_width();
    auto height_of_containing_block = containing_block_state.content_height();

    auto min_width = box.computed_values().min_width().is_auto() ? 0 : box.computed_values().min_width().to_px(box, width_of_containing_block);
    auto specified_max_width = should_treat_max_width_as_none(box, available_space.width) ? input_width : box.computed_values().max_width().to_px(box, width_of_containing_block);
    auto max_width = max(min_width, specified_max_width);

    auto min_height = box.computed_values().min_height().is_auto() ? 0 : box.computed_values().min_height().to_px(box, height_of_containing_block);
    auto specified_max_height = should_treat_max_height_as_none(box, available_space.height) ? input_height : box.computed_values().max_height().to_px(box, height_of_containing_block);
    auto max_height = max(min_height, specified_max_height);

    CSSPixelFraction aspect_ratio = *box.preferred_aspect_ratio();

    // These are from the "Constraint Violation" table in spec, but reordered so that each condition is
    // interpreted as mutually exclusive to any other.
    if (input_width < min_width && input_height > max_height)
        return { min_width, max_height };
    if (input_width > max_width && input_height < min_height)
        return { max_width, min_height };

    if (input_width > 0 && input_height > 0) {
        if (input_width > max_width && input_height > max_height && max_width / input_width <= max_height / input_height)
            return { max_width, max(min_height, max_width / aspect_ratio) };
        if (input_width > max_width && input_height > max_height && max_width / input_width > max_height / input_height)
            return { max(min_width, max_height * aspect_ratio), max_height };
        if (input_width < min_width && input_height < min_height && min_width / input_width <= min_height / input_height)
            return { min(max_width, min_height * aspect_ratio), min_height };
        if (input_width < min_width && input_height < min_height && min_width / input_width > min_height / input_height)
            return { min_width, min(max_height, min_width / aspect_ratio) };
    }

    if (input_width > max_width)
        return { max_width, max(max_width / aspect_ratio, min_height) };
    if (input_width < min_width)
        return { min_width, min(min_width / aspect_ratio, max_height) };
    if (input_height > max_height)
        return { max(max_height * aspect_ratio, min_width), max_height };
    if (input_height < min_height)
        return { min(min_height * aspect_ratio, max_width), min_height };

    return { input_width, input_height };
}

Optional<CSSPixels> FormattingContext::compute_auto_height_for_absolutely_positioned_element(Box const& box, AvailableSpace const& available_space, BeforeOrAfterInsideLayout before_or_after_inside_layout) const
{
    // NOTE: CSS 2.2 tells us to use the "auto height for block formatting context roots" here.
    //       That's fine as long as the box is a BFC root.
    if (creates_block_formatting_context(box)) {
        if (before_or_after_inside_layout == BeforeOrAfterInsideLayout::Before)
            return {};
        return compute_auto_height_for_block_formatting_context_root(box);
    }

    // NOTE: For anything else, we use the fit-content height.
    //       This should eventually be replaced by the new absolute positioning model:
    //       https://www.w3.org/TR/css-position-3/#abspos-layout
    return calculate_fit_content_height(box, m_state.get(box).available_inner_space_or_constraints_from(available_space));
}

// https://www.w3.org/TR/CSS22/visudet.html#root-height
CSSPixels FormattingContext::compute_auto_height_for_block_formatting_context_root(Box const& root) const
{
    // 10.6.7 'Auto' heights for block formatting context roots
    Optional<CSSPixels> top;
    Optional<CSSPixels> bottom;

    if (root.children_are_inline()) {
        // If it only has inline-level children, the height is the distance between
        // the top content edge and the bottom of the bottommost line box.
        auto const& line_boxes = m_state.get(root).line_boxes;
        top = 0;
        if (!line_boxes.is_empty())
            bottom = line_boxes.last().bottom();
    } else {
        // If it has block-level children, the height is the distance between
        // the top margin-edge of the topmost block-level child box
        // and the bottom margin-edge of the bottommost block-level child box.

        // NOTE: The top margin edge of the topmost block-level child box is the same as the top content edge of the root box.
        top = 0;

        root.for_each_child_of_type<Box>([&](Layout::Box& child_box) {
            // Absolutely positioned children are ignored,
            // and relatively positioned boxes are considered without their offset.
            // Note that the child box may be an anonymous block box.
            if (child_box.is_absolutely_positioned())
                return IterationDecision::Continue;

            // FIXME: This doesn't look right.
            if ((root.computed_values().overflow_y() == CSS::Overflow::Visible) && child_box.is_floating())
                return IterationDecision::Continue;

            auto const& child_box_state = m_state.get(child_box);

            CSSPixels child_box_bottom = child_box_state.offset.y() + child_box_state.content_height() + child_box_state.margin_box_bottom();

            if (!bottom.has_value() || child_box_bottom > bottom.value())
                bottom = child_box_bottom;

            return IterationDecision::Continue;
        });
    }

    // In addition, if the element has any floating descendants
    // whose bottom margin edge is below the element's bottom content edge,
    // then the height is increased to include those edges.
    for (auto floating_box : m_state.get(root).floating_descendants()) {
        // NOTE: Floating box coordinates are relative to their own containing block,
        //       which may or may not be the BFC root.
        auto margin_box = margin_box_rect_in_ancestor_coordinate_space(*floating_box, root);
        CSSPixels floating_box_bottom_margin_edge = margin_box.bottom();
        if (!bottom.has_value() || floating_box_bottom_margin_edge > bottom.value())
            bottom = floating_box_bottom_margin_edge;
    }

    return max(CSSPixels(0.0f), bottom.value_or(0) - top.value_or(0));
}

// 17.5.2 Table width algorithms: the 'table-layout' property
// https://www.w3.org/TR/CSS22/tables.html#width-layout
CSSPixels FormattingContext::compute_table_box_width_inside_table_wrapper(Box const& box, AvailableSpace const& available_space)
{
    // Table wrapper width should be equal to width of table box it contains

    auto const& computed_values = box.computed_values();

    auto width_of_containing_block = available_space.width.to_px_or_zero();

    auto zero_value = CSS::Length::make_px(0);

    auto margin_left = computed_values.margin().left().resolved(box, width_of_containing_block);
    auto margin_right = computed_values.margin().right().resolved(box, width_of_containing_block);

    // If 'margin-left', or 'margin-right' are computed as 'auto', their used value is '0'.
    if (margin_left.is_auto())
        margin_left = zero_value;
    if (margin_right.is_auto())
        margin_right = zero_value;

    // table-wrapper can't have borders or paddings but it might have margin taken from table-root.
    auto available_width = width_of_containing_block - margin_left.to_px(box) - margin_right.to_px(box);

    Optional<Box const&> table_box;
    box.for_each_in_subtree_of_type<Box>([&](Box const& child_box) {
        if (child_box.display().is_table_inside()) {
            table_box = child_box;
            return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    });
    VERIFY(table_box.has_value());

    LayoutState throwaway_state;

    auto& table_box_state = throwaway_state.get_mutable(*table_box);
    auto const& table_box_computed_values = table_box->computed_values();
    table_box_state.border_left = table_box_computed_values.border_left().width;
    table_box_state.border_right = table_box_computed_values.border_right().width;

    auto context = make<TableFormattingContext>(throwaway_state, LayoutMode::IntrinsicSizing, *table_box, this);
    context->run_until_width_calculation(m_state.get(*table_box).available_inner_space_or_constraints_from(available_space));

    auto table_used_width = throwaway_state.get(*table_box).border_box_width();
    return available_space.width.is_definite() ? min(table_used_width, available_width) : table_used_width;
}

// 17.5.3 Table height algorithms
// https://www.w3.org/TR/CSS22/tables.html#height-layout
CSSPixels FormattingContext::compute_table_box_height_inside_table_wrapper(Box const& box, AvailableSpace const& available_space)
{
    // Table wrapper height should be equal to height of table box it contains

    auto const& computed_values = box.computed_values();

    auto width_of_containing_block = available_space.width.to_px_or_zero();
    auto height_of_containing_block = available_space.height.to_px_or_zero();

    auto zero_value = CSS::Length::make_px(0);

    auto margin_top = computed_values.margin().top().resolved(box, width_of_containing_block);
    auto margin_bottom = computed_values.margin().bottom().resolved(box, width_of_containing_block);

    // If 'margin-top', or 'margin-top' are computed as 'auto', their used value is '0'.
    if (margin_top.is_auto())
        margin_top = zero_value;
    if (margin_bottom.is_auto())
        margin_bottom = zero_value;

    // table-wrapper can't have borders or paddings but it might have margin taken from table-root.
    auto available_height = height_of_containing_block - margin_top.to_px(box) - margin_bottom.to_px(box);

    LayoutState throwaway_state;

    auto context = create_independent_formatting_context_if_needed(throwaway_state, LayoutMode::IntrinsicSizing, box);
    VERIFY(context);
    context->run(m_state.get(box).available_inner_space_or_constraints_from(available_space));

    Optional<Box const&> table_box;
    box.for_each_in_subtree_of_type<Box>([&](Box const& child_box) {
        if (child_box.display().is_table_inside()) {
            table_box = child_box;
            return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    });
    VERIFY(table_box.has_value());

    auto table_used_height = throwaway_state.get(*table_box).border_box_height();
    return available_space.height.is_definite() ? min(table_used_height, available_height) : table_used_height;
}

// 10.3.2 Inline, replaced elements, https://www.w3.org/TR/CSS22/visudet.html#inline-replaced-width
CSSPixels FormattingContext::tentative_width_for_replaced_element(Box const& box, CSS::Size const& computed_width, AvailableSpace const& available_space) const
{
    // Treat percentages of indefinite containing block widths as 0 (the initial width).
    if (computed_width.is_percentage() && !m_state.get(*box.containing_block()).has_definite_width())
        return 0;

    auto computed_height = should_treat_height_as_auto(box, available_space) ? CSS::Size::make_auto() : box.computed_values().height();

    CSSPixels used_width = 0;
    if (computed_width.is_auto()) {
        used_width = computed_width.to_px(box, available_space.width.to_px_or_zero());
    } else {
        used_width = calculate_inner_width(box, available_space.width, computed_width);
    }

    // If 'height' and 'width' both have computed values of 'auto' and the element also has an intrinsic width,
    // then that intrinsic width is the used value of 'width'.
    if (computed_height.is_auto() && computed_width.is_auto() && box.has_natural_width())
        return box.natural_width().value();

    // If 'height' and 'width' both have computed values of 'auto' and the element has no intrinsic width,
    // but does have an intrinsic height and intrinsic ratio;
    // or if 'width' has a computed value of 'auto',
    // 'height' has some other computed value, and the element does have an intrinsic ratio; then the used value of 'width' is:
    //
    //     (used height) * (intrinsic ratio)
    if ((computed_height.is_auto() && computed_width.is_auto() && !box.has_natural_width() && box.has_natural_height() && box.has_preferred_aspect_ratio())
        || (computed_width.is_auto() && !computed_height.is_auto() && box.has_preferred_aspect_ratio())) {
        return compute_height_for_replaced_element(box, available_space) * box.preferred_aspect_ratio().value();
    }

    // If 'height' and 'width' both have computed values of 'auto' and the element has an intrinsic ratio but no intrinsic height or width,
    // then the used value of 'width' is undefined in CSS 2.2. However, it is suggested that, if the containing block's width does not itself
    // depend on the replaced element's width, then the used value of 'width' is calculated from the constraint equation used for block-level,
    // non-replaced elements in normal flow.
    if (computed_height.is_auto() && computed_width.is_auto() && !box.has_natural_width() && !box.has_natural_height() && box.has_preferred_aspect_ratio()) {
        return calculate_stretch_fit_width(box, available_space.width);
    }

    // Otherwise, if 'width' has a computed value of 'auto', and the element has an intrinsic width, then that intrinsic width is the used value of 'width'.
    if (computed_width.is_auto() && box.has_natural_width())
        return box.natural_width().value();

    // Otherwise, if 'width' has a computed value of 'auto', but none of the conditions above are met, then the used value of 'width' becomes 300px.
    // If 300px is too wide to fit the device, UAs should use the width of the largest rectangle that has a 2:1 ratio and fits the device instead.
    if (computed_width.is_auto())
        return 300;

    return used_width;
}

void FormattingContext::compute_width_for_absolutely_positioned_element(Box const& box, AvailableSpace const& available_space)
{
    if (box_is_sized_as_replaced_element(box))
        compute_width_for_absolutely_positioned_replaced_element(box, available_space);
    else
        compute_width_for_absolutely_positioned_non_replaced_element(box, available_space);
}

void FormattingContext::compute_height_for_absolutely_positioned_element(Box const& box, AvailableSpace const& available_space, BeforeOrAfterInsideLayout before_or_after_inside_layout)
{
    if (box_is_sized_as_replaced_element(box))
        compute_height_for_absolutely_positioned_replaced_element(box, available_space, before_or_after_inside_layout);
    else
        compute_height_for_absolutely_positioned_non_replaced_element(box, available_space, before_or_after_inside_layout);
}

CSSPixels FormattingContext::compute_width_for_replaced_element(Box const& box, AvailableSpace const& available_space) const
{
    // 10.3.4 Block-level, replaced elements in normal flow...
    // 10.3.2 Inline, replaced elements

    auto zero_value = CSS::Length::make_px(0);
    auto width_of_containing_block = available_space.width.to_px_or_zero();

    auto computed_width = should_treat_width_as_auto(box, available_space) ? CSS::Size::make_auto() : box.computed_values().width();
    auto computed_height = should_treat_height_as_auto(box, available_space) ? CSS::Size::make_auto() : box.computed_values().height();

    // 1. The tentative used width is calculated (without 'min-width' and 'max-width')
    auto used_width = tentative_width_for_replaced_element(box, computed_width, available_space);

    if (computed_width.is_auto() && computed_height.is_auto() && box.has_preferred_aspect_ratio()) {
        CSSPixels w = used_width;
        CSSPixels h = tentative_height_for_replaced_element(box, computed_height, available_space);
        used_width = solve_replaced_size_constraint(w, h, box, available_space).width();
        return used_width;
    }

    // 2. The tentative used width is greater than 'max-width', the rules above are applied again,
    //    but this time using the computed value of 'max-width' as the computed value for 'width'.
    if (!should_treat_max_width_as_none(box, available_space.width)) {
        auto const& computed_max_width = box.computed_values().max_width();
        if (used_width > computed_max_width.to_px(box, width_of_containing_block)) {
            used_width = tentative_width_for_replaced_element(box, computed_max_width, available_space);
        }
    }

    // 3. If the resulting width is smaller than 'min-width', the rules above are applied again,
    //    but this time using the value of 'min-width' as the computed value for 'width'.
    auto computed_min_width = box.computed_values().min_width();
    if (!computed_min_width.is_auto()) {
        if (used_width < computed_min_width.to_px(box, width_of_containing_block)) {
            used_width = tentative_width_for_replaced_element(box, computed_min_width, available_space);
        }
    }

    return used_width;
}

// 10.6.2 Inline replaced elements, block-level replaced elements in normal flow, 'inline-block' replaced elements in normal flow and floating replaced elements
// https://www.w3.org/TR/CSS22/visudet.html#inline-replaced-height
CSSPixels FormattingContext::tentative_height_for_replaced_element(Box const& box, CSS::Size const& computed_height, AvailableSpace const& available_space) const
{
    // If 'height' and 'width' both have computed values of 'auto' and the element also has
    // an intrinsic height, then that intrinsic height is the used value of 'height'.
    if (should_treat_width_as_auto(box, available_space) && should_treat_height_as_auto(box, available_space) && box.has_natural_height())
        return box.natural_height().value();

    // Otherwise, if 'height' has a computed value of 'auto', and the element has an intrinsic ratio then the used value of 'height' is:
    //
    //     (used width) / (intrinsic ratio)
    if (computed_height.is_auto() && box.has_preferred_aspect_ratio())
        return m_state.get(box).content_width() / box.preferred_aspect_ratio().value();

    // Otherwise, if 'height' has a computed value of 'auto', and the element has an intrinsic height, then that intrinsic height is the used value of 'height'.
    if (computed_height.is_auto() && box.has_natural_height())
        return box.natural_height().value();

    // Otherwise, if 'height' has a computed value of 'auto', but none of the conditions above are met,
    // then the used value of 'height' must be set to the height of the largest rectangle that has a 2:1 ratio, has a height not greater than 150px,
    // and has a width not greater than the device width.
    if (computed_height.is_auto())
        return 150;

    // FIXME: Handle cases when available_space is not definite.
    return calculate_inner_height(box, available_space, computed_height);
}

CSSPixels FormattingContext::compute_height_for_replaced_element(Box const& box, AvailableSpace const& available_space) const
{
    // 10.6.2 Inline replaced elements
    // 10.6.4 Block-level replaced elements in normal flow
    // 10.6.6 Floating replaced elements
    // 10.6.10 'inline-block' replaced elements in normal flow

    auto height_of_containing_block = m_state.get(*box.non_anonymous_containing_block()).content_height();
    auto computed_width = should_treat_width_as_auto(box, available_space) ? CSS::Size::make_auto() : box.computed_values().width();
    auto computed_height = should_treat_height_as_auto(box, available_space) ? CSS::Size::make_auto() : box.computed_values().height();

    // 1. The tentative used height is calculated (without 'min-height' and 'max-height')
    CSSPixels used_height = tentative_height_for_replaced_element(box, computed_height, available_space);

    // However, for replaced elements with both 'width' and 'height' computed as 'auto',
    // use the algorithm under 'Minimum and maximum widths'
    // https://www.w3.org/TR/CSS22/visudet.html#min-max-widths
    // to find the used width and height.
    if ((computed_width.is_auto() && computed_height.is_auto() && box.has_preferred_aspect_ratio())
        // NOTE: This is a special case where calling tentative_width_for_replaced_element() would call us right back,
        //       and we'd end up in an infinite loop. So we need to handle this case separately.
        && !(!box.has_natural_width() && box.has_natural_height())) {
        CSSPixels w = tentative_width_for_replaced_element(box, computed_width, available_space);
        CSSPixels h = used_height;
        used_height = solve_replaced_size_constraint(w, h, box, available_space).height();
        return used_height;
    }

    // 2. If this tentative height is greater than 'max-height', the rules above are applied again,
    //    but this time using the value of 'max-height' as the computed value for 'height'.
    if (!should_treat_max_height_as_none(box, available_space.height)) {
        auto const& computed_max_height = box.computed_values().max_height();
        if (used_height > computed_max_height.to_px(box, height_of_containing_block)) {
            used_height = tentative_height_for_replaced_element(box, computed_max_height, available_space);
        }
    }

    // 3. If the resulting height is smaller than 'min-height', the rules above are applied again,
    //    but this time using the value of 'min-height' as the computed value for 'height'.
    auto computed_min_height = box.computed_values().min_height();
    if (!computed_min_height.is_auto()) {
        if (used_height < computed_min_height.to_px(box, height_of_containing_block)) {
            used_height = tentative_height_for_replaced_element(box, computed_min_height, available_space);
        }
    }

    return used_height;
}

void FormattingContext::compute_width_for_absolutely_positioned_non_replaced_element(Box const& box, AvailableSpace const& available_space)
{
    auto width_of_containing_block = available_space.width.to_px_or_zero();
    auto const& computed_values = box.computed_values();
    auto zero_value = CSS::Length::make_px(0);
    auto& box_state = m_state.get_mutable(box);

    auto margin_left = CSS::Length::make_auto();
    auto margin_right = CSS::Length::make_auto();
    auto const border_left = computed_values.border_left().width;
    auto const border_right = computed_values.border_right().width;
    auto const padding_left = box_state.padding_left;
    auto const padding_right = box_state.padding_right;

    auto computed_left = computed_values.inset().left();
    auto computed_right = computed_values.inset().right();
    auto left = computed_values.inset().left().to_px(box, width_of_containing_block);
    auto right = computed_values.inset().right().to_px(box, width_of_containing_block);

    auto try_compute_width = [&](CSS::Length const& a_width) {
        margin_left = computed_values.margin().left().resolved(box, width_of_containing_block);
        margin_right = computed_values.margin().right().resolved(box, width_of_containing_block);

        auto width = a_width;

        auto solve_for_left = [&] {
            return width_of_containing_block - margin_left.to_px(box) - border_left - padding_left - width.to_px(box) - padding_right - border_right - margin_right.to_px(box) - right;
        };

        auto solve_for_width = [&] {
            return CSS::Length::make_px(max(CSSPixels(0), width_of_containing_block - left - margin_left.to_px(box) - border_left - padding_left - padding_right - border_right - margin_right.to_px(box) - right));
        };

        auto solve_for_right = [&] {
            return width_of_containing_block - left - margin_left.to_px(box) - border_left - padding_left - width.to_px(box) - padding_right - border_right - margin_right.to_px(box);
        };

        // If all three of 'left', 'width', and 'right' are 'auto':
        if (computed_left.is_auto() && width.is_auto() && computed_right.is_auto()) {
            // First set any 'auto' values for 'margin-left' and 'margin-right' to 0.
            if (margin_left.is_auto())
                margin_left = CSS::Length::make_px(0);
            if (margin_right.is_auto())
                margin_right = CSS::Length::make_px(0);
            // Then, if the 'direction' property of the element establishing the static-position containing block
            // is 'ltr' set 'left' to the static position and apply rule number three below;
            // otherwise, set 'right' to the static position and apply rule number one below.

            // NOTE: As with compute_height_for_absolutely_positioned_non_replaced_element, we actually apply these
            //       steps in the opposite order since the static position may depend on the width of the box.

            auto result = calculate_shrink_to_fit_widths(box);
            auto available_width = solve_for_width();
            CSSPixels content_width = min(max(result.preferred_minimum_width, available_width.to_px(box)), result.preferred_width);
            width = CSS::Length::make_px(content_width);
            m_state.get_mutable(box).set_content_width(content_width);

            auto static_position = m_state.get(box).static_position();

            left = static_position.x();
            right = solve_for_right();
        }

        // If none of the three is auto:
        if (!computed_left.is_auto() && !width.is_auto() && !computed_right.is_auto()) {
            // If both margin-left and margin-right are auto,
            // solve the equation under the extra constraint that the two margins get equal values
            // FIXME: unless this would make them negative, in which case when direction of the containing block is ltr (rtl), set margin-left (margin-right) to 0 and solve for margin-right (margin-left).
            auto size_available_for_margins = width_of_containing_block - border_left - padding_left - width.to_px(box) - padding_right - border_right - left - right;
            if (margin_left.is_auto() && margin_right.is_auto()) {
                margin_left = CSS::Length::make_px(size_available_for_margins / 2);
                margin_right = CSS::Length::make_px(size_available_for_margins / 2);
                return width;
            }

            // If one of margin-left or margin-right is auto, solve the equation for that value.
            if (margin_left.is_auto()) {
                margin_left = CSS::Length::make_px(size_available_for_margins);
                return width;
            }
            if (margin_right.is_auto()) {
                margin_right = CSS::Length::make_px(size_available_for_margins);
                return width;
            }
            // If the values are over-constrained, ignore the value for left
            // (in case the direction property of the containing block is rtl)
            // or right (in case direction is ltr) and solve for that value.

            // NOTE: At this point we *are* over-constrained since none of margin-left, left, width, right, or margin-right are auto.
            // FIXME: Check direction.
            right = solve_for_right();
            return width;
        }

        if (margin_left.is_auto())
            margin_left = CSS::Length::make_px(0);
        if (margin_right.is_auto())
            margin_right = CSS::Length::make_px(0);

        // 1. 'left' and 'width' are 'auto' and 'right' is not 'auto',
        //    then the width is shrink-to-fit. Then solve for 'left'
        if (computed_left.is_auto() && width.is_auto() && !computed_right.is_auto()) {
            auto result = calculate_shrink_to_fit_widths(box);
            auto available_width = solve_for_width();
            width = CSS::Length::make_px(min(max(result.preferred_minimum_width, available_width.to_px(box)), result.preferred_width));
            left = solve_for_left();
        }

        // 2. 'left' and 'right' are 'auto' and 'width' is not 'auto',
        //    then if the 'direction' property of the element establishing
        //    the static-position containing block is 'ltr' set 'left'
        //    to the static position, otherwise set 'right' to the static position.
        //    Then solve for 'left' (if 'direction is 'rtl') or 'right' (if 'direction' is 'ltr').
        else if (computed_left.is_auto() && computed_right.is_auto() && !width.is_auto()) {
            // FIXME: Check direction
            auto static_position = m_state.get(box).static_position();
            left = static_position.x();
            right = solve_for_right();
        }

        // 3. 'width' and 'right' are 'auto' and 'left' is not 'auto',
        //    then the width is shrink-to-fit. Then solve for 'right'
        else if (width.is_auto() && computed_right.is_auto() && !computed_left.is_auto()) {
            auto result = calculate_shrink_to_fit_widths(box);
            auto available_width = solve_for_width();
            width = CSS::Length::make_px(min(max(result.preferred_minimum_width, available_width.to_px(box)), result.preferred_width));
            right = solve_for_right();
        }

        // 4. 'left' is 'auto', 'width' and 'right' are not 'auto', then solve for 'left'
        else if (computed_left.is_auto() && !width.is_auto() && !computed_right.is_auto()) {
            left = solve_for_left();
        }

        // 5. 'width' is 'auto', 'left' and 'right' are not 'auto', then solve for 'width'
        else if (width.is_auto() && !computed_left.is_auto() && !computed_right.is_auto()) {
            width = solve_for_width();
        }

        // 6. 'right' is 'auto', 'left' and 'width' are not 'auto', then solve for 'right'
        else if (computed_right.is_auto() && !computed_left.is_auto() && !width.is_auto()) {
            right = solve_for_right();
        }

        return width;
    };

    // 1. The tentative used width is calculated (without 'min-width' and 'max-width')
    auto used_width = try_compute_width([&] {
        if (is<TableWrapper>(box))
            return CSS::Length::make_px(compute_table_box_width_inside_table_wrapper(box, available_space));
        if (computed_values.width().is_auto())
            return CSS::Length::make_auto();
        return CSS::Length::make_px(calculate_inner_width(box, available_space.width, computed_values.width()));
    }());

    // 2. The tentative used width is greater than 'max-width', the rules above are applied again,
    //    but this time using the computed value of 'max-width' as the computed value for 'width'.
    if (!should_treat_max_width_as_none(box, available_space.width)) {
        auto max_width = calculate_inner_width(box, available_space.width, computed_values.max_width());
        if (used_width.to_px(box) > max_width) {
            used_width = try_compute_width(CSS::Length::make_px(max_width));
        }
    }

    // 3. If the resulting width is smaller than 'min-width', the rules above are applied again,
    //    but this time using the value of 'min-width' as the computed value for 'width'.
    if (!computed_values.min_width().is_auto()) {
        auto min_width = calculate_inner_width(box, available_space.width, computed_values.min_width());
        if (used_width.to_px(box) < min_width) {
            used_width = try_compute_width(CSS::Length::make_px(min_width));
        }
    }

    box_state.set_content_width(used_width.to_px(box));
    box_state.inset_left = left;
    box_state.inset_right = right;
    box_state.margin_left = margin_left.to_px(box);
    box_state.margin_right = margin_right.to_px(box);
}

void FormattingContext::compute_width_for_absolutely_positioned_replaced_element(Box const& box, AvailableSpace const& available_space)
{
    // 10.3.8 Absolutely positioned, replaced elements
    // In this case, section 10.3.7 applies up through and including the constraint equation,
    // but the rest of section 10.3.7 is replaced by the following rules:

    // 1. The used value of 'width' is determined as for inline replaced elements.
    if (is<ReplacedBox>(box)) {
        // FIXME: This const_cast is gross.
        static_cast<ReplacedBox&>(const_cast<Box&>(box)).prepare_for_replaced_layout();
    }

    auto width = compute_width_for_replaced_element(box, available_space);
    auto width_of_containing_block = available_space.width.to_px_or_zero();
    auto available = width_of_containing_block - width;
    auto const& computed_values = box.computed_values();
    auto left = computed_values.inset().left();
    auto margin_left = computed_values.margin().left();
    auto right = computed_values.inset().right();
    auto margin_right = computed_values.margin().right();
    auto static_position = m_state.get(box).static_position();

    auto to_px = [&](const CSS::LengthPercentage& l) {
        return l.to_px(box, width_of_containing_block);
    };

    // If 'margin-left' or 'margin-right' is specified as 'auto' its used value is determined by the rules below.
    // 2. If both 'left' and 'right' have the value 'auto', then if the 'direction' property of the
    // element establishing the static-position containing block is 'ltr', set 'left' to the static
    // position; else if 'direction' is 'rtl', set 'right' to the static position.
    if (left.is_auto() && right.is_auto()) {
        left = CSS::Length::make_px(static_position.x());
    }

    // 3. If 'left' or 'right' are 'auto', replace any 'auto' on 'margin-left' or 'margin-right' with '0'.
    if (left.is_auto() || right.is_auto()) {
        if (margin_left.is_auto())
            margin_left = CSS::Length::make_px(0);
        if (margin_right.is_auto())
            margin_right = CSS::Length::make_px(0);
    }

    // 4. If at this point both 'margin-left' and 'margin-right' are still 'auto', solve the equation
    // under the extra constraint that the two margins must get equal values, unless this would make
    // them negative, in which case when the direction of the containing block is 'ltr' ('rtl'),
    // set 'margin-left' ('margin-right') to zero and solve for 'margin-right' ('margin-left').
    if (margin_left.is_auto() && margin_right.is_auto()) {
        auto remainder = available - to_px(left) - to_px(right);
        if (remainder < 0) {
            margin_left = CSS::Length::make_px(0);
            margin_right = CSS::Length::make_px(0);
        } else {
            margin_left = CSS::Length::make_px(remainder / 2);
            margin_right = CSS::Length::make_px(remainder / 2);
        }
    }

    // 5. If at this point there is an 'auto' left, solve the equation for that value.
    if (left.is_auto()) {
        left = CSS::Length::make_px(available - to_px(right) - to_px(margin_left) - to_px(margin_right));
    } else if (right.is_auto()) {
        right = CSS::Length::make_px(available - to_px(left) - to_px(margin_left) - to_px(margin_right));
    } else if (margin_left.is_auto()) {
        margin_left = CSS::Length::make_px(available - to_px(left) - to_px(right) - to_px(margin_right));
    } else if (margin_right.is_auto()) {
        margin_right = CSS::Length::make_px(available - to_px(left) - to_px(margin_left) - to_px(right));
    }

    // 6. If at this point the values are over-constrained, ignore the value for either 'left'
    // (in case the 'direction' property of the containing block is 'rtl') or 'right'
    // (in case 'direction' is 'ltr') and solve for that value.
    if (0 != available - to_px(left) - to_px(right) - to_px(margin_left) - to_px(margin_right)) {
        right = CSS::Length::make_px(available - to_px(left) - to_px(margin_left) - to_px(margin_right));
    }

    auto& box_state = m_state.get_mutable(box);
    box_state.inset_left = to_px(left);
    box_state.inset_right = to_px(right);
    box_state.margin_left = to_px(margin_left);
    box_state.margin_right = to_px(margin_right);
    box_state.set_content_width(width);
}

// https://drafts.csswg.org/css-position-3/#abs-non-replaced-height
void FormattingContext::compute_height_for_absolutely_positioned_non_replaced_element(Box const& box, AvailableSpace const& available_space, BeforeOrAfterInsideLayout before_or_after_inside_layout)
{
    // 5.3. The Height Of Absolutely Positioned, Non-Replaced Elements

    // For absolutely positioned elements, the used values of the vertical dimensions must satisfy this constraint:
    // top + margin-top + border-top-width + padding-top + height + padding-bottom + border-bottom-width + margin-bottom + bottom = height of containing block

    // NOTE: This function is called twice: both before and after inside layout.
    //       In the before pass, if it turns out we need the automatic height of the box, we abort these steps.
    //       This allows the box to retain an indefinite height from the perspective of inside layout.

    auto apply_min_max_height_constraints = [this, &box, &available_space](CSS::Length unconstrained_height) -> CSS::Length {
        auto const& computed_min_height = box.computed_values().min_height();
        auto const& computed_max_height = box.computed_values().max_height();
        auto constrained_height = unconstrained_height;
        if (!computed_max_height.is_none()) {
            auto inner_max_height = calculate_inner_height(box, available_space, computed_max_height);
            if (inner_max_height < constrained_height.to_px(box))
                constrained_height = CSS::Length::make_px(inner_max_height);
        }
        if (!computed_min_height.is_auto()) {
            auto inner_min_height = calculate_inner_height(box, available_space, computed_min_height);
            if (inner_min_height > constrained_height.to_px(box))
                constrained_height = CSS::Length::make_px(inner_min_height);
        }
        return constrained_height;
    };

    auto margin_top = box.computed_values().margin().top();
    auto margin_bottom = box.computed_values().margin().bottom();
    auto top = box.computed_values().inset().top();
    auto bottom = box.computed_values().inset().bottom();

    auto width_of_containing_block = available_space.width.to_px_or_zero();
    auto height_of_containing_block = available_space.height.to_px_or_zero();

    enum class ClampToZero {
        No,
        Yes,
    };

    auto& state = m_state.get(box);
    auto try_compute_height = [&](CSS::Length height) -> CSS::Length {
        auto solve_for = [&](CSS::Length length, ClampToZero clamp_to_zero = ClampToZero::No) {
            auto unclamped_value = height_of_containing_block
                - top.to_px(box, height_of_containing_block)
                - margin_top.to_px(box, width_of_containing_block)
                - box.computed_values().border_top().width
                - state.padding_top
                - apply_min_max_height_constraints(height).to_px(box)
                - state.padding_bottom
                - box.computed_values().border_bottom().width
                - margin_bottom.to_px(box, width_of_containing_block)
                - bottom.to_px(box, height_of_containing_block)
                + length.to_px(box);
            if (clamp_to_zero == ClampToZero::Yes)
                return CSS::Length::make_px(max(CSSPixels(0), unclamped_value));
            return CSS::Length::make_px(unclamped_value);
        };

        auto solve_for_top = [&] {
            top = solve_for(top.resolved(box, height_of_containing_block));
        };

        auto solve_for_bottom = [&] {
            bottom = solve_for(bottom.resolved(box, height_of_containing_block));
        };

        auto solve_for_height = [&] {
            height = solve_for(height, ClampToZero::Yes);
        };

        auto solve_for_margin_top = [&] {
            margin_top = solve_for(margin_top.resolved(box, width_of_containing_block));
        };

        auto solve_for_margin_bottom = [&] {
            margin_bottom = solve_for(margin_bottom.resolved(box, width_of_containing_block));
        };

        auto solve_for_margin_top_and_margin_bottom = [&] {
            auto remainder = solve_for(CSS::Length::make_px(margin_top.to_px(box, width_of_containing_block) + margin_bottom.to_px(box, width_of_containing_block))).to_px(box);
            margin_top = CSS::Length::make_px(remainder / 2);
            margin_bottom = CSS::Length::make_px(remainder / 2);
        };

        // If all three of top, height, and bottom are auto:
        if (top.is_auto() && height.is_auto() && bottom.is_auto()) {
            // First set any auto values for margin-top and margin-bottom to 0,
            if (margin_top.is_auto())
                margin_top = CSS::Length::make_px(0);
            if (margin_bottom.is_auto())
                margin_bottom = CSS::Length::make_px(0);

            // then set top to the static position,
            // and finally apply rule number three below.

            // NOTE: We actually perform these two steps in the opposite order,
            //       because the static position may depend on the height of the box (due to alignment properties).

            auto maybe_height = compute_auto_height_for_absolutely_positioned_element(box, available_space, before_or_after_inside_layout);
            if (!maybe_height.has_value())
                return height;
            height = CSS::Length::make_px(maybe_height.value());

            auto constrained_height = apply_min_max_height_constraints(height);
            m_state.get_mutable(box).set_content_height(constrained_height.to_px(box));

            auto static_position = m_state.get(box).static_position();
            top = CSS::Length::make_px(static_position.y());

            solve_for_bottom();
        }

        // If none of the three are auto:
        else if (!top.is_auto() && !height.is_auto() && !bottom.is_auto()) {
            // If both margin-top and margin-bottom are auto,
            if (margin_top.is_auto() && margin_bottom.is_auto()) {
                // solve the equation under the extra constraint that the two margins get equal values.
                solve_for_margin_top_and_margin_bottom();
            }

            // If one of margin-top or margin-bottom is auto,
            else if (margin_top.is_auto() || margin_bottom.is_auto()) {
                // solve the equation for that value.
                if (margin_top.is_auto())
                    solve_for_margin_top();
                else
                    solve_for_margin_bottom();
            }

            // If the values are over-constrained,
            else {
                // ignore the value for bottom and solve for that value.
                solve_for_bottom();
            }
        }

        // Otherwise,
        else {
            // set auto values for margin-top and margin-bottom to 0,
            if (margin_top.is_auto())
                margin_top = CSS::Length::make_px(0);
            if (margin_bottom.is_auto())
                margin_bottom = CSS::Length::make_px(0);

            // and pick one of the following six rules that apply.

            // 1. If top and height are auto and bottom is not auto,
            if (top.is_auto() && height.is_auto() && !bottom.is_auto()) {
                // then the height is based on the Auto heights for block formatting context roots,
                auto maybe_height = compute_auto_height_for_absolutely_positioned_element(box, available_space, before_or_after_inside_layout);
                if (!maybe_height.has_value())
                    return height;
                height = CSS::Length::make_px(maybe_height.value());

                // and solve for top.
                solve_for_top();
            }

            // 2. If top and bottom are auto and height is not auto,
            else if (top.is_auto() && bottom.is_auto() && !height.is_auto()) {
                // then set top to the static position,
                top = CSS::Length::make_px(m_state.get(box).static_position().y());

                // then solve for bottom.
                solve_for_bottom();
            }

            // 3. If height and bottom are auto and top is not auto,
            else if (height.is_auto() && bottom.is_auto() && !top.is_auto()) {
                // then the height is based on the Auto heights for block formatting context roots,
                auto maybe_height = compute_auto_height_for_absolutely_positioned_element(box, available_space, before_or_after_inside_layout);
                if (!maybe_height.has_value())
                    return height;
                height = CSS::Length::make_px(maybe_height.value());

                // and solve for bottom.
                solve_for_bottom();
            }

            // 4. If top is auto, height and bottom are not auto,
            else if (top.is_auto() && !height.is_auto() && !bottom.is_auto()) {
                // then solve for top.
                solve_for_top();
            }

            // 5. If height is auto, top and bottom are not auto,
            else if (height.is_auto() && !top.is_auto() && !bottom.is_auto()) {
                // then solve for height.
                solve_for_height();
            }

            // 6. If bottom is auto, top and height are not auto,
            else if (bottom.is_auto() && !top.is_auto() && !height.is_auto()) {
                // then solve for bottom.
                solve_for_bottom();
            }
        }

        return height;
    };

    // Compute the height based on box type and CSS properties:
    // https://www.w3.org/TR/css-sizing-3/#box-sizing
    auto used_height = try_compute_height([&] {
        if (is<TableWrapper>(box))
            return CSS::Length::make_px(compute_table_box_height_inside_table_wrapper(box, available_space));
        if (should_treat_height_as_auto(box, available_space))
            return CSS::Length::make_auto();
        return CSS::Length::make_px(calculate_inner_height(box, available_space, box.computed_values().height()));
    }());

    used_height = apply_min_max_height_constraints(used_height);

    // NOTE: The following is not directly part of any spec, but this is where we resolve
    //       the final used values for vertical margin/border/padding.

    auto& box_state = m_state.get_mutable(box);
    box_state.set_content_height(used_height.to_px(box));

    // do not set calculated insets or margins on the first pass, there will be a second pass
    if (box.computed_values().height().is_auto() && before_or_after_inside_layout == BeforeOrAfterInsideLayout::Before)
        return;
    box_state.set_has_definite_height(true);
    box_state.inset_top = top.to_px(box, height_of_containing_block);
    box_state.inset_bottom = bottom.to_px(box, height_of_containing_block);
    box_state.margin_top = margin_top.to_px(box, width_of_containing_block);
    box_state.margin_bottom = margin_bottom.to_px(box, width_of_containing_block);
}

CSSPixelRect FormattingContext::content_box_rect_in_static_position_ancestor_coordinate_space(Box const& box, Box const& ancestor_box) const
{
    auto rect = content_box_rect(box);
    if (&box == &ancestor_box)
        return rect;
    for (auto const* current = box.static_position_containing_block(); current; current = current->static_position_containing_block()) {
        if (current == &ancestor_box)
            return rect;
        auto const& current_state = m_state.get(*current);
        rect.translate_by(current_state.offset);
    }
    // If we get here, ancestor_box was not an ancestor of `box`!
    VERIFY_NOT_REACHED();
}

void FormattingContext::layout_absolutely_positioned_element(Box const& box, AvailableSpace const& available_space)
{
    if (box.is_svg_box()) {
        dbgln("FIXME: Implement support for absolutely positioned SVG elements.");
        return;
    }

    auto& containing_block_state = m_state.get_mutable(*box.containing_block());

    // The size of the containing block of an abspos box is always definite from the perspective of the abspos box.
    // Since abspos boxes are laid out last, we can mark the containing block as having definite sizes at this point.
    containing_block_state.set_has_definite_width(true);
    containing_block_state.set_has_definite_height(true);

    auto& box_state = m_state.get_mutable(box);

    // The border computed values are not changed by the compute_height & width calculations below.
    // The spec only adjusts and computes sizes, insets and margins.
    box_state.border_left = box.computed_values().border_left().width;
    box_state.border_right = box.computed_values().border_right().width;
    box_state.border_top = box.computed_values().border_top().width;
    box_state.border_bottom = box.computed_values().border_bottom().width;

    auto const containing_block_width = available_space.width.to_px_or_zero();
    box_state.padding_left = box.computed_values().padding().left().to_px(box, containing_block_width);
    box_state.padding_right = box.computed_values().padding().right().to_px(box, containing_block_width);
    box_state.padding_top = box.computed_values().padding().top().to_px(box, containing_block_width);
    box_state.padding_bottom = box.computed_values().padding().bottom().to_px(box, containing_block_width);

    compute_width_for_absolutely_positioned_element(box, available_space);

    // NOTE: We compute height before *and* after doing inside layout.
    //       This is done so that inside layout can resolve percentage heights.
    //       In some situations, e.g with non-auto top & bottom values, the height can be determined early.
    compute_height_for_absolutely_positioned_element(box, available_space, BeforeOrAfterInsideLayout::Before);

    // If the box width and/or height is fixed and/or or resolved from inset properties,
    // mark the size as being definite (since layout was not required to resolve it, per CSS-SIZING-3).
    auto is_length_but_not_auto = [](auto& length_percentage) {
        return length_percentage.is_length() && !length_percentage.is_auto();
    };
    if (is_length_but_not_auto(box.computed_values().inset().left())
        && is_length_but_not_auto(box.computed_values().inset().right())) {
        box_state.set_has_definite_width(true);
    }
    if (is_length_but_not_auto(box.computed_values().inset().top())
        && is_length_but_not_auto(box.computed_values().inset().bottom())) {
        box_state.set_has_definite_height(true);
    }

    // NOTE: BFC is special, as their abspos auto height depends on performing inside layout.
    //       For other formatting contexts, the height we've resolved early is good.
    //       See FormattingContext::compute_auto_height_for_absolutely_positioned_element()
    //       for the special-casing of BFC roots.
    if (!creates_block_formatting_context(box)) {
        box_state.set_has_definite_width(true);
        box_state.set_has_definite_height(true);
    }

    auto independent_formatting_context = layout_inside(box, LayoutMode::Normal, box_state.available_inner_space_or_constraints_from(available_space));

    if (box.computed_values().height().is_auto()) {
        compute_height_for_absolutely_positioned_element(box, available_space, BeforeOrAfterInsideLayout::After);
    }

    CSSPixelPoint used_offset;

    auto static_position = m_state.get(box).static_position();
    auto offset_to_static_parent = content_box_rect_in_static_position_ancestor_coordinate_space(box, *box.containing_block());
    static_position += offset_to_static_parent.location();

    if (box.computed_values().inset().top().is_auto() && box.computed_values().inset().bottom().is_auto()) {
        used_offset.set_y(static_position.y());
    } else {
        used_offset.set_y(box_state.inset_top);
        // NOTE: Absolutely positioned boxes are relative to the *padding edge* of the containing block.
        used_offset.translate_by(0, -containing_block_state.padding_top);
    }

    if (box.computed_values().inset().left().is_auto() && box.computed_values().inset().right().is_auto()) {
        used_offset.set_x(static_position.x());
    } else {
        used_offset.set_x(box_state.inset_left);
        // NOTE: Absolutely positioned boxes are relative to the *padding edge* of the containing block.
        used_offset.translate_by(-containing_block_state.padding_left, 0);
    }

    used_offset.translate_by(box_state.margin_box_left(), box_state.margin_box_top());

    box_state.set_content_offset(used_offset);

    if (independent_formatting_context)
        independent_formatting_context->parent_context_did_dimension_child_root_box();
}

void FormattingContext::compute_height_for_absolutely_positioned_replaced_element(Box const& box, AvailableSpace const& available_space, BeforeOrAfterInsideLayout before_or_after_inside_layout)
{
    // 10.6.5 Absolutely positioned, replaced elements
    // This situation is similar to 10.6.4, except that the element has an intrinsic height.

    // The used value of 'height' is determined as for inline replaced elements.
    auto height = compute_height_for_replaced_element(box, available_space);

    auto height_of_containing_block = available_space.height.to_px_or_zero();
    auto available = height_of_containing_block - height;
    auto const& computed_values = box.computed_values();
    auto top = computed_values.inset().top();
    auto margin_top = computed_values.margin().top();
    auto bottom = computed_values.inset().bottom();
    auto margin_bottom = computed_values.margin().bottom();
    auto static_position = m_state.get(box).static_position();

    auto to_px = [&](const CSS::LengthPercentage& l) {
        return l.to_px(box, height_of_containing_block);
    };

    // If 'margin-top' or 'margin-bottom' is specified as 'auto' its used value is determined by the rules below.
    // 2. If both 'top' and 'bottom' have the value 'auto', replace 'top' with the element's static position.
    if (top.is_auto() && bottom.is_auto()) {
        top = CSS::Length::make_px(static_position.x());
    }

    // 3. If 'bottom' is 'auto', replace any 'auto' on 'margin-top' or 'margin-bottom' with '0'.
    if (bottom.is_auto()) {
        if (margin_top.is_auto())
            margin_top = CSS::Length::make_px(0);
        if (margin_bottom.is_auto())
            margin_bottom = CSS::Length::make_px(0);
    }

    // 4. If at this point both 'margin-top' and 'margin-bottom' are still 'auto',
    // solve the equation under the extra constraint that the two margins must get equal values.
    if (margin_top.is_auto() && margin_bottom.is_auto()) {
        auto remainder = available - to_px(top) - to_px(bottom);
        margin_top = CSS::Length::make_px(remainder / 2);
        margin_bottom = CSS::Length::make_px(remainder / 2);
    }

    // 5. If at this point there is an 'auto' left, solve the equation for that value.
    if (top.is_auto()) {
        top = CSS::Length::make_px(available - to_px(bottom) - to_px(margin_top) - to_px(margin_bottom));
    } else if (bottom.is_auto()) {
        bottom = CSS::Length::make_px(available - to_px(top) - to_px(margin_top) - to_px(margin_bottom));
    } else if (margin_top.is_auto()) {
        margin_top = CSS::Length::make_px(available - to_px(top) - to_px(bottom) - to_px(margin_bottom));
    } else if (margin_bottom.is_auto()) {
        margin_bottom = CSS::Length::make_px(available - to_px(top) - to_px(margin_top) - to_px(bottom));
    }

    // 6. If at this point the values are over-constrained, ignore the value for 'bottom' and solve for that value.
    if (0 != available - to_px(top) - to_px(bottom) - to_px(margin_top) - to_px(margin_bottom)) {
        bottom = CSS::Length::make_px(available - to_px(top) - to_px(margin_top) - to_px(margin_bottom));
    }

    auto& box_state = m_state.get_mutable(box);
    box_state.set_content_height(height);

    // do not set calculated insets or margins on the first pass, there will be a second pass
    if (box.computed_values().height().is_auto() && before_or_after_inside_layout == BeforeOrAfterInsideLayout::Before)
        return;
    box_state.set_has_definite_height(true);
    box_state.inset_top = to_px(top);
    box_state.inset_bottom = to_px(bottom);
    box_state.margin_top = to_px(margin_top);
    box_state.margin_bottom = to_px(margin_bottom);
}

// https://www.w3.org/TR/css-position-3/#relpos-insets
void FormattingContext::compute_inset(NodeWithStyleAndBoxModelMetrics const& box, CSSPixelSize containing_block_size)
{
    if (box.computed_values().position() != CSS::Positioning::Relative)
        return;

    auto resolve_two_opposing_insets = [&](CSS::LengthPercentage const& computed_first, CSS::LengthPercentage const& computed_second, CSSPixels& used_start, CSSPixels& used_end, CSSPixels reference_for_percentage) {
        auto resolved_first = computed_first.to_px(box, reference_for_percentage);
        auto resolved_second = computed_second.to_px(box, reference_for_percentage);

        if (computed_first.is_auto() && computed_second.is_auto()) {
            // If opposing inset properties in an axis both compute to auto (their initial values),
            // their used values are zero (i.e., the boxes stay in their original position in that axis).
            used_start = 0;
            used_end = 0;
        } else if (computed_first.is_auto() || computed_second.is_auto()) {
            // If only one is auto, its used value becomes the negation of the other, and the box is shifted by the specified amount.
            if (computed_first.is_auto()) {
                used_end = resolved_second;
                used_start = -used_end;
            } else {
                used_start = resolved_first;
                used_end = -used_start;
            }
        } else {
            // If neither is auto, the position is over-constrained; (with respect to the writing mode of its containing block)
            // the computed end side value is ignored, and its used value becomes the negation of the start side.
            used_start = resolved_first;
            used_end = -used_start;
        }
    };

    auto& box_state = m_state.get_mutable(box);
    auto const& computed_values = box.computed_values();

    // FIXME: Respect the containing block's writing-mode.
    resolve_two_opposing_insets(computed_values.inset().left(), computed_values.inset().right(), box_state.inset_left, box_state.inset_right, containing_block_size.width());
    resolve_two_opposing_insets(computed_values.inset().top(), computed_values.inset().bottom(), box_state.inset_top, box_state.inset_bottom, containing_block_size.height());
}

// https://drafts.csswg.org/css-sizing-3/#fit-content-size
CSSPixels FormattingContext::calculate_fit_content_width(Layout::Box const& box, AvailableSpace const& available_space) const
{
    // If the available space in a given axis is definite,
    // equal to clamp(min-content size, stretch-fit size, max-content size)
    // (i.e. max(min-content size, min(max-content size, stretch-fit size))).
    if (available_space.width.is_definite()) {
        return max(calculate_min_content_width(box),
            min(calculate_stretch_fit_width(box, available_space.width),
                calculate_max_content_width(box)));
    }

    // When sizing under a min-content constraint, equal to the min-content size.
    if (available_space.width.is_min_content())
        return calculate_min_content_width(box);

    // Otherwise, equal to the max-content size in that axis.
    return calculate_max_content_width(box);
}

// https://drafts.csswg.org/css-sizing-3/#fit-content-size
CSSPixels FormattingContext::calculate_fit_content_height(Layout::Box const& box, AvailableSpace const& available_space) const
{
    // If the available space in a given axis is definite,
    // equal to clamp(min-content size, stretch-fit size, max-content size)
    // (i.e. max(min-content size, min(max-content size, stretch-fit size))).
    if (available_space.height.is_definite()) {
        return max(calculate_min_content_height(box, available_space.width.to_px_or_zero()),
            min(calculate_stretch_fit_height(box, available_space.height),
                calculate_max_content_height(box, available_space.width.to_px_or_zero())));
    }

    // When sizing under a min-content constraint, equal to the min-content size.
    if (available_space.height.is_min_content())
        return calculate_min_content_height(box, available_space.width.to_px_or_zero());

    // Otherwise, equal to the max-content size in that axis.
    return calculate_max_content_height(box, available_space.width.to_px_or_zero());
}

CSSPixels FormattingContext::calculate_min_content_width(Layout::Box const& box) const
{
    if (box.is_replaced_box() && box.computed_values().width().is_percentage()) {
        return 0;
    }

    if (box.has_natural_width())
        return *box.natural_width();

    auto& cache = box.cached_intrinsic_sizes().min_content_width;
    if (cache.has_value())
        return cache.value();

    LayoutState throwaway_state;

    auto& box_state = throwaway_state.get_mutable(box);
    box_state.width_constraint = SizeConstraint::MinContent;
    box_state.set_indefinite_content_width();

    auto context = const_cast<FormattingContext*>(this)->create_independent_formatting_context_if_needed(throwaway_state, LayoutMode::IntrinsicSizing, box);
    if (!context) {
        context = make<BlockFormattingContext>(throwaway_state, LayoutMode::IntrinsicSizing, as<BlockContainer>(box), nullptr);
    }

    auto available_width = AvailableSize::make_min_content();
    auto available_height = box_state.has_definite_height()
        ? AvailableSize::make_definite(box_state.content_height())
        : AvailableSize::make_indefinite();

    context->run(AvailableSpace(available_width, available_height));

    auto min_content_width = clamp_to_max_dimension_value(context->automatic_content_width());
    cache.emplace(min_content_width);
    return min_content_width;
}

CSSPixels FormattingContext::calculate_max_content_width(Layout::Box const& box) const
{
    if (box.has_natural_width())
        return *box.natural_width();

    auto& cache = box.cached_intrinsic_sizes().max_content_width;
    if (cache.has_value())
        return cache.value();

    LayoutState throwaway_state;

    auto& box_state = throwaway_state.get_mutable(box);
    box_state.width_constraint = SizeConstraint::MaxContent;
    box_state.set_indefinite_content_width();

    auto context = const_cast<FormattingContext*>(this)->create_independent_formatting_context_if_needed(throwaway_state, LayoutMode::IntrinsicSizing, box);
    if (!context) {
        context = make<BlockFormattingContext>(throwaway_state, LayoutMode::IntrinsicSizing, as<BlockContainer>(box), nullptr);
    }

    auto available_width = AvailableSize::make_max_content();
    auto available_height = box_state.has_definite_height()
        ? AvailableSize::make_definite(box_state.content_height())
        : AvailableSize::make_indefinite();

    context->run(AvailableSpace(available_width, available_height));

    auto max_content_width = clamp_to_max_dimension_value(context->automatic_content_width());
    cache.emplace(max_content_width);
    return max_content_width;
}

// https://www.w3.org/TR/css-sizing-3/#min-content-block-size
CSSPixels FormattingContext::calculate_min_content_height(Layout::Box const& box, CSSPixels width) const
{
    // For block containers, tables, and inline boxes, this is equivalent to the max-content block size.
    if (box.is_block_container() || box.display().is_table_inside())
        return calculate_max_content_height(box, width);

    if (box.has_natural_height())
        return *box.natural_height();

    auto& cache = box.cached_intrinsic_sizes().min_content_height.ensure(width);
    if (cache.has_value())
        return cache.value();

    LayoutState throwaway_state;

    auto& box_state = throwaway_state.get_mutable(box);
    box_state.height_constraint = SizeConstraint::MinContent;
    box_state.set_indefinite_content_height();
    box_state.set_content_width(width);

    auto context = const_cast<FormattingContext*>(this)->create_independent_formatting_context_if_needed(throwaway_state, LayoutMode::IntrinsicSizing, box);
    if (!context) {
        context = make<BlockFormattingContext>(throwaway_state, LayoutMode::IntrinsicSizing, as<BlockContainer>(box), nullptr);
    }

    context->run(AvailableSpace(AvailableSize::make_definite(width), AvailableSize::make_min_content()));

    auto min_content_height = clamp_to_max_dimension_value(context->automatic_content_height());
    cache.emplace(min_content_height);
    return min_content_height;
}

CSSPixels FormattingContext::calculate_max_content_height(Layout::Box const& box, CSSPixels width) const
{
    if (box.has_preferred_aspect_ratio())
        return width / *box.preferred_aspect_ratio();

    if (box.has_natural_height())
        return *box.natural_height();

    auto& cache_slot = box.cached_intrinsic_sizes().max_content_height.ensure(width);
    if (cache_slot.has_value())
        return cache_slot.value();

    LayoutState throwaway_state;

    auto& box_state = throwaway_state.get_mutable(box);
    box_state.height_constraint = SizeConstraint::MaxContent;
    box_state.set_indefinite_content_height();
    box_state.set_content_width(width);

    auto context = const_cast<FormattingContext*>(this)->create_independent_formatting_context_if_needed(throwaway_state, LayoutMode::IntrinsicSizing, box);
    if (!context) {
        context = make<BlockFormattingContext>(throwaway_state, LayoutMode::IntrinsicSizing, as<BlockContainer>(box), nullptr);
    }

    context->run(AvailableSpace(AvailableSize::make_definite(width), AvailableSize::make_max_content()));

    auto max_content_height = clamp_to_max_dimension_value(context->automatic_content_height());
    cache_slot.emplace(max_content_height);
    return max_content_height;
}

CSSPixels FormattingContext::calculate_inner_width(Layout::Box const& box, AvailableSize const& available_width, CSS::Size const& width) const
{
    VERIFY(!width.is_auto());

    auto width_of_containing_block = available_width.to_px_or_zero();
    if (width.is_fit_content()) {
        return calculate_fit_content_width(box, AvailableSpace { available_width, AvailableSize::make_indefinite() });
    }
    if (width.is_max_content()) {
        return calculate_max_content_width(box);
    }
    if (width.is_min_content()) {
        return calculate_min_content_width(box);
    }

    auto& computed_values = box.computed_values();
    if (computed_values.box_sizing() == CSS::BoxSizing::BorderBox) {
        auto const& state = m_state.get(box);
        auto inner_width = width.to_px(box, width_of_containing_block)
            - computed_values.border_left().width
            - state.padding_left
            - computed_values.border_right().width
            - state.padding_right;
        return max(inner_width, 0);
    }

    return width.to_px(box, width_of_containing_block);
}

CSSPixels FormattingContext::calculate_inner_height(Box const& box, AvailableSpace const& available_space, CSS::Size const& height) const
{
    if (height.is_auto() && box.has_preferred_aspect_ratio()) {
        return m_state.get(box).content_width() / *box.preferred_aspect_ratio();
    }

    VERIFY(!height.is_auto());

    if (height.is_fit_content()) {
        return calculate_fit_content_height(box, available_space);
    }
    if (height.is_max_content()) {
        return calculate_max_content_height(box, available_space.width.to_px_or_zero());
    }
    if (height.is_min_content()) {
        return calculate_min_content_height(box, available_space.width.to_px_or_zero());
    }

    auto height_of_containing_block = available_space.height.to_px_or_zero();
    auto& computed_values = box.computed_values();

    if (computed_values.box_sizing() == CSS::BoxSizing::BorderBox) {
        auto const& state = m_state.get(box);
        auto inner_height = height.to_px(box, height_of_containing_block)
            - computed_values.border_top().width
            - state.padding_top
            - computed_values.border_bottom().width
            - state.padding_bottom;
        return max(inner_height, 0);
    }

    return height.to_px(box, height_of_containing_block);
}

CSSPixels FormattingContext::containing_block_width_for(NodeWithStyleAndBoxModelMetrics const& node) const
{
    auto const& used_values = m_state.get(node);
    switch (used_values.width_constraint) {
    case SizeConstraint::MinContent:
        return 0;
    case SizeConstraint::MaxContent:
        return CSSPixels::max();
    case SizeConstraint::None:
        return used_values.containing_block_used_values()->content_width();
    }
    VERIFY_NOT_REACHED();
}

// https://drafts.csswg.org/css-sizing-3/#stretch-fit-size
CSSPixels FormattingContext::calculate_stretch_fit_width(Box const& box, AvailableSize const& available_width) const
{
    // The size a box would take if its outer size filled the available space in the given axis;
    // in other words, the stretch fit into the available space, if that is definite.

    // Undefined if the available space is indefinite.
    if (!available_width.is_definite())
        return 0;

    auto const& box_state = m_state.get(box);
    return available_width.to_px_or_zero()
        - box_state.margin_left
        - box_state.margin_right
        - box_state.padding_left
        - box_state.padding_right
        - box_state.border_left
        - box_state.border_right;
}

// https://drafts.csswg.org/css-sizing-3/#stretch-fit-size
CSSPixels FormattingContext::calculate_stretch_fit_height(Box const& box, AvailableSize const& available_height) const
{
    // The size a box would take if its outer size filled the available space in the given axis;
    // in other words, the stretch fit into the available space, if that is definite.
    // Undefined if the available space is indefinite.
    auto const& box_state = m_state.get(box);
    return available_height.to_px_or_zero()
        - box_state.margin_top
        - box_state.margin_bottom
        - box_state.padding_top
        - box_state.padding_bottom
        - box_state.border_top
        - box_state.border_bottom;
}

bool FormattingContext::should_treat_width_as_auto(Box const& box, AvailableSpace const& available_space) const
{
    auto const& computed_width = box.computed_values().width();
    if (computed_width.is_auto())
        return true;
    if (computed_width.contains_percentage()) {
        if (available_space.width.is_max_content())
            return true;
        if (available_space.width.is_indefinite())
            return true;
    }
    // AD-HOC: If the box has a preferred aspect ratio and an intrinsic keyword for width...
    if (box.has_preferred_aspect_ratio()
        && (computed_width.is_min_content() || computed_width.is_max_content() || computed_width.is_fit_content())) {
        // If the box has no natural height to resolve the aspect ratio, we treat the width as auto.
        if (!box.has_natural_height())
            return true;
        // If the box has definite height, we can resolve the width through the aspect ratio.
        if (m_state.get(box).has_definite_height())
            return true;
    }
    return false;
}

bool FormattingContext::should_treat_height_as_auto(Box const& box, AvailableSpace const& available_space) const
{
    auto computed_height = box.computed_values().height();
    if (computed_height.is_auto()) {
        auto const& box_state = m_state.get(box);
        if (box_state.has_definite_width() && box.has_preferred_aspect_ratio())
            return false;
        return true;
    }

    if (computed_height.contains_percentage()) {
        if (available_space.height.is_max_content())
            return true;
        if (available_space.height.is_indefinite())
            return true;
    }

    // AD-HOC: If the box has a preferred aspect ratio and an intrinsic keyword for height...
    if (box.has_preferred_aspect_ratio()
        && (computed_height.is_min_content() || computed_height.is_max_content() || computed_height.is_fit_content())) {
        // If the box has no natural width to resolve the aspect ratio, we treat the height as auto.
        if (!box.has_natural_width())
            return true;
        // If the box has definite width, we can resolve the height through the aspect ratio.
        if (m_state.get(box).has_definite_width())
            return true;
    }
    return false;
}

bool FormattingContext::can_skip_is_anonymous_text_run(Box& box)
{
    if (box.is_anonymous() && !box.is_generated() && !box.first_child_of_type<BlockContainer>()) {
        bool contains_only_white_space = true;
        box.for_each_in_subtree([&](auto const& node) {
            if (!is<TextNode>(node) || !static_cast<TextNode const&>(node).dom_node().data().bytes_as_string_view().is_whitespace()) {
                contains_only_white_space = false;
                return TraversalDecision::Break;
            }
            return TraversalDecision::Continue;
        });
        if (contains_only_white_space)
            return true;
    }
    return false;
}

CSSPixelRect FormattingContext::absolute_content_rect(Box const& box) const
{
    auto const& box_state = m_state.get(box);
    CSSPixelRect rect { box_state.offset, { box_state.content_width(), box_state.content_height() } };
    for (auto* block = box_state.containing_block_used_values(); block; block = block->containing_block_used_values())
        rect.translate_by(block->offset);
    return rect;
}

Box const* FormattingContext::box_child_to_derive_baseline_from(Box const& box) const
{
    if (!box.has_children() || box.children_are_inline())
        return nullptr;
    // To find the baseline of a box, we first look for the last in-flow child with at least one line box.
    auto const* last_box_child = box.last_child_of_type<Box>();
    for (Node const* child = last_box_child; child; child = child->previous_sibling()) {
        if (!child->is_box())
            continue;
        auto& child_box = static_cast<Box const&>(*child);
        if (!child_box.is_out_of_flow(*this) && !m_state.get(child_box).line_boxes.is_empty()) {
            return &child_box;
        }
        auto box_child_to_derive_baseline_from_candidate = box_child_to_derive_baseline_from(child_box);
        if (box_child_to_derive_baseline_from_candidate)
            return box_child_to_derive_baseline_from_candidate;
    }
    // None of the children has a line box.
    return nullptr;
}

CSSPixels FormattingContext::box_baseline(Box const& box) const
{
    auto const& box_state = m_state.get(box);

    // https://www.w3.org/TR/CSS2/visudet.html#propdef-vertical-align
    auto const& vertical_align = box.computed_values().vertical_align();
    if (vertical_align.has<CSS::VerticalAlign>()) {
        switch (vertical_align.get<CSS::VerticalAlign>()) {
        case CSS::VerticalAlign::Top:
            // Top: Align the top of the aligned subtree with the top of the line box.
            return box_state.border_box_top();
        case CSS::VerticalAlign::Middle:
            // Align the vertical midpoint of the box with the baseline of the parent box plus half the x-height of the parent.
            return box_state.content_height() / 2 + CSSPixels::nearest_value_for(box.containing_block()->first_available_font().pixel_metrics().x_height / 2);
        case CSS::VerticalAlign::Bottom:
            // Bottom: Align the bottom of the aligned subtree with the bottom of the line box.
            return box_state.content_height() + box_state.margin_box_top();
        case CSS::VerticalAlign::TextTop:
            // TextTop: Align the top of the box with the top of the parent's content area (see 10.6.1).
            return box.computed_values().font_size();
        case CSS::VerticalAlign::TextBottom:
            // TextTop: Align the bottom of the box with the bottom of the parent's content area (see 10.6.1).
            return box_state.content_height() - CSSPixels::nearest_value_for(box.containing_block()->first_available_font().pixel_metrics().descent * 2);
        default:
            break;
        }
    }

    if (!box_state.line_boxes.is_empty())
        return box_state.margin_box_top() + box_state.offset.y() + box_state.line_boxes.last().baseline();
    if (auto const* child_box = box_child_to_derive_baseline_from(box)) {
        return box_state.margin_box_top() + box_state.offset.y() + box_baseline(*child_box);
    }
    // If none of the children have a baseline set, the bottom margin edge of the box is used.
    return box_state.margin_box_height();
}

CSSPixelRect FormattingContext::margin_box_rect(LayoutState::UsedValues const& used_values) const
{
    return {
        used_values.offset.translated(-used_values.margin_box_left(), -used_values.margin_box_top()),
        {
            used_values.margin_box_left() + used_values.content_width() + used_values.margin_box_right(),
            used_values.margin_box_top() + used_values.content_height() + used_values.margin_box_bottom(),
        },
    };
}

CSSPixelRect FormattingContext::content_box_rect(Box const& box) const
{
    return content_box_rect(m_state.get(box));
}

CSSPixelRect FormattingContext::content_box_rect(LayoutState::UsedValues const& used_values) const
{
    return CSSPixelRect { used_values.offset, { used_values.content_width(), used_values.content_height() } };
}

CSSPixelRect FormattingContext::content_box_rect_in_ancestor_coordinate_space(LayoutState::UsedValues const& used_values, Box const& ancestor_box) const
{
    auto rect = content_box_rect(used_values);
    if (&used_values.node() == &ancestor_box)
        return rect;
    for (auto const* current = used_values.containing_block_used_values(); current; current = current->containing_block_used_values()) {
        if (&current->node() == &ancestor_box)
            return rect;
        rect.translate_by(current->offset);
    }
    // If we get here, ancestor_box was not a containing block ancestor of `box`!
    VERIFY_NOT_REACHED();
}

CSSPixelRect FormattingContext::margin_box_rect_in_ancestor_coordinate_space(LayoutState::UsedValues const& used_values, Box const& ancestor_box) const
{
    auto rect = margin_box_rect(used_values);
    if (&used_values.node() == &ancestor_box)
        return rect;
    for (auto const* current = used_values.containing_block_used_values(); current; current = current->containing_block_used_values()) {
        if (&current->node() == &ancestor_box)
            return rect;
        rect.translate_by(current->offset);
    }
    // If we get here, ancestor_box was not a containing block ancestor of `box`!
    VERIFY_NOT_REACHED();
}

CSSPixelRect FormattingContext::margin_box_rect_in_ancestor_coordinate_space(Box const& box, Box const& ancestor_box) const
{
    return margin_box_rect_in_ancestor_coordinate_space(m_state.get(box), ancestor_box);
}

bool box_is_sized_as_replaced_element(Box const& box)
{
    // When a box has a preferred aspect ratio, its automatic sizes are calculated the same as for a
    // replaced element with a natural aspect ratio and no natural size in that axis, see e.g. CSS2 §10
    // and CSS Flexible Box Model Level 1 §9.2.
    // https://www.w3.org/TR/css-sizing-4/#aspect-ratio-automatic
    if (is<ReplacedBox>(box))
        return true;

    if (box.has_preferred_aspect_ratio()) {
        // From CSS2:
        // If height and width both have computed values of auto and the element has an intrinsic ratio but no intrinsic height or width,
        // then the used value of width is undefined in CSS 2.
        // However, it is suggested that, if the containing block’s width does not itself depend on the replaced element’s width,
        // then the used value of width is calculated from the constraint equation used for block-level, non-replaced elements in normal flow.

        // AD-HOC: If box has preferred aspect ratio but width and height are not specified, then we should
        //         size it as a normal box to match other browsers.
        if (box.computed_values().height().is_auto()
            && box.computed_values().width().is_auto()
            && !box.has_natural_width()
            && !box.has_natural_height()) {
            return false;
        }
        return true;
    }

    return false;
}

bool FormattingContext::should_treat_max_width_as_none(Box const& box, AvailableSize const& available_width) const
{
    auto const& max_width = box.computed_values().max_width();
    if (max_width.is_none())
        return true;
    if (available_width.is_max_content() && max_width.is_max_content())
        return true;
    if (box.is_absolutely_positioned())
        return false;
    if (max_width.contains_percentage()) {
        if (available_width.is_max_content())
            return true;
        if (available_width.is_min_content())
            return false;
        if (!m_state.get(*box.non_anonymous_containing_block()).has_definite_width())
            return true;
    }
    if (max_width.is_fit_content() && available_width.is_intrinsic_sizing_constraint())
        return true;
    if (max_width.is_max_content() && available_width.is_max_content())
        return true;
    if (max_width.is_min_content() && available_width.is_min_content())
        return true;
    return false;
}

bool FormattingContext::should_treat_max_height_as_none(Box const& box, AvailableSize const& available_height) const
{
    // https://www.w3.org/TR/CSS22/visudet.html#min-max-heights
    // If the height of the containing block is not specified explicitly (i.e., it depends on content height),
    // and this element is not absolutely positioned, the percentage value is treated as '0' (for 'min-height')
    // or 'none' (for 'max-height').
    auto const& max_height = box.computed_values().max_height();
    if (max_height.is_none())
        return true;
    if (box.is_absolutely_positioned())
        return false;
    if (max_height.contains_percentage()) {
        if (available_height.is_min_content())
            return false;
        if (!m_state.get(*box.non_anonymous_containing_block()).has_definite_height())
            return true;
    }
    if (max_height.is_fit_content() && available_height.is_intrinsic_sizing_constraint())
        return true;
    if (max_height.is_max_content() && available_height.is_max_content())
        return true;
    if (max_height.is_min_content() && available_height.is_min_content())
        return true;
    return false;
}

}
