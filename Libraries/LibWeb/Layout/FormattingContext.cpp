/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/StyleValues/AnchorStyleValue.h>
#include <LibWeb/CSS/StyleValues/CalcNodeRef.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/Dump.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/Layout/BlockFormattingContext.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/FlexFormattingContext.h>
#include <LibWeb/Layout/FormattingContext.h>
#include <LibWeb/Layout/GridFormattingContext.h>
#include <LibWeb/Layout/InlineNode.h>
#include <LibWeb/Layout/ListItemMarkerBox.h>
#include <LibWeb/Layout/ReplacedBox.h>
#include <LibWeb/Layout/ReplacedWithChildrenFormattingContext.h>
#include <LibWeb/Layout/SVGFormattingContext.h>
#include <LibWeb/Layout/SVGSVGBox.h>
#include <LibWeb/Layout/TableFormattingContext.h>
#include <LibWeb/Layout/TableWrapper.h>
#include <LibWeb/Layout/TextInputBox.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Layout/Viewport.h>

namespace Web::Layout {

enum class SizeDimension {
    Inline,
    Block,
};

// NB: Intrinsic grid sizing can transfer an aspect ratio before block-axis border metrics are copied into the layout
//     state. Border widths are already definite at computed-value time, while padding remains resolved in the state.
static CSSPixels content_block_size_from_aspect_ratio(Box const& box, LayoutState::UsedValues const& box_state, CSSPixels content_inline_size)
{
    VERIFY(box.has_preferred_aspect_ratio());

    auto aspect_ratio = *box.preferred_aspect_ratio();
    if (aspect_ratio == 0)
        return 0;

    if (box.computed_values().box_sizing_for_aspect_ratio() == CSS::BoxSizing::BorderBox) {
        auto const& computed_values = box.computed_values();
        auto border_box_left = computed_values.border_left().width + box_state.padding_left;
        auto border_box_right = computed_values.border_right().width + box_state.padding_right;
        auto border_box_top = computed_values.border_top().width + box_state.padding_top;
        auto border_box_bottom = computed_values.border_bottom().width + box_state.padding_bottom;
        auto border_box_inline_size = content_inline_size + border_box_left + border_box_right;
        auto border_box_block_size = border_box_inline_size / aspect_ratio;
        return max(border_box_block_size - border_box_top - border_box_bottom, 0);
    }

    return content_inline_size / aspect_ratio;
}

static CSSPixels content_block_size_from_aspect_ratio(Box const& box, LayoutState::UsedValues const& box_state)
{
    return content_block_size_from_aspect_ratio(box, box_state, box_state.content_inline_size());
}

static CSSPixels content_inline_size_from_aspect_ratio(Box const& box, LayoutState::UsedValues const& box_state, CSSPixels content_block_size)
{
    VERIFY(box.has_preferred_aspect_ratio());

    auto aspect_ratio = *box.preferred_aspect_ratio();
    if (aspect_ratio == 0)
        return 0;

    if (box.computed_values().box_sizing_for_aspect_ratio() == CSS::BoxSizing::BorderBox) {
        auto const& computed_values = box.computed_values();
        auto border_box_left = computed_values.border_left().width + box_state.padding_left;
        auto border_box_right = computed_values.border_right().width + box_state.padding_right;
        auto border_box_top = computed_values.border_top().width + box_state.padding_top;
        auto border_box_bottom = computed_values.border_bottom().width + box_state.padding_bottom;
        auto border_box_block_size = content_block_size + border_box_top + border_box_bottom;
        auto border_box_inline_size = border_box_block_size * aspect_ratio;
        return max(border_box_inline_size - border_box_left - border_box_right, 0);
    }

    return content_block_size * aspect_ratio;
}

struct ReplacedMaxContentSizeConstraints {
    Optional<CSSPixels> definite_size_in_ratio_determining_axis;
    Optional<CSSPixels> minimum_inline_size;
    Optional<CSSPixels> minimum_block_size;
};

// https://drafts.csswg.org/css-sizing-3/#intrinsic-sizes
static Optional<CSSPixels> max_content_size_for_replaced_element_without_natural_size(Box const& box, CSS::SizeWithAspectRatio const& natural_size, LayoutState::UsedValues const& box_state, SizeDimension dimension, ReplacedMaxContentSizeConstraints const& constraints = {})
{
    // the intrinsic sizes of replaced elements without natural sizes are defined below:
    auto is_inline_axis = dimension == SizeDimension::Inline;
    if (!box.is_replaced_box() || (is_inline_axis ? natural_size.has_width() : natural_size.has_height()))
        return {};

    // SVG Integration says that a non-top-level <svg> starts with auto width/height, and that with a viewBox, missing
    // width/height attributes "keep" their auto value. The resulting width, height, and aspect ratio are then
    // "used in CSS sizing as intrinsic element size properties".
    //
    // CSS Sizing defines max-content as the size the box would have "if it was a float" with an auto preferred size.
    // CSS2 replaced sizing then resolves auto width from "(used height) * (intrinsic ratio)", and auto height from
    // "(used width) / (intrinsic ratio)". Keep this SVG specific bridge before falling through to CSS Sizing's fallback
    // for replaced elements without natural sizes.
    //  - https://svgwg.org/specs/integration/#svg-css-sizing
    //  - https://drafts.csswg.org/css-sizing-3/#intrinsic-sizes
    //  - https://drafts.csswg.org/css2/#inline-replaced-width
    //  - https://drafts.csswg.org/css2/#inline-replaced-height
    if (box.is_svg_svg_box() && natural_size.has_aspect_ratio()) {
        if (is_inline_axis && natural_size.has_height())
            return natural_size.height.value() * natural_size.aspect_ratio.value();
        if (!is_inline_axis && natural_size.has_width())
            return natural_size.width.value() / natural_size.aspect_ratio.value();
    }

    // For the max-content size:
    // If it has a preferred aspect ratio:
    if (box.has_preferred_aspect_ratio()) {
        // If the available space is definite in the inline axis, use the stretch fit into that size for the inline size
        // and calculate the block size using the aspect ratio.
        //
        // NB: This helper is only for the max-content size, which has no definite available inline size. Callers may
        //     still know a definite used size in the opposite axis when the box lacks a natural size in that axis.
        if (constraints.definite_size_in_ratio_determining_axis.has_value())
            return is_inline_axis
                ? content_inline_size_from_aspect_ratio(box, box_state, constraints.definite_size_in_ratio_determining_axis.value())
                : content_block_size_from_aspect_ratio(box, box_state, constraints.definite_size_in_ratio_determining_axis.value());

        // Otherwise if the box has a <length> as its computed value for min-width or min-height, use that size and
        // calculate the other dimension using the aspect ratio; if both dimensions have a <length> minimum, choose the
        // one that results in the larger overall size.
        //
        // NOTE: This case was previous calculated from a 300x150 default size, rather than the box’s min size. This is
        //       believed to be a better behavior, and likely to be Web-compatible, but please send feedback to the CSSWG
        //       if there are any problems.
        Optional<CSSPixels> size_from_min_inline_size;
        if (constraints.minimum_inline_size.has_value()) {
            auto inline_size = constraints.minimum_inline_size.value();
            size_from_min_inline_size = is_inline_axis ? inline_size : content_block_size_from_aspect_ratio(box, box_state, inline_size);
        } else {
            auto const& min_inline_size = box.computed_values().min_width();
            if (min_inline_size.is_length_percentage() && !min_inline_size.contains_percentage()) {
                auto inline_size = min_inline_size.to_px(0);
                size_from_min_inline_size = is_inline_axis ? inline_size : content_block_size_from_aspect_ratio(box, box_state, inline_size);
            }
        }

        Optional<CSSPixels> size_from_min_block_size;
        if (constraints.minimum_block_size.has_value()) {
            auto block_size = constraints.minimum_block_size.value();
            size_from_min_block_size = is_inline_axis ? content_inline_size_from_aspect_ratio(box, box_state, block_size) : block_size;
        } else {
            auto const& min_block_size = box.computed_values().min_height();
            if (min_block_size.is_length_percentage() && !min_block_size.contains_percentage()) {
                auto block_size = min_block_size.to_px(0);
                size_from_min_block_size = is_inline_axis ? content_inline_size_from_aspect_ratio(box, box_state, block_size) : block_size;
            }
        }

        if (size_from_min_inline_size.has_value() && size_from_min_block_size.has_value())
            return max(size_from_min_inline_size.value(), size_from_min_block_size.value());
        if (size_from_min_inline_size.has_value())
            return size_from_min_inline_size.value();
        if (size_from_min_block_size.has_value())
            return size_from_min_block_size.value();

        // Otherwise use an inline size matching the corresponding dimension of the initial containing block and calculate
        // the other dimension using the aspect ratio.
        //
        // NOTE: This author-controllable behavior is made possible by the new auto value for the min size properties.
        //       This is believed to be a better behavior, but it is not yet clear if it is Web-compatible, so please
        //       send feedback to the CSSWG if there are any problems.
        auto initial_containing_block_inline_size = box.document().viewport_rect().width();
        return is_inline_axis ? initial_containing_block_inline_size : content_block_size_from_aspect_ratio(box, box_state, initial_containing_block_inline_size);
    }

    // If it has no preferred aspect ratio:
    // For both the min-content size and max-content size:
    // If the box has a <length> as its computed minimum size (min-width/min-height) in that dimension, use that size.
    auto const& min_size = is_inline_axis ? box.computed_values().min_width() : box.computed_values().min_height();
    if (min_size.is_length_percentage() && !min_size.contains_percentage())
        return min_size.to_px(0);

    // Otherwise, use 300px for the width and/or 150px for the height as needed.
    return is_inline_axis ? CSSPixels(300) : CSSPixels(150);
}

static bool is_text_control_input(HTML::HTMLInputElement const& input_element)
{
    switch (input_element.type_state()) {
    case HTML::HTMLInputElement::TypeAttributeState::Text:
    case HTML::HTMLInputElement::TypeAttributeState::Search:
    case HTML::HTMLInputElement::TypeAttributeState::URL:
    case HTML::HTMLInputElement::TypeAttributeState::Telephone:
    case HTML::HTMLInputElement::TypeAttributeState::Email:
    case HTML::HTMLInputElement::TypeAttributeState::Password:
    case HTML::HTMLInputElement::TypeAttributeState::Number:
        return true;
    default:
        return false;
    }
}

static Optional<CSS::SizeWithAspectRatio> default_preferred_size_for_appearance_none_text_input(Box const& box)
{
    if (box.computed_values().appearance() != CSS::Appearance::None)
        return {};

    auto const* input_element = as_if<HTML::HTMLInputElement>(box.dom_node());
    if (!input_element || !is_text_control_input(*input_element))
        return {};

    // https://drafts.csswg.org/css-ui-4/#appearance-switching
    // The element is rendered following the usual rules of CSS. Replaced elements other than widgets are not affected
    // by this and remain replaced elements. Widgets must not have their native appearance, and instead must have their
    // primitive appearance.
    //
    // https://html.spec.whatwg.org/multipage/rendering.html#the-input-element-as-a-text-entry-widget
    // An input element whose type attribute is in one of the above states is an element with default preferred size,
    // and user agents are expected to apply the 'field-sizing' CSS property to the element.
    return TextInputBox::default_preferred_size_for_text_control(*input_element, box);
}

static CSS::SizeWithAspectRatio intrinsic_size_for_replaced_sizing(Box const& box)
{
    auto intrinsic_size = box.auto_content_box_size();
    if (intrinsic_size.has_width() || intrinsic_size.has_height() || intrinsic_size.has_aspect_ratio())
        return intrinsic_size;

    return default_preferred_size_for_appearance_none_text_input(box).value_or(intrinsic_size);
}

FormattingContext::FormattingContext(Type type, LayoutMode layout_mode, LayoutState& state, Box const& context_box, FormattingContext* parent)
    : m_type(type)
    , m_layout_mode(layout_mode)
    , m_parent(parent)
    , m_context_box(context_box)
    , m_state(state)
{
}

FormattingContext::~FormattingContext() = default;

void FormattingContext::place_child(Box const& child, CSSPixelPoint content_offset)
{
    m_state.get_mutable(child).place(content_offset);
}

void FormattingContext::dimension_list_item_marker(ListItemMarkerBox const& marker)
{
    auto& marker_state = m_state.get_mutable(marker);

    if (auto const* list_style_image = marker.list_style_image()) {
        marker_state.set_content_inline_size(list_style_image->natural_width(marker.document()).value_or(0));
        marker_state.set_content_block_size(list_style_image->natural_height(marker.document()).value_or(0));
        return;
    }

    auto marker_size = marker.relative_size();
    marker_state.set_content_block_size(marker_size);

    auto const& marker_font = marker.first_available_font();
    if (auto marker_text = marker.text(); marker_text.has_value()) {
        // FIXME: Use per-code-point fonts to measure text.
        auto text_inline_size = marker_font.width(marker_text.value());
        marker_state.set_content_inline_size(CSSPixels::nearest_value_for(text_inline_size));
    } else {
        marker_state.set_content_inline_size(marker_size);
    }
}

CSSPixels FormattingContext::distance_between_marker_and_list_item(ListItemMarkerBox const& marker)
{
    if (marker.text().has_value())
        return 0;
    return CSSPixels::nearest_value_for(.5f * marker.first_available_font().pixel_size());
}

bool FormattingContext::computed_block_size_establishes_definite_containing_block_size(CSS::Size const& computed_block_size)
{
    // A resolved used block size is not always a definite containing block size.
    // Intrinsic sizing keywords like fit-content still depend on child layout,
    // so percentage-sized descendants must continue to treat it as indefinite.
    return !computed_block_size.is_intrinsic_sizing_constraint();
}

// https://developer.mozilla.org/en-US/docs/Web/Guide/CSS/Block_formatting_context
bool FormattingContext::creates_block_formatting_context(Box const& box)
{
    // NOTE: Replaced elements never create a BFC.
    if (box.is_replaced_box())
        return false;

    // AD-HOC: We create a BFC for SVG foreignObject.
    if (box.is_svg_foreign_object_box())
        return true;

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
    if (box.has_layout_containment() || box.has_paint_containment())
        return true;

    // https://drafts.csswg.org/css-conditional-5/#valdef-container-type-size
    // Applies style containment and size containment to the principal box, and establishes an independent formatting
    // context.
    if (box.computed_values().container_type().is_size_container || box.computed_values().container_type().is_inline_size_container)
        return true;

    if (box.parent()) {
        auto parent_display = box.parent()->display();

        // Flex items (direct children of the element with display: flex or inline-flex) if they are neither flex nor grid nor table containers themselves.
        if (parent_display.is_flex_inside())
            return true;
        // Grid items (direct children of the element with display: grid or inline-grid) if they are neither flex nor grid nor table containers themselves.
        if (parent_display.is_grid_inside())
            return true;
    }

    // https://drafts.csswg.org/css-multicol-2/#the-multi-column-model
    // An element whose 'column-width', 'column-count', or 'column-height' property is not 'auto' establishes a multi-
    // column container (or multicol container for short), and therefore acts as a container for multi-column layout.
    // FIXME: Maybe add column-height, depending on the resolution for https://github.com/w3c/csswg-drafts/issues/12688
    if (!box.computed_values().column_width().is_auto() || !box.computed_values().column_count().is_auto())
        return true;

    // FIXME: column-span: all should always create a new formatting context, even when the column-span: all element isn't contained by a multicol container (Spec change, Chrome bug).

    // https://html.spec.whatwg.org/multipage/rendering.html#the-fieldset-and-legend-elements
    if (box.is_fieldset_box())
        // The fieldset element, when it generates a CSS box, is expected to act as follows:
        // The element is expected to establish a new block formatting context.
        return true;

    // https://html.spec.whatwg.org/multipage/rendering.html#button-layout
    // An element using button layout establishes a new formatting context for its contents.
    if (auto const* html_element = as_if<HTML::HTMLElement>(box.dom_node()); html_element && html_element->uses_button_layout())
        return true;

    return false;
}

Optional<FormattingContext::Type> FormattingContext::formatting_context_type_created_by_box(Box const& box)
{
    if (is<SVGSVGBox>(box))
        return Type::SVG;

    if (box.is_replaced_box_with_children())
        return Type::ReplacedWithChildren;

    if (box.is_replaced_box())
        return Type::InternalReplaced;

    if (!box.can_have_children())
        return {};

    auto display = box.display();

    // Native controls can use a generic box to host their shadow tree. When a table-specific
    // display was adjusted to inline or block, keep that box atomic and give its contents an
    // independent context without changing the formatting of ordinary controls.
    if (box.has_replaced_element_table_display_adjustment()) {
        if (is<BlockContainer>(box))
            return Type::Block;
        return Type::InternalReplaced;
    }

    if (display.is_flex_inside())
        return Type::Flex;

    if (display.is_table_inside())
        return Type::Table;

    if (display.is_grid_inside())
        return Type::Grid;

    if (display.is_math_inside())
        // FIXME: We should create a MathML-specific formatting context here, but for now use a BFC, so _something_ is displayed
        return Type::Block;

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

Box const& FormattingContext::box_establishing_containing_formatting_context(Box const& child)
{
    auto const* box = child.containing_block();
    VERIFY(box);
    while (box->containing_block() && !formatting_context_type_created_by_box(*box).has_value())
        box = box->containing_block();
    return *box;
}

void FormattingContext::register_contained_abspos_child(Box const& child, StaticPositionRect const& static_position_rect)
{
    if (!child.containing_block())
        return;
    m_state.register_contained_abspos_child(box_establishing_containing_formatting_context(child), child, static_position_rect);
}

LogicalOffset FormattingContext::aligned_static_offset(StaticPositionRect const& static_position_rect, LayoutState::UsedValues const& box_state)
{
    return static_position_rect.aligned_offset_for_box_with_size({ box_state.margin_box_inline_size(), box_state.margin_box_block_size() });
}

// FIXME: This is a hack. Get rid of it.
struct ReplacedFormattingContext : public FormattingContext {
    ReplacedFormattingContext(LayoutState& state, LayoutMode layout_mode, Box const& box)
        : FormattingContext(Type::InternalReplaced, layout_mode, state, box)
    {
    }
    virtual CSSPixels automatic_content_inline_size() const override { return 0; }
    virtual CSSPixels automatic_content_block_size() const override { return 0; }
    virtual void run(LayoutInput const&) override { }
};

// FIXME: This is a hack. Get rid of it.
struct DummyFormattingContext : public FormattingContext {
    DummyFormattingContext(LayoutState& state, LayoutMode layout_mode, Box const& box)
        : FormattingContext(Type::InternalDummy, layout_mode, state, box)
    {
    }
    virtual CSSPixels automatic_content_inline_size() const override { return 0; }
    virtual CSSPixels automatic_content_block_size() const override { return 0; }
    virtual void run(LayoutInput const&) override { }
};

OwnPtr<FormattingContext> FormattingContext::create_independent_formatting_context_if_needed(LayoutState& state, LayoutMode layout_mode, Box const& child_box, FormattingContext* parent)
{
    auto type = formatting_context_type_created_by_box(child_box);
    if (!type.has_value())
        return nullptr;

    switch (type.value()) {
    case Type::Block:
        return make<BlockFormattingContext>(state, layout_mode, as<BlockContainer>(child_box), parent);
    case Type::SVG:
        return make<SVGFormattingContext>(state, layout_mode, child_box, parent);
    case Type::Flex:
        return make<FlexFormattingContext>(state, layout_mode, child_box, parent);
    case Type::Grid:
        return make<GridFormattingContext>(state, layout_mode, child_box, parent);
    case Type::Table:
        return make<TableFormattingContext>(state, layout_mode, child_box, parent);
    case Type::ReplacedWithChildren:
        return make<ReplacedWithChildrenFormattingContext>(state, layout_mode, child_box, parent);
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

NonnullOwnPtr<FormattingContext> FormattingContext::create_independent_formatting_context(LayoutState& state, LayoutMode layout_mode, Box const& child_box, FormattingContext* parent)
{
    if (auto context = create_independent_formatting_context_if_needed(state, layout_mode, child_box, parent))
        return context.release_nonnull();

    if (auto child_block_container = as_if<BlockContainer>(child_box))
        return make<BlockFormattingContext>(state, layout_mode, *child_block_container, nullptr);

    // HACK: Instead of crashing in scenarios that assume the formatting context can be created, create a dummy formatting context that does nothing.
    dbgln("FIXME: An independent formatting context was requested from a Box that does not have a formatting context type. A dummy formatting context will be created instead.");
    return make<DummyFormattingContext>(state, layout_mode, child_box);
}

OwnPtr<FormattingContext> FormattingContext::layout_inside(Box const& child_box, LayoutMode layout_mode, LayoutInput const& layout_input)
{
    {
        // OPTIMIZATION: If we're doing intrinsic sizing and `child_box` has definite size in both axes,
        //               we don't need to layout its insides. The size is resolvable without learning
        //               the metrics of whatever's inside the box.
        //
        // https://drafts.csswg.org/css2/#propdef-vertical-align
        // The baseline of an inline-block is the baseline of its last line box in the normal flow, unless it has
        // either no in-flow line boxes or if its 'overflow' property has a computed value other than visible, in which
        // case the baseline is the bottom margin edge.
        //
        // Inline-level boxes can contribute a baseline to their parent line box, so they still need their contents
        // laid out even when their own intrinsic size is already definite.
        auto const& used_values = m_state.get(child_box);
        if (layout_mode == LayoutMode::IntrinsicSizing
            && !child_box.is_inline()
            && used_values.inline_size_constraint == SizeConstraint::None
            && used_values.block_size_constraint == SizeConstraint::None
            && used_values.has_definite_inline_size()
            && used_values.has_definite_block_size()) {
            return nullptr;
        }
    }

    if (!child_box.can_have_children())
        return {};

    auto independent_formatting_context = create_independent_formatting_context_if_needed(m_state, layout_mode, child_box, this);
    if (independent_formatting_context)
        independent_formatting_context->run(layout_input);
    else
        run(layout_input);

    return independent_formatting_context;
}

CSSPixels FormattingContext::greatest_child_inline_size(Box const& box) const
{
    CSSPixels max_inline_size = 0;
    if (box.children_are_inline()) {
        for (auto& line_box : m_state.get(box).line_boxes)
            max_inline_size = max(max_inline_size, line_box_physical_horizontal_extent(box, line_box));
    } else {
        box.for_each_child_of_type<Box>([&](Box const& child) {
            if (!child.is_absolutely_positioned())
                max_inline_size = max(max_inline_size, m_state.get(child).margin_box_inline_size());
            return IterationDecision::Continue;
        });
    }
    return max_inline_size;
}

CSSPixels FormattingContext::line_box_physical_horizontal_extent(Box const& box, LineBox const& line_box)
{
    if (line_box.has_block_level_box())
        return line_box.inline_length();

    if (box.computed_values().writing_mode() == CSS::WritingMode::HorizontalTb)
        return line_box.inline_length();

    CSSPixels leftmost_physical_horizontal_offset = 0;
    CSSPixels rightmost_physical_horizontal_offset = 0;
    bool saw_fragment = false;
    for (auto const& fragment : line_box.fragments()) {
        auto fragment_physical_left = fragment.offset().x();
        auto fragment_physical_right = fragment_physical_left + fragment.physical_horizontal_extent();
        leftmost_physical_horizontal_offset = saw_fragment ? min(leftmost_physical_horizontal_offset, fragment_physical_left) : fragment_physical_left;
        rightmost_physical_horizontal_offset = saw_fragment ? max(rightmost_physical_horizontal_offset, fragment_physical_right) : fragment_physical_right;
        saw_fragment = true;
    }

    if (!saw_fragment)
        return 0;

    return rightmost_physical_horizontal_offset - leftmost_physical_horizontal_offset;
}

FormattingContext::ShrinkToFitInlineSizeResult FormattingContext::calculate_shrink_to_fit_inline_sizes(Box const& box, ContainingBlockConstraints const& containing_block_constraints)
{
    return {
        .preferred_inline_size = calculate_max_content_inline_size(box, containing_block_constraints),
        .preferred_minimum_inline_size = calculate_min_content_inline_size(box, containing_block_constraints),
    };
}

LogicalSize FormattingContext::solve_replaced_size_constraint(CSSPixels input_inline_size, CSSPixels input_block_size, Box const& box, AvailableSpace const& available_space, ContainingBlockConstraints const& containing_block_constraints) const
{
    // 10.4 Minimum and maximum widths: 'min-width' and 'max-width'
    // https://www.w3.org/TR/CSS22/visudet.html#min-max-widths

    auto min_inline_size = box.computed_values().min_width().is_auto() ? 0 : calculate_inner_inline_size(box, available_space.inline_size, box.computed_values().min_width(), containing_block_constraints);
    auto specified_max_inline_size = should_treat_max_inline_size_as_none(box, available_space.inline_size, containing_block_constraints) ? input_inline_size : calculate_inner_inline_size(box, available_space.inline_size, box.computed_values().max_width(), containing_block_constraints);
    auto max_inline_size = max(min_inline_size, specified_max_inline_size);

    auto min_block_size = box.computed_values().min_height().is_auto() ? 0 : calculate_inner_block_size(box, available_space, box.computed_values().min_height(), containing_block_constraints);
    auto specified_max_block_size = should_treat_max_block_size_as_none(box, available_space.block_size, containing_block_constraints) ? input_block_size : calculate_inner_block_size(box, available_space, box.computed_values().max_height(), containing_block_constraints);
    auto max_block_size = max(min_block_size, specified_max_block_size);

    auto const& box_state = m_state.get(box);
    // These are from the "Constraint Violation" table in spec, but reordered so that each condition is
    // interpreted as mutually exclusive to any other.
    if (input_inline_size < min_inline_size && input_block_size > max_block_size)
        return { min_inline_size, max_block_size };
    if (input_inline_size > max_inline_size && input_block_size < min_block_size)
        return { max_inline_size, min_block_size };

    if (input_inline_size > 0 && input_block_size > 0) {
        if (input_inline_size > max_inline_size && input_block_size > max_block_size && max_inline_size / input_inline_size <= max_block_size / input_block_size)
            return { max_inline_size, max(min_block_size, content_block_size_from_aspect_ratio(box, box_state, max_inline_size)) };
        if (input_inline_size > max_inline_size && input_block_size > max_block_size && max_inline_size / input_inline_size > max_block_size / input_block_size)
            return { max(min_inline_size, content_inline_size_from_aspect_ratio(box, box_state, max_block_size)), max_block_size };
        if (input_inline_size < min_inline_size && input_block_size < min_block_size && min_inline_size / input_inline_size <= min_block_size / input_block_size)
            return { min(max_inline_size, content_inline_size_from_aspect_ratio(box, box_state, min_block_size)), min_block_size };
        if (input_inline_size < min_inline_size && input_block_size < min_block_size && min_inline_size / input_inline_size > min_block_size / input_block_size)
            return { min_inline_size, min(max_block_size, content_block_size_from_aspect_ratio(box, box_state, min_inline_size)) };
    }

    if (input_inline_size > max_inline_size)
        return { max_inline_size, max(content_block_size_from_aspect_ratio(box, box_state, max_inline_size), min_block_size) };
    if (input_inline_size < min_inline_size)
        return { min_inline_size, min(content_block_size_from_aspect_ratio(box, box_state, min_inline_size), max_block_size) };
    if (input_block_size > max_block_size)
        return { max(content_inline_size_from_aspect_ratio(box, box_state, max_block_size), min_inline_size), max_block_size };
    if (input_block_size < min_block_size)
        return { min(content_inline_size_from_aspect_ratio(box, box_state, min_block_size), max_inline_size), min_block_size };

    return { input_inline_size, input_block_size };
}

Optional<CSSPixels> FormattingContext::compute_automatic_block_size_for_absolutely_positioned_element(Box const& box, AvailableSpace const& available_space, ContainingBlockConstraints const& containing_block_constraints, BeforeOrAfterInsideLayout before_or_after_inside_layout) const
{
    // NOTE: CSS 2.2 tells us to use the automatic block size for block formatting context roots here.
    //       That's fine as long as the box is a BFC root.
    if (creates_block_formatting_context(box)) {
        if (before_or_after_inside_layout == BeforeOrAfterInsideLayout::Before)
            return {};
        return compute_automatic_block_size_for_block_formatting_context_root(box);
    }

    // NOTE: For anything else, we use the fit-content block size.
    //       This should eventually be replaced by the new absolute positioning model:
    //       https://www.w3.org/TR/css-position-3/#abspos-layout
    return calculate_fit_content_block_size(box, m_state.get(box).available_inner_space_or_constraints_from(available_space), containing_block_constraints);
}

// https://www.w3.org/TR/CSS22/visudet.html#root-height
CSSPixels FormattingContext::compute_automatic_block_size_for_block_formatting_context_root(Box const& root) const
{
    // 10.6.7 'Auto' heights for block formatting context roots
    Optional<CSSPixels> top;
    Optional<CSSPixels> bottom;

    if (root.children_are_inline()) {
        // If it only has inline-level children, the block size is the distance between
        // the top content edge and the bottom of the bottommost line box.
        auto const& line_boxes = m_state.get(root).line_boxes;
        top = 0;
        if (!line_boxes.is_empty()) {
            bottom = line_boxes.last().physical_vertical_end();
            // A trailing interrupting block's bottom margin cannot collapse out of a BFC root,
            // so it contributes to the root's automatic block size. The line box bottom excludes it.
            if (line_boxes.last().has_block_level_box())
                bottom = max(CSSPixels(0), bottom.value() + line_boxes.last().block_level_box_block_end_margin());
        }
    } else {
        // If it has block-level children, the block size is the distance between
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

            if (child_box.is_floating())
                return IterationDecision::Continue;

            // Children that have not been laid out yet contribute nothing to the automatic block size.
            auto const* child_box_state = m_state.try_get(child_box);
            if (!child_box_state)
                return IterationDecision::Continue;

            CSSPixels child_box_bottom = child_box_state->content_logical_offset().block_offset + child_box_state->content_block_size() + child_box_state->margin_box_bottom();

            if (!bottom.has_value() || child_box_bottom > bottom.value())
                bottom = child_box_bottom;

            return IterationDecision::Continue;
        });
    }

    // In addition, if the element has any floating descendants
    // whose bottom margin edge is below the element's bottom content edge,
    // then the block size is increased to include those edges.
    if (auto lowest_float_bottom_margin_edge = m_state.get(root).lowest_floating_descendant_bottom_margin_edge(); lowest_float_bottom_margin_edge.has_value()) {
        if (!bottom.has_value() || *lowest_float_bottom_margin_edge > bottom.value())
            bottom = lowest_float_bottom_margin_edge;
    }

    return max(CSSPixels(0.0f), bottom.value_or(0) - top.value_or(0));
}

CSSPixels FormattingContext::measure_automatic_content_block_size(Box const& box, AvailableSpace const& inner_available_space, ContainingBlockConstraints const& containing_block_constraints)
{
    LayoutState throwaway_state(box, LayoutState::Purpose::Measurement);
    throwaway_state.create(box, containing_block_constraints.percentage_basis_inline_size, containing_block_constraints.percentage_basis_block_size);
    auto measuring_context = create_independent_formatting_context_if_needed(throwaway_state, m_layout_mode, box, this);
    measuring_context->run(LayoutInput { inner_available_space, containing_block_constraints });
    return measuring_context->automatic_content_block_size();
}

void FormattingContext::make_button_content_box_definite(Box const& box, AvailableSpace const& available_space, ContainingBlockConstraints const& containing_block_constraints, Optional<CSSPixels> measured_content_block_size)
{
    auto const* html_element = as_if<HTML::HTMLElement>(box.dom_node());
    if (!html_element || !html_element->uses_button_layout())
        return;

    // Flex/grid-inside buttons are their own flex/grid container and get no anonymous content wrapper,
    // so there is nothing to make definite for centering.
    auto display = box.display();
    if (display.is_flex_inside() || display.is_grid_inside())
        return;

    auto const& computed_values = box.computed_values();

    // With auto height and no min-height the content box already exactly wraps the content, so there is
    // no extra space to center within and no need to force a definite content box.
    if (computed_values.height().is_auto() && computed_values.min_height().is_auto())
        return;

    auto& box_state = m_state.get_mutable(box);
    if (box_state.has_definite_block_size())
        return;

    auto natural_content_block_size = measured_content_block_size.value_or_lazy_evaluated([&] {
        return measure_automatic_content_block_size(box, box_state.available_inner_space_or_constraints_from(available_space), containing_block_constraints);
    });

    auto used_block_size = should_treat_block_size_as_auto(box, available_space, containing_block_constraints)
        ? natural_content_block_size
        : calculate_inner_block_size(box, available_space, computed_values.height(), containing_block_constraints);
    if (!should_treat_max_block_size_as_none(box, available_space.block_size, containing_block_constraints) && !computed_values.max_height().is_auto())
        used_block_size = min(used_block_size, calculate_inner_block_size(box, available_space, computed_values.max_height(), containing_block_constraints));
    if (!computed_values.min_height().is_auto())
        used_block_size = max(used_block_size, calculate_inner_block_size(box, available_space, computed_values.min_height(), containing_block_constraints));

    // Only force a definite content box when the button's used block size exceeds its content block size, so a larger
    // preferred or minimum size has room to center within. A content-sized box stays indefinite, so an intrinsic
    // keyword does not resolve percentage-sized descendants.
    if (used_block_size <= natural_content_block_size)
        return;

    box_state.set_content_block_size(used_block_size);
    box_state.set_has_definite_block_size(true);
}

// 17.5.2 Table width algorithms: the 'table-layout' property
// https://www.w3.org/TR/CSS22/tables.html#width-layout
CSSPixels FormattingContext::compute_table_box_inline_size_inside_table_wrapper(
    Box const& box,
    AvailableSpace const& available_space,
    ContainingBlockConstraints const& table_wrapper_constraints,
    Optional<CSSPixels> table_wrapper_containing_block_inline_size,
    TableWrapperInlineSizeMode table_wrapper_inline_size_mode)
{
    // CSS 2 says the table wrapper inline size is the border-edge inline size of the table grid box inside it.

    auto const& computed_values = box.computed_values();

    auto containing_block_inline_size = table_wrapper_containing_block_inline_size.value_or(available_space.inline_size.to_px_or_zero());

    // If 'margin-left', or 'margin-right' are computed as 'auto', their used value is '0'.
    auto margin_left = computed_values.margin().left().to_px_or_zero(containing_block_inline_size);
    auto margin_right = computed_values.margin().right().to_px_or_zero(containing_block_inline_size);

    // table-wrapper can't have borders or paddings but it might have margin taken from table-root.
    auto available_inline_size = containing_block_inline_size - margin_left - margin_right;

    Optional<Box const&> table_box;
    box.for_each_in_subtree_of_type<Box>([&](Box const& child_box) {
        if (child_box.display().is_table_inside()) {
            table_box = child_box;
            return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    });
    VERIFY(table_box.has_value());

    LayoutState throwaway_state(box, LayoutState::Purpose::Measurement);

    // The table wrapper is invisible to percentage resolution, so the table box gets the
    // wrapper's constraints unchanged. Callers measuring a table wrapper for grid alignment
    // pass the grid-area inline size as the wrapper's percentage basis.
    throwaway_state.create(box, table_wrapper_constraints.percentage_basis_inline_size, table_wrapper_constraints.percentage_basis_block_size);
    auto const& table_constraints = table_wrapper_constraints;
    auto& table_box_state = throwaway_state.create(*table_box, table_constraints.percentage_basis_inline_size, table_constraints.percentage_basis_block_size);
    auto const& table_box_computed_values = table_box->computed_values();
    table_box_state.border_left = table_box_computed_values.border_left().width;
    table_box_state.border_right = table_box_computed_values.border_right().width;
    table_box_state.padding_left = table_box_computed_values.padding().left().to_px_or_zero(containing_block_inline_size);
    table_box_state.padding_right = table_box_computed_values.padding().right().to_px_or_zero(containing_block_inline_size);

    auto context = make<TableFormattingContext>(throwaway_state, LayoutMode::IntrinsicSizing, *table_box, this);
    context->run_until_inline_size_calculation(
        LayoutInput { table_box_state.available_inner_space_or_constraints_from(available_space), table_constraints },
        TableFormattingContext::RowMeasurement::Skip);

    auto table_used_inline_size = throwaway_state.get(*table_box).border_box_inline_size();
    if (table_wrapper_inline_size_mode == TableWrapperInlineSizeMode::UseTableUsedInlineSizeIfNotAuto
        && !table_box->computed_values().width().is_auto()) {
        return table_used_inline_size;
    }
    return available_space.inline_size.is_definite() ? min(table_used_inline_size, available_inline_size) : table_used_inline_size;
}

// 17.5.3 Table height algorithms
// https://www.w3.org/TR/CSS22/tables.html#height-layout
CSSPixels FormattingContext::compute_table_box_block_size_inside_table_wrapper(Box const& box, AvailableSpace const& available_space, ContainingBlockConstraints const& table_wrapper_constraints)
{
    // The table wrapper block size should equal the block size of the table box it contains.

    auto const& computed_values = box.computed_values();

    auto containing_block_inline_size = available_space.inline_size.to_px_or_zero();
    auto containing_block_block_size = available_space.block_size.to_px_or_zero();

    // If 'margin-top', or 'margin-bottom' are computed as 'auto', their used value is '0'.
    auto margin_top = computed_values.margin().top().resolved_or_auto(containing_block_inline_size).to_px_or_zero();
    auto margin_bottom = computed_values.margin().bottom().resolved_or_auto(containing_block_inline_size).to_px_or_zero();

    // table-wrapper can't have borders or paddings but it might have margin taken from table-root.
    auto available_block_size = containing_block_block_size - margin_top - margin_bottom;

    LayoutState throwaway_state(box, LayoutState::Purpose::Measurement);
    throwaway_state.create(box, table_wrapper_constraints.percentage_basis_inline_size, table_wrapper_constraints.percentage_basis_block_size);

    auto context = create_independent_formatting_context_if_needed(throwaway_state, LayoutMode::IntrinsicSizing, box, this);
    VERIFY(context);
    context->run(LayoutInput { m_state.get(box).available_inner_space_or_constraints_from(available_space), table_wrapper_constraints });

    Optional<Box const&> table_box;
    box.for_each_in_subtree_of_type<Box>([&](Box const& child_box) {
        if (child_box.display().is_table_inside()) {
            table_box = child_box;
            return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    });
    VERIFY(table_box.has_value());

    auto table_used_block_size = throwaway_state.get(*table_box).border_box_block_size();
    return available_space.block_size.is_definite() ? min(table_used_block_size, available_block_size) : table_used_block_size;
}

ContainingBlockConstraints FormattingContext::constraints_for_child_context(
    LayoutState::UsedValues const& containing_block_used_values,
    ContainingBlockConstraints const& containing_block_constraints)
{
    auto const& containing_block = containing_block_used_values.node();
    auto const* containing_block_box = as_if<Box>(containing_block);
    // Anonymous boxes are invisible to percentage resolution: their children resolve percentages
    // against the closest non-anonymous ancestor, so an anonymous containing block without a
    // definite size of its own passes the constraints it was given through. Anonymous table
    // cells are the exception: they are proper containing blocks with their own size semantics.
    auto should_forward_indefinite_basis = containing_block_box
        && containing_block_box->is_anonymous()
        && !containing_block_box->display().is_table_cell()
        && !containing_block_box->has_auto_content_box_size()
        && containing_block_used_values.inline_size_constraint == SizeConstraint::None
        && containing_block_used_values.block_size_constraint == SizeConstraint::None;

    auto percentage_basis_inline_size = containing_block_used_values.has_definite_inline_size()
        ? Optional<CSSPixels> { containing_block_used_values.content_inline_size() }
        : should_forward_indefinite_basis ? containing_block_constraints.percentage_basis_inline_size
                                          : Optional<CSSPixels> {};
    auto percentage_basis_block_size = containing_block_used_values.has_definite_block_size()
        ? Optional<CSSPixels> { containing_block_used_values.content_block_size() }
        : should_forward_indefinite_basis ? containing_block_constraints.percentage_basis_block_size
                                          : Optional<CSSPixels> {};

    // https://quirks.spec.whatwg.org/#the-percentage-height-calculation-quirk
    auto quirks_mode_percentage_basis_block_size = [&]() -> Optional<CSSPixels> {
        // 1. Let element be the nearest ancestor containing block of element, if there is one.
        //    Otherwise, return the initial containing block.
        if (containing_block.is_viewport())
            return containing_block_used_values.content_block_size();

        // 2. If element has a computed value of the display property that is table-cell, then return a
        //    UA-defined value.
        if (containing_block.display().is_table_cell()) {
            // FIXME: Likely UA-defined value should not be 0.
            return CSSPixels(0);
        }

        // 3. If element has a computed value of the height property that is not auto, then return element.
        if (!containing_block.computed_values().height().is_auto())
            return containing_block_used_values.content_block_size();

        // 4. If element has a computed value of the position property that is absolute, or if element is a
        //    not a block container or a table wrapper box, then return element.
        if (containing_block.is_absolutely_positioned() || !is<BlockContainer>(containing_block) || is<TableWrapper>(containing_block))
            return containing_block_used_values.content_block_size();

        // 5. Jump to the first step.
        // NOTE: Evaluated incrementally: in-flow auto-height block containers pass the basis they
        //       inherited from their own containing block through to their children.
        return containing_block_constraints.quirks_mode_percentage_basis_block_size;
    }();

    return { percentage_basis_inline_size, percentage_basis_block_size, quirks_mode_percentage_basis_block_size };
}

LayoutInput FormattingContext::layout_input_for_child_context(
    LayoutState::UsedValues const& containing_block_used_values,
    LayoutInput const& containing_block_layout_input,
    AvailableSpace available_space)
{
    return LayoutInput {
        available_space,
        constraints_for_child_context(containing_block_used_values, containing_block_layout_input.containing_block_constraints),
        containing_block_layout_input.content_box_position_in_bfc_root,
    };
}

// 10.3.2 Inline, replaced elements, https://www.w3.org/TR/CSS22/visudet.html#inline-replaced-width
CSSPixels FormattingContext::tentative_inline_size_for_replaced_element(Box const& box, CSS::Size const& computed_inline_size, AvailableSpace const& available_space, ContainingBlockConstraints const& containing_block_constraints) const
{
    // Treat percentages of indefinite containing block widths as 0 (the initial width).
    if (computed_inline_size.is_percentage() && !containing_block_constraints.percentage_basis_inline_size.has_value())
        return 0;

    auto computed_block_size = should_treat_block_size_as_auto(box, available_space, containing_block_constraints) ? CSS::Size::make_auto() : box.computed_values().height();

    CSSPixels used_inline_size = 0;
    if (computed_inline_size.is_auto()) {
        used_inline_size = computed_inline_size.to_px(available_space.inline_size.to_px_or_zero());
    } else {
        used_inline_size = calculate_inner_inline_size(box, available_space.inline_size, computed_inline_size, containing_block_constraints);
    }

    // If 'height' and 'width' both have computed values of 'auto' and the element also has an intrinsic width,
    // then that intrinsic width is the used value of 'width'.
    auto intrinsic = intrinsic_size_for_replaced_sizing(box);
    if (computed_block_size.is_auto() && computed_inline_size.is_auto() && intrinsic.has_width())
        return intrinsic.width.value();

    // If 'height' and 'width' both have computed values of 'auto' and the element has no intrinsic width,
    // but does have an intrinsic height and intrinsic ratio;
    // or if 'width' has a computed value of 'auto',
    // 'height' has some other computed value, and the element does have an intrinsic ratio; then the used value of 'width' is:
    //
    //     (used height) * (intrinsic ratio)
    if ((computed_block_size.is_auto() && computed_inline_size.is_auto() && !intrinsic.has_width() && intrinsic.has_height() && box.has_preferred_aspect_ratio())
        || (computed_inline_size.is_auto() && !computed_block_size.is_auto() && box.has_preferred_aspect_ratio())) {
        auto content_block_size = compute_block_size_for_replaced_element(box, available_space, containing_block_constraints);
        return content_inline_size_from_aspect_ratio(box, m_state.get(box), content_block_size);
    }

    // If 'height' and 'width' both have computed values of 'auto' and the element has an intrinsic ratio but no intrinsic height or width,
    // then the used value of 'width' is undefined in CSS 2.2. However, it is suggested that, if the containing block's width does not itself
    // depend on the replaced element's width, then the used value of 'width' is calculated from the constraint equation used for block-level,
    // non-replaced elements in normal flow.
    if (computed_block_size.is_auto() && computed_inline_size.is_auto() && !intrinsic.has_width() && !intrinsic.has_height() && box.has_preferred_aspect_ratio()) {
        if (!available_space.inline_size.is_intrinsic_sizing_constraint())
            return calculate_stretch_fit_inline_size(box, available_space.inline_size);

        switch (cyclic_percentage_intrinsic_contribution(box, box.computed_values().width(), available_space.inline_size, CyclicPercentageSizeProperty::PreferredOrMaxSize)) {
        case CyclicPercentageIntrinsicContribution::ResolveAsZero:
            return 0;
        case CyclicPercentageIntrinsicContribution::TreatAsInitialValue:
            break;
        case CyclicPercentageIntrinsicContribution::NotCyclic:
            return calculate_stretch_fit_inline_size(box, available_space.inline_size);
        }
    }

    // Otherwise, if 'width' has a computed value of 'auto', and the element has an intrinsic width, then that intrinsic width is the used value of 'width'.
    if (computed_inline_size.is_auto() && intrinsic.has_width())
        return intrinsic.width.value();

    // Otherwise, if 'width' has a computed value of 'auto', but none of the conditions above are met, then the used value of 'width' becomes 300px.
    // If 300px is too wide to fit the device, UAs should use the width of the largest rectangle that has a 2:1 ratio and fits the device instead.
    if (computed_inline_size.is_auto())
        return 300;

    return used_inline_size;
}

void FormattingContext::compute_inline_size_for_absolutely_positioned_element(Box const& box, AvailableSpace const& available_space, ContainingBlockConstraints const& containing_block_constraints, StaticPositionRect const& static_position_rect)
{
    if (box_is_sized_as_replaced_element(box, available_space, containing_block_constraints))
        compute_inline_size_for_absolutely_positioned_replaced_element(box, available_space, containing_block_constraints, static_position_rect);
    else
        compute_inline_size_for_absolutely_positioned_non_replaced_element(box, available_space, containing_block_constraints, static_position_rect);
}

void FormattingContext::compute_block_size_for_absolutely_positioned_element(Box const& box, AvailableSpace const& available_space, ContainingBlockConstraints const& containing_block_constraints, StaticPositionRect const& static_position_rect, BeforeOrAfterInsideLayout before_or_after_inside_layout)
{
    if (box_is_sized_as_replaced_element(box, available_space, containing_block_constraints))
        compute_block_size_for_absolutely_positioned_replaced_element(box, available_space, containing_block_constraints, static_position_rect, before_or_after_inside_layout);
    else
        compute_block_size_for_absolutely_positioned_non_replaced_element(box, available_space, containing_block_constraints, static_position_rect, before_or_after_inside_layout);
}

CSSPixels FormattingContext::compute_inline_size_for_replaced_element(Box const& box, AvailableSpace const& available_space, ContainingBlockConstraints const& containing_block_constraints) const
{
    // 10.3.4 Block-level, replaced elements in normal flow...
    // 10.3.2 Inline, replaced elements

    auto computed_inline_size = should_treat_inline_size_as_auto(box, available_space) ? CSS::Size::make_auto() : box.computed_values().width();
    auto computed_block_size = should_treat_block_size_as_auto(box, available_space, containing_block_constraints) ? CSS::Size::make_auto() : box.computed_values().height();

    // 1. The tentative used width is calculated (without 'min-width' and 'max-width')
    auto used_inline_size = tentative_inline_size_for_replaced_element(box, computed_inline_size, available_space, containing_block_constraints);

    if (computed_inline_size.is_auto() && computed_block_size.is_auto() && box.has_preferred_aspect_ratio()) {
        CSSPixels inline_size = used_inline_size;
        CSSPixels block_size = tentative_block_size_for_replaced_element(box, computed_block_size, available_space, containing_block_constraints);
        used_inline_size = solve_replaced_size_constraint(inline_size, block_size, box, available_space, containing_block_constraints).inline_size;
    }

    // 2. If the tentative used width is greater than 'max-width', the rules above are applied again,
    //    but this time using the computed value of 'max-width' as the computed value for 'width'.
    if (!should_treat_max_inline_size_as_none(box, available_space.inline_size, containing_block_constraints)) {
        auto const& computed_max_inline_size = box.computed_values().max_width();
        auto max_inline_size = calculate_inner_inline_size(box, available_space.inline_size, computed_max_inline_size, containing_block_constraints);
        if (used_inline_size > max_inline_size)
            used_inline_size = tentative_inline_size_for_replaced_element(box, computed_max_inline_size, available_space, containing_block_constraints);
    }

    // 3. If the resulting width is smaller than 'min-width', the rules above are applied again,
    //    but this time using the value of 'min-width' as the computed value for 'width'.
    auto computed_min_inline_size = box.computed_values().min_width();
    if (!computed_min_inline_size.is_auto()) {
        auto min_inline_size = calculate_inner_inline_size(box, available_space.inline_size, computed_min_inline_size, containing_block_constraints);
        if (used_inline_size < min_inline_size)
            used_inline_size = tentative_inline_size_for_replaced_element(box, computed_min_inline_size, available_space, containing_block_constraints);
    }

    return used_inline_size;
}

// 10.6.2 Inline replaced elements, block-level replaced elements in normal flow, 'inline-block' replaced elements in normal flow and floating replaced elements
// https://www.w3.org/TR/CSS22/visudet.html#inline-replaced-height
CSSPixels FormattingContext::tentative_block_size_for_replaced_element(Box const& box, CSS::Size const& computed_block_size, AvailableSpace const& available_space, ContainingBlockConstraints const& containing_block_constraints) const
{
    auto intrinsic = intrinsic_size_for_replaced_sizing(box);
    // If 'height' and 'width' both have computed values of 'auto' and the element also has
    // an intrinsic height, then that intrinsic height is the used value of 'height'.
    if (should_treat_inline_size_as_auto(box, available_space) && should_treat_block_size_as_auto(box, available_space, containing_block_constraints) && intrinsic.has_height())
        return intrinsic.height.value();

    // Otherwise, if 'height' has a computed value of 'auto', and the element has an intrinsic ratio then the used value of 'height' is:
    //
    //     (used width) / (intrinsic ratio)
    if (computed_block_size.is_auto() && box.has_preferred_aspect_ratio())
        return content_block_size_from_aspect_ratio(box, m_state.get(box));

    // Otherwise, if 'height' has a computed value of 'auto', and the element has an intrinsic height, then that intrinsic height is the used value of 'height'.
    if (computed_block_size.is_auto() && intrinsic.has_height())
        return intrinsic.height.value();

    // Otherwise, if 'height' has a computed value of 'auto', but none of the conditions above are met,
    // then the used value of 'height' must be set to the height of the largest rectangle that has a 2:1 ratio, has a height not greater than 150px,
    // and has a width not greater than the device width.
    if (computed_block_size.is_auto())
        return 150;

    // FIXME: Handle cases when available_space is not definite.
    return calculate_inner_block_size(box, available_space, computed_block_size, containing_block_constraints);
}

CSSPixels FormattingContext::compute_block_size_for_replaced_element(Box const& box, AvailableSpace const& available_space, ContainingBlockConstraints const& containing_block_constraints) const
{
    // 10.6.2 Inline replaced elements
    // 10.6.4 Block-level replaced elements in normal flow
    // 10.6.6 Floating replaced elements
    // 10.6.10 'inline-block' replaced elements in normal flow

    auto computed_inline_size = should_treat_inline_size_as_auto(box, available_space) ? CSS::Size::make_auto() : box.computed_values().width();
    auto computed_block_size = should_treat_block_size_as_auto(box, available_space, containing_block_constraints) ? CSS::Size::make_auto() : box.computed_values().height();

    // 1. The tentative used height is calculated (without 'min-height' and 'max-height')
    CSSPixels used_block_size = tentative_block_size_for_replaced_element(box, computed_block_size, available_space, containing_block_constraints);

    // However, for replaced elements with both 'width' and 'height' computed as 'auto',
    // use the algorithm under 'Minimum and maximum widths'
    // https://www.w3.org/TR/CSS22/visudet.html#min-max-widths
    // to find the used width and height.
    if ((computed_inline_size.is_auto() && computed_block_size.is_auto() && box.has_preferred_aspect_ratio())) {
        // NOTE: This is a special case where calling tentative_inline_size_for_replaced_element() would call us right back,
        //       and we'd end up in an infinite loop. So we need to handle this case separately.
        if (auto intrinsic = intrinsic_size_for_replaced_sizing(box); intrinsic.has_width() || !intrinsic.has_height()) {
            CSSPixels inline_size = tentative_inline_size_for_replaced_element(box, computed_inline_size, available_space, containing_block_constraints);
            CSSPixels block_size = used_block_size;
            used_block_size = solve_replaced_size_constraint(inline_size, block_size, box, available_space, containing_block_constraints).block_size;
        }
    }
    // 2. If this tentative height is greater than 'max-height', the rules above are applied again,
    //    but this time using the value of 'max-height' as the computed value for 'height'.
    if (!should_treat_max_block_size_as_none(box, available_space.block_size, containing_block_constraints)) {
        auto const& computed_max_block_size = box.computed_values().max_height();
        auto max_block_size = calculate_inner_block_size(box, available_space, computed_max_block_size, containing_block_constraints);
        if (used_block_size > max_block_size)
            used_block_size = tentative_block_size_for_replaced_element(box, computed_max_block_size, available_space, containing_block_constraints);
    }

    // 3. If the resulting height is smaller than 'min-height', the rules above are applied again,
    //    but this time using the value of 'min-height' as the computed value for 'height'.
    auto computed_min_block_size = box.computed_values().min_height();
    if (!computed_min_block_size.is_auto()) {
        auto min_block_size = calculate_inner_block_size(box, available_space, computed_min_block_size, containing_block_constraints);
        if (used_block_size < min_block_size)
            used_block_size = tentative_block_size_for_replaced_element(box, computed_min_block_size, available_space, containing_block_constraints);
    }

    return used_block_size;
}

void FormattingContext::compute_inline_size_for_absolutely_positioned_non_replaced_element(Box const& box, AvailableSpace const& available_space, ContainingBlockConstraints const& containing_block_constraints, StaticPositionRect const& static_position_rect)
{
    auto containing_block_inline_size = available_space.inline_size.to_px_or_zero();
    auto const& computed_values = box.computed_values();
    auto& box_state = m_state.get_mutable(box);

    auto margin_left = CSS::LengthOrAuto::make_auto();
    auto margin_right = CSS::LengthOrAuto::make_auto();
    auto const border_left = computed_values.border_left().width;
    auto const border_right = computed_values.border_right().width;
    auto const padding_left = box_state.padding_left;
    auto const padding_right = box_state.padding_right;

    auto computed_left = computed_values.inset().left();
    auto computed_right = computed_values.inset().right();
    auto left = computed_values.inset().left().to_px_or_zero(containing_block_inline_size);
    auto right = computed_values.inset().right().to_px_or_zero(containing_block_inline_size);

    auto try_compute_inline_size = [&](CSS::LengthOrAuto const& input_inline_size) {
        margin_left = computed_values.margin().left().resolved_or_auto(containing_block_inline_size);
        margin_right = computed_values.margin().right().resolved_or_auto(containing_block_inline_size);

        auto inline_size = input_inline_size;

        auto solve_for_left = [&] {
            return containing_block_inline_size - margin_left.to_px_or_zero() - border_left - padding_left - inline_size.to_px_or_zero() - padding_right - border_right - margin_right.to_px_or_zero() - right;
        };

        auto solve_for_inline_size = [&] {
            return CSS::Length::make_px(max(CSSPixels(0), containing_block_inline_size - left - margin_left.to_px_or_zero() - border_left - padding_left - padding_right - border_right - margin_right.to_px_or_zero() - right));
        };

        auto solve_for_right = [&] {
            return containing_block_inline_size - left - margin_left.to_px_or_zero() - border_left - padding_left - inline_size.to_px_or_zero() - padding_right - border_right - margin_right.to_px_or_zero();
        };

        auto calculate_shrink_to_fit_inline_size = [&] {
            auto available_inline_size = solve_for_inline_size().to_px(box);
            auto preferred_inline_size = calculate_max_content_inline_size(box, containing_block_constraints);
            if (preferred_inline_size <= available_inline_size)
                return preferred_inline_size;
            auto preferred_minimum_inline_size = calculate_min_content_inline_size(box, containing_block_constraints);
            return min(max(preferred_minimum_inline_size, available_inline_size), preferred_inline_size);
        };

        // If all three of 'left', 'width', and 'right' are 'auto':
        if (computed_left.is_auto() && inline_size.is_auto() && computed_right.is_auto()) {
            // First set any 'auto' values for 'margin-left' and 'margin-right' to 0.
            if (margin_left.is_auto())
                margin_left = CSS::Length::make_px(0);
            if (margin_right.is_auto())
                margin_right = CSS::Length::make_px(0);
            // Then, if the 'direction' property of the element establishing the static-position containing block
            // is 'ltr' set 'left' to the static position and apply rule number three below;
            // otherwise, set 'right' to the static position and apply rule number one below.

            // NOTE: As with compute_block_size_for_absolutely_positioned_non_replaced_element, we actually apply these
            //       steps in the opposite order since the static position may depend on the width of the box.

            auto content_inline_size = calculate_shrink_to_fit_inline_size();
            inline_size = CSS::Length::make_px(content_inline_size);
            m_state.get_mutable(box).set_content_inline_size(content_inline_size);

            auto static_offset = aligned_static_offset(static_position_rect, box_state);

            left = static_offset.inline_offset;
            right = solve_for_right();
        }

        // If none of the three is auto:
        if (!computed_left.is_auto() && !inline_size.is_auto() && !computed_right.is_auto()) {
            // If both margin-left and margin-right are auto,
            // solve the equation under the extra constraint that the two margins get equal values
            // FIXME: unless this would make them negative, in which case when direction of the containing block is ltr (rtl), set margin-left (margin-right) to 0 and solve for margin-right (margin-left).
            auto inline_size_available_for_margins = containing_block_inline_size - border_left - padding_left - inline_size.to_px_or_zero() - padding_right - border_right - left - right;
            if (margin_left.is_auto() && margin_right.is_auto()) {
                margin_left = CSS::Length::make_px(inline_size_available_for_margins / 2);
                margin_right = CSS::Length::make_px(inline_size_available_for_margins / 2);
                return inline_size;
            }

            // If one of margin-left or margin-right is auto, solve the equation for that value.
            if (margin_left.is_auto()) {
                margin_left = CSS::Length::make_px(inline_size_available_for_margins);
                return inline_size;
            }
            if (margin_right.is_auto()) {
                margin_right = CSS::Length::make_px(inline_size_available_for_margins);
                return inline_size;
            }
            // If the values are over-constrained, ignore the value for left
            // (in case the direction property of the containing block is rtl)
            // or right (in case direction is ltr) and solve for that value.

            // NOTE: At this point we *are* over-constrained since none of margin-left, left, width, right, or margin-right are auto.
            // FIXME: Check direction.
            right = solve_for_right();
            return inline_size;
        }

        if (margin_left.is_auto())
            margin_left = CSS::Length::make_px(0);
        if (margin_right.is_auto())
            margin_right = CSS::Length::make_px(0);

        // 1. 'left' and 'width' are 'auto' and 'right' is not 'auto',
        //    then the width is shrink-to-fit. Then solve for 'left'
        if (computed_left.is_auto() && inline_size.is_auto() && !computed_right.is_auto()) {
            inline_size = CSS::Length::make_px(calculate_shrink_to_fit_inline_size());
            left = solve_for_left();
        }

        // 2. 'left' and 'right' are 'auto' and 'width' is not 'auto',
        //    then if the 'direction' property of the element establishing
        //    the static-position containing block is 'ltr' set 'left'
        //    to the static position, otherwise set 'right' to the static position.
        //    Then solve for 'left' (if 'direction is 'rtl') or 'right' (if 'direction' is 'ltr').
        else if (computed_left.is_auto() && computed_right.is_auto() && !inline_size.is_auto()) {
            // FIXME: Check direction
            auto static_offset = aligned_static_offset(static_position_rect, box_state);
            left = static_offset.inline_offset;
            right = solve_for_right();
        }

        // 3. 'width' and 'right' are 'auto' and 'left' is not 'auto',
        //    then the width is shrink-to-fit. Then solve for 'right'
        else if (inline_size.is_auto() && computed_right.is_auto() && !computed_left.is_auto()) {
            inline_size = CSS::Length::make_px(calculate_shrink_to_fit_inline_size());
            right = solve_for_right();
        }

        // 4. 'left' is 'auto', 'width' and 'right' are not 'auto', then solve for 'left'
        else if (computed_left.is_auto() && !inline_size.is_auto() && !computed_right.is_auto()) {
            left = solve_for_left();
        }

        // 5. 'width' is 'auto', 'left' and 'right' are not 'auto', then solve for 'width'
        else if (inline_size.is_auto() && !computed_left.is_auto() && !computed_right.is_auto()) {
            inline_size = solve_for_inline_size();
        }

        // 6. 'right' is 'auto', 'left' and 'width' are not 'auto', then solve for 'right'
        else if (computed_right.is_auto() && !computed_left.is_auto() && !inline_size.is_auto()) {
            right = solve_for_right();
        }

        return inline_size;
    };

    // 1. The tentative used width is calculated (without 'min-width' and 'max-width')
    auto used_inline_size = try_compute_inline_size([&] -> CSS::LengthOrAuto {
        if (is<TableWrapper>(box))
            return CSS::Length::make_px(compute_table_box_inline_size_inside_table_wrapper(box, available_space, containing_block_constraints));
        if (computed_values.width().is_auto())
            return CSS::LengthOrAuto::make_auto();
        return CSS::Length::make_px(calculate_inner_inline_size(box, available_space.inline_size, computed_values.width(), containing_block_constraints));
    }());

    // 2. The tentative used width is greater than 'max-width', the rules above are applied again,
    //    but this time using the computed value of 'max-width' as the computed value for 'width'.
    if (!should_treat_max_inline_size_as_none(box, available_space.inline_size, containing_block_constraints)) {
        auto max_inline_size = calculate_inner_inline_size(box, available_space.inline_size, computed_values.max_width(), containing_block_constraints);
        if (used_inline_size.to_px_or_zero() > max_inline_size) {
            used_inline_size = try_compute_inline_size(CSS::Length::make_px(max_inline_size));
        }
    }

    // 3. If the resulting width is smaller than 'min-width', the rules above are applied again,
    //    but this time using the value of 'min-width' as the computed value for 'width'.
    if (!computed_values.min_width().is_auto()) {
        auto min_inline_size = calculate_inner_inline_size(box, available_space.inline_size, computed_values.min_width(), containing_block_constraints);
        if (used_inline_size.to_px_or_zero() < min_inline_size) {
            used_inline_size = try_compute_inline_size(CSS::Length::make_px(min_inline_size));
        }
    }

    box_state.set_content_inline_size(used_inline_size.to_px_or_zero());
    box_state.inset_left = left;
    box_state.inset_right = right;
    box_state.margin_left = margin_left.to_px_or_zero();
    box_state.margin_right = margin_right.to_px_or_zero();
}

// https://drafts.csswg.org/css2/#abs-replaced-width
void FormattingContext::compute_inline_size_for_absolutely_positioned_replaced_element(Box const& box, AvailableSpace const& available_space, ContainingBlockConstraints const& containing_block_constraints, StaticPositionRect const& static_position_rect)
{
    // 10.3.8 Absolutely positioned, replaced elements
    // In this case, section 10.3.7 applies up through and including the constraint equation,
    // but the rest of section 10.3.7 is replaced by the following rules:

    // 1. The used value of 'width' is determined as for inline replaced elements.

    auto inline_size = compute_inline_size_for_replaced_element(box, available_space, containing_block_constraints);
    auto containing_block_inline_size = available_space.inline_size.to_px_or_zero();
    auto const& computed_values = box.computed_values();
    auto& box_state = m_state.get_mutable(box);
    auto const border_left = computed_values.border_left().width;
    auto const border_right = computed_values.border_right().width;
    auto const padding_left = box_state.padding_left;
    auto const padding_right = box_state.padding_right;
    auto available = containing_block_inline_size - inline_size - border_left - padding_left - padding_right - border_right;
    auto left = computed_values.inset().left();
    auto margin_left = computed_values.margin().left();
    auto right = computed_values.inset().right();
    auto margin_right = computed_values.margin().right();
    auto static_offset = aligned_static_offset(static_position_rect, box_state);

    auto to_px = [&](CSS::LengthPercentageOrAuto const& l) {
        return l.to_px_or_zero(containing_block_inline_size);
    };

    // If 'margin-left' or 'margin-right' is specified as 'auto' its used value is determined by the rules below.
    // 2. If both 'left' and 'right' have the value 'auto', then if the 'direction' property of the
    // element establishing the static-position containing block is 'ltr', set 'left' to the static
    // position; else if 'direction' is 'rtl', set 'right' to the static position.
    if (left.is_auto() && right.is_auto()) {
        left = CSS::Length::make_px(static_offset.inline_offset);
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

    box_state.inset_left = to_px(left);
    box_state.inset_right = to_px(right);
    box_state.margin_left = to_px(margin_left);
    box_state.margin_right = to_px(margin_right);
    box_state.set_content_inline_size(inline_size);
}

// https://drafts.csswg.org/css-position-3/#abs-non-replaced-height
void FormattingContext::compute_block_size_for_absolutely_positioned_non_replaced_element(Box const& box, AvailableSpace const& available_space, ContainingBlockConstraints const& containing_block_constraints, StaticPositionRect const& static_position_rect, BeforeOrAfterInsideLayout before_or_after_inside_layout)
{
    // 5.3. The Height Of Absolutely Positioned, Non-Replaced Elements

    // For absolutely positioned elements, the used values of the vertical dimensions must satisfy this constraint:
    // top + margin-top + border-top-width + padding-top + height + padding-bottom + border-bottom-width + margin-bottom + bottom = height of containing block

    // NOTE: This function is called twice: both before and after inside layout.
    //       In the before pass, if the box needs its automatic block size, abort these steps.
    //       This lets the box retain an indefinite block size from the perspective of inside layout.

    auto apply_min_max_block_size_constraints = [this, &box, &available_space, &containing_block_constraints](CSS::LengthOrAuto const& unconstrained_block_size) -> CSS::LengthOrAuto {
        auto const& computed_min_block_size = box.computed_values().min_height();
        auto const& computed_max_block_size = box.computed_values().max_height();
        auto constrained_block_size = unconstrained_block_size;
        if (!computed_max_block_size.is_none()) {
            auto inner_max_block_size = calculate_inner_block_size(box, available_space, computed_max_block_size, containing_block_constraints);
            if (inner_max_block_size < constrained_block_size.to_px_or_zero())
                constrained_block_size = CSS::Length::make_px(inner_max_block_size);
        }
        if (!computed_min_block_size.is_auto()) {
            auto inner_min_block_size = calculate_inner_block_size(box, available_space, computed_min_block_size, containing_block_constraints);
            if (inner_min_block_size > constrained_block_size.to_px_or_zero())
                constrained_block_size = CSS::Length::make_px(inner_min_block_size);
        }
        return constrained_block_size;
    };

    auto margin_top = box.computed_values().margin().top();
    auto margin_bottom = box.computed_values().margin().bottom();
    auto top = box.computed_values().inset().top();
    auto bottom = box.computed_values().inset().bottom();

    auto containing_block_inline_size = available_space.inline_size.to_px_or_zero();
    auto containing_block_block_size = available_space.block_size.to_px_or_zero();

    enum class ClampToZero {
        No,
        Yes,
    };

    auto& state = m_state.get(box);
    auto try_compute_block_size = [&](CSS::LengthOrAuto block_size) -> CSS::LengthOrAuto {
        // Reset values that may have been modified by a previous call (when re-solving for min/max-height).
        margin_top = box.computed_values().margin().top();
        margin_bottom = box.computed_values().margin().bottom();
        top = box.computed_values().inset().top();
        bottom = box.computed_values().inset().bottom();

        auto solve_for = [&](CSS::LengthOrAuto const& length_or_auto, ClampToZero clamp_to_zero = ClampToZero::No) {
            auto unclamped_value = containing_block_block_size
                - top.to_px_or_zero(containing_block_block_size)
                - margin_top.to_px_or_zero(containing_block_inline_size)
                - box.computed_values().border_top().width
                - state.padding_top
                - block_size.to_px_or_zero()
                - state.padding_bottom
                - box.computed_values().border_bottom().width
                - margin_bottom.to_px_or_zero(containing_block_inline_size)
                - bottom.to_px_or_zero(containing_block_block_size)
                + length_or_auto.to_px_or_zero();
            if (clamp_to_zero == ClampToZero::Yes)
                return CSS::Length::make_px(max(CSSPixels(0), unclamped_value));
            return CSS::Length::make_px(unclamped_value);
        };

        auto solve_for_top = [&] {
            top = solve_for(top.resolved_or_auto(containing_block_block_size));
        };

        auto solve_for_bottom = [&] {
            bottom = solve_for(bottom.resolved_or_auto(containing_block_block_size));
        };

        auto solve_for_block_size = [&] {
            block_size = solve_for(block_size, ClampToZero::Yes);
        };

        auto solve_for_margin_top = [&] {
            margin_top = solve_for(margin_top.resolved_or_auto(containing_block_inline_size));
        };

        auto solve_for_margin_bottom = [&] {
            margin_bottom = solve_for(margin_bottom.resolved_or_auto(containing_block_inline_size));
        };

        auto solve_for_margin_top_and_margin_bottom = [&] {
            auto remainder = solve_for(CSS::Length::make_px(margin_top.to_px_or_zero(containing_block_inline_size) + margin_bottom.to_px_or_zero(containing_block_inline_size))).to_px(box);
            margin_top = CSS::Length::make_px(remainder / 2);
            margin_bottom = CSS::Length::make_px(remainder / 2);
        };

        // If all three of top, height, and bottom are auto:
        if (top.is_auto() && block_size.is_auto() && bottom.is_auto()) {
            // First set any auto values for margin-top and margin-bottom to 0,
            if (margin_top.is_auto())
                margin_top = CSS::Length::make_px(0);
            if (margin_bottom.is_auto())
                margin_bottom = CSS::Length::make_px(0);

            // then set top to the static position,
            // and finally apply rule number three below.

            // NOTE: We actually perform these two steps in the opposite order,
            //       because the static position may depend on the box's block size due to alignment properties.

            auto maybe_block_size = compute_automatic_block_size_for_absolutely_positioned_element(box, available_space, containing_block_constraints, before_or_after_inside_layout);
            if (!maybe_block_size.has_value())
                return block_size;
            block_size = CSS::Length::make_px(maybe_block_size.value());

            auto constrained_block_size = apply_min_max_block_size_constraints(block_size);
            m_state.get_mutable(box).set_content_block_size(constrained_block_size.to_px_or_zero());

            auto static_offset = aligned_static_offset(static_position_rect, state);
            top = CSS::Length::make_px(static_offset.block_offset);

            solve_for_bottom();
        }

        // If none of the three are auto:
        else if (!top.is_auto() && !block_size.is_auto() && !bottom.is_auto()) {
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
            if (top.is_auto() && block_size.is_auto() && !bottom.is_auto()) {
                // then the height is based on the Auto heights for block formatting context roots,
                auto maybe_block_size = compute_automatic_block_size_for_absolutely_positioned_element(box, available_space, containing_block_constraints, before_or_after_inside_layout);
                if (!maybe_block_size.has_value())
                    return block_size;
                block_size = CSS::Length::make_px(maybe_block_size.value());

                // and solve for top.
                solve_for_top();
            }

            // 2. If top and bottom are auto and height is not auto,
            else if (top.is_auto() && bottom.is_auto() && !block_size.is_auto()) {
                // then set top to the static position,
                top = CSS::Length::make_px(aligned_static_offset(static_position_rect, state).block_offset);

                // then solve for bottom.
                solve_for_bottom();
            }

            // 3. If height and bottom are auto and top is not auto,
            else if (block_size.is_auto() && bottom.is_auto() && !top.is_auto()) {
                // then the height is based on the Auto heights for block formatting context roots,
                auto maybe_block_size = compute_automatic_block_size_for_absolutely_positioned_element(box, available_space, containing_block_constraints, before_or_after_inside_layout);
                if (!maybe_block_size.has_value())
                    return block_size;
                block_size = CSS::Length::make_px(maybe_block_size.value());

                // and solve for bottom.
                solve_for_bottom();
            }

            // 4. If top is auto, height and bottom are not auto,
            else if (top.is_auto() && !block_size.is_auto() && !bottom.is_auto()) {
                // then solve for top.
                solve_for_top();
            }

            // 5. If height is auto, top and bottom are not auto,
            else if (block_size.is_auto() && !top.is_auto() && !bottom.is_auto()) {
                // then solve for height.
                solve_for_block_size();
            }

            // 6. If bottom is auto, top and height are not auto,
            else if (bottom.is_auto() && !top.is_auto() && !block_size.is_auto()) {
                // then solve for bottom.
                solve_for_bottom();
            }
        }

        return block_size;
    };

    // Intrinsic block sizes depend on the box's own used inline size, which was already resolved above, not on the
    // inline size of the containing block. Absolutely positioned boxes routinely have a narrower used inline size
    // than their containing block, and measuring their content at the containing block inline size can prevent text
    // from wrapping and underestimate the block size. Feed the box's own inline size to the intrinsic calculation.
    auto available_space_for_intrinsic_block_size = available_space;
    available_space_for_intrinsic_block_size.inline_size = AvailableSize::make_definite(state.content_inline_size());

    // Compute the block size based on box type and CSS properties:
    // https://www.w3.org/TR/css-sizing-3/#box-sizing
    auto used_block_size = try_compute_block_size([&] -> CSS::LengthOrAuto {
        if (is<TableWrapper>(box))
            return CSS::Length::make_px(compute_table_box_block_size_inside_table_wrapper(box, available_space, containing_block_constraints));
        if (should_treat_block_size_as_auto(box, available_space, containing_block_constraints))
            return CSS::LengthOrAuto::make_auto();
        return CSS::Length::make_px(calculate_inner_block_size(box, available_space_for_intrinsic_block_size, box.computed_values().height(), containing_block_constraints));
    }());

    // If the tentative used height is greater than 'max-height', the rules above are applied again,
    // but this time using the computed value of 'max-height' as the computed value for 'height'.
    auto const& computed_max_block_size = box.computed_values().max_height();
    if (!used_block_size.is_auto() && !computed_max_block_size.is_none()) {
        auto max_block_size = calculate_inner_block_size(box, available_space_for_intrinsic_block_size, computed_max_block_size, containing_block_constraints);
        if (used_block_size.to_px_or_zero() > max_block_size)
            used_block_size = try_compute_block_size(CSS::Length::make_px(max_block_size));
    }

    // If the resulting height is smaller than 'min-height', the rules above are applied again,
    // but this time using the value of 'min-height' as the computed value for 'height'.
    auto const& computed_min_block_size = box.computed_values().min_height();
    if (!used_block_size.is_auto() && !computed_min_block_size.is_auto()) {
        auto min_block_size = calculate_inner_block_size(box, available_space_for_intrinsic_block_size, computed_min_block_size, containing_block_constraints);
        if (used_block_size.to_px_or_zero() < min_block_size)
            used_block_size = try_compute_block_size(CSS::Length::make_px(min_block_size));
    }

    // For the before-inside-layout pass where the block size is still auto, apply min-max as a simple clamp.
    if (used_block_size.is_auto())
        used_block_size = apply_min_max_block_size_constraints(used_block_size);

    // NOTE: The following is not directly part of any spec, but this is where we resolve
    //       the final used values for vertical margin/border/padding.

    auto& box_state = m_state.get_mutable(box);
    box_state.set_content_block_size(used_block_size.to_px_or_zero());

    // do not set calculated insets or margins on the first pass, there will be a second pass
    if (box.computed_values().height().is_auto() && before_or_after_inside_layout == BeforeOrAfterInsideLayout::Before)
        return;
    if (computed_block_size_establishes_definite_containing_block_size(box.computed_values().height()))
        box_state.set_has_definite_block_size(true);
    box_state.inset_top = top.to_px_or_zero(containing_block_block_size);
    box_state.inset_bottom = bottom.to_px_or_zero(containing_block_block_size);
    box_state.margin_top = margin_top.to_px_or_zero(containing_block_inline_size);
    box_state.margin_bottom = margin_bottom.to_px_or_zero(containing_block_inline_size);
}

// FIXME: Containing block handling for absolutely positioned elements needs architectural improvements.
//
//        The CSS specification defines the containing block as a *rectangle*, not a box. For most cases,
//        this rectangle is derived from the padding box of the nearest positioned ancestor Box. However,
//        when the positioned ancestor is an *inline* element (e.g., a <span> with position: relative),
//        the containing block rectangle should be the bounding box of that inline's fragments.
//
//        Currently, Layout::Node::m_containing_block is typed as Layout::Box*, which cannot represent
//        inline elements. The proper fix would be to:
//        1. Separate the concept of "the node that establishes the containing block" from "the containing
//           block rectangle".
//        2. Store a reference to the establishing node (which could be InlineNode or Box).
//        3. Compute the containing block rectangle on demand based on the establishing node's type.
//
//        For now, we use a surgical workaround: when laying out an absolutely positioned element, we check
//        if there's an inline element with position:relative (or other containing-block-establishing
//        properties) between the abspos element and its current containing_block(). If found, we compute
//        the inline's fragment bounding box and use that for sizing and positioning, then adjust the final
//        offset to be relative to the containing_block() Box that the rest of the system expects.

// Computes the bounding box rectangle of an inline node's fragments, in the coordinate
// space of the abspos containing block. Returns the padding-box rect because that's the
// edge containing blocks are formed by.
//
// When an inline element has block-level descendants, the layout tree splits the inline
// into "before"/"middle"/"after" anonymous wrappers; we walk all of them so the rect
// covers the inline's full extent (matching getClientRects() for split inlines).
static Optional<CSSPixelRect> compute_inline_containing_block_rect(InlineNode const& inline_node, Box const& abspos_containing_block, LayoutState const& state)
{
    auto const* inline_dom_node = inline_node.dom_node();
    if (!inline_dom_node)
        return {};

    auto const* outer_block = inline_node.non_anonymous_containing_block();
    if (!outer_block)
        return {};

    Optional<CSSPixelRect> bounding_rect;
    Optional<CSSPixelRect> empty_bounding_rect;
    auto union_rect = [](Optional<CSSPixelRect>& destination, CSSPixelRect const& rect) {
        if (!destination.has_value()) {
            destination = rect;
            return;
        }
        auto left = min(destination->left(), rect.left());
        auto top = min(destination->top(), rect.top());
        auto right = max(destination->right(), rect.right());
        auto bottom = max(destination->bottom(), rect.bottom());
        destination = CSSPixelRect { left, top, right - left, bottom - top };
    };
    auto add_fragment_rect = [&](CSSPixelRect const& rect) {
        if (rect.is_empty()) {
            union_rect(empty_bounding_rect, rect);
            return;
        }
        union_rect(bounding_rect, rect);
    };
    auto make_physical_rect = [](CSS::WritingMode writing_mode, CSSPixels inline_start, CSSPixels block_start, CSSPixels inline_size, CSSPixels block_size) {
        if (writing_mode == CSS::WritingMode::HorizontalTb)
            return CSSPixelRect { { inline_start, block_start }, { inline_size, block_size } };
        return CSSPixelRect { { block_start, inline_start }, { block_size, inline_size } };
    };
    // The fragment belongs to the line boxes of the box we're currently walking, so `offset`
    // (that box's running offset from abspos_containing_block) is the fragment's coordinate
    // space origin; no walk up the containing block chain is needed.
    auto add_atomic_inline_fragment_rect = [&](LineBoxFragment const& fragment, CSSPixelPoint offset) {
        auto const* child_used_values = state.try_get(fragment.layout_node());
        if (!child_used_values)
            return;

        auto const writing_mode = fragment.writing_mode();
        auto const is_horizontal = writing_mode == CSS::WritingMode::HorizontalTb;
        auto const inline_axis_border_box_start = fragment.inline_offset() - (is_horizontal ? child_used_values->border_box_left() : child_used_values->border_box_top());
        auto const inline_axis_border_box_extent = is_horizontal
            ? child_used_values->border_box_inline_size()
            : child_used_values->border_box_block_size();
        auto const block_axis_line_height = inline_node.computed_values().line_height();
        auto const block_axis_start = [&] {
            if (fragment.style_source().computed_values().block_axis_is_reverse())
                return fragment.block_offset() + child_used_values->border_box_right() - block_axis_line_height;
            return fragment.block_offset() - (is_horizontal ? child_used_values->border_box_top() : child_used_values->border_box_left());
        }();
        add_fragment_rect(make_physical_rect(writing_mode,
            inline_axis_border_box_start,
            block_axis_start,
            inline_axis_border_box_extent,
            block_axis_line_height)
                .translated(offset));
    };

    // Walk outer_block's subtree in pre-order, threading the running offset from
    // abspos_containing_block down so we don't have to re-walk to the root for every
    // matching node. We prune subtrees rooted at non-anonymous boxes that don't belong
    // to the inline (sibling content can't contribute to its rect), and skip out-of-flow
    // descendants (they aren't part of the inline's fragments).
    auto walk = [&](this auto& self, Node const& node, CSSPixelPoint offset) -> void {
        auto const* used_values = state.try_get(node);
        if (used_values && !used_values->line_boxes.is_empty()) {
            for (auto const& line_box : used_values->line_boxes) {
                for (auto const& fragment : line_box.fragments()) {
                    auto const* dom = fragment.layout_node().dom_node();
                    if (!dom || !inline_dom_node->is_inclusive_ancestor_of(*dom))
                        continue;
                    if (fragment.is_atomic_inline()) {
                        add_atomic_inline_fragment_rect(fragment, offset);
                        continue;
                    }
                    add_fragment_rect({ fragment.offset() + offset, fragment.size() });
                }
            }
        }

        for (auto child = node.first_child(); child; child = child->next_sibling()) {
            auto const* child_with_style = as_if<NodeWithStyle>(*child);
            if (child_with_style && (child_with_style->is_absolutely_positioned() || child_with_style->is_floating()))
                continue;
            auto const* child_used_values = state.try_get(*child);
            auto child_offset = child_used_values ? offset + child_used_values->content_offset() : offset;
            auto const* box_child = as_if<Box>(child.ptr());
            if (box_child && !box_child->is_anonymous()) {
                auto const* dom = box_child->dom_node();
                if (!dom || !inline_dom_node->is_inclusive_ancestor_of(*dom))
                    continue;
                // Atomic inlines contribute their fragment rect via their containing block's
                // line boxes above; don't descend into their independent subtree.
                if (box_child->is_atomic_inline())
                    continue;
                // child_offset addresses the box's content area; the border-box origin sits
                // (border-left + padding-left, border-top + padding-top) before that.
                if (child_used_values) {
                    auto const border_box_origin = child_offset - CSSPixelPoint {
                        child_used_values->border_left + child_used_values->padding_left,
                        child_used_values->border_top + child_used_values->padding_top,
                    };
                    add_fragment_rect({ border_box_origin, { child_used_values->border_box_inline_size(), child_used_values->border_box_block_size() } });
                }
            }
            self(*child, child_offset);
        }
    };

    CSSPixelPoint outer_offset;
    for (Node const* ancestor = outer_block; ancestor && ancestor != &abspos_containing_block; ancestor = ancestor->parent()) {
        if (auto const* used_values = state.try_get(*ancestor))
            outer_offset.translate_by(used_values->content_offset());
    }
    walk(*outer_block, outer_offset);

    if (!bounding_rect.has_value())
        bounding_rect = empty_bounding_rect;
    if (!bounding_rect.has_value())
        return {};

    // Expand the bounding rect by the inline's padding to get the padding box.
    if (auto const* inline_used_values = state.try_get(inline_node)) {
        bounding_rect->inflate(
            inline_used_values->padding_top,
            inline_used_values->padding_right,
            inline_used_values->padding_bottom,
            inline_used_values->padding_left);
    }

    return bounding_rect;
}

struct AbsposAxisModes {
    AbsposAxisMode inline_axis;
    AbsposAxisMode block_axis;
};

// Per-axis mode: auto+auto insets -> static position, otherwise -> inset from rect.
static AbsposAxisModes abspos_axis_modes_from_computed_insets(CSS::ComputedValues const& computed_values)
{
    auto const& inset = computed_values.inset();
    return {
        .inline_axis = inset.left().is_auto() && inset.right().is_auto() ? AbsposAxisMode::StaticPosition : AbsposAxisMode::InsetFromRect,
        .block_axis = inset.top().is_auto() && inset.bottom().is_auto() ? AbsposAxisMode::StaticPosition : AbsposAxisMode::InsetFromRect,
    };
}

AbsposContainingBlockInfo FormattingContext::resolve_abspos_containing_block_info(Box const& box)
{
    auto const& computed_values = box.computed_values();
    auto [inline_axis_mode, block_axis_mode] = abspos_axis_modes_from_computed_insets(computed_values);

    // Check if there's an inline element that should be the real containing block.
    auto inline_containing_block = box.inline_containing_block_if_applicable();
    if (inline_containing_block && box.containing_block()) {
        auto rect = compute_inline_containing_block_rect(*inline_containing_block, *box.containing_block(), m_state);
        if (rect.has_value()) {
            return {
                {
                    { rect->x(), rect->y() },
                    { rect->width(), rect->height() },
                },
                inline_axis_mode,
                block_axis_mode,
                {},
                {},
            };
        }
    }

    // Normal case: padding box of the actual containing block.
    VERIFY(box.containing_block());
    auto& containing_block_state = m_state.get(*box.containing_block());
    LogicalRect rect {
        { -containing_block_state.padding_left, -containing_block_state.padding_top },
        {
            containing_block_state.content_inline_size() + containing_block_state.padding_left + containing_block_state.padding_right,
            containing_block_state.content_block_size() + containing_block_state.padding_top + containing_block_state.padding_bottom,
        },
    };
    return { rect, inline_axis_mode, block_axis_mode, {}, {} };
}

static bool style_value_contains_anchor(CSS::StyleValue const& value)
{
    if (value.is_anchor())
        return true;
    if (value.is_calculated())
        return value.as_calculated().contains_anchor_function();
    return false;
}

// NB: Generated boxes for pseudo-elements are anonymous, so their computed values live in
//     the generator element under the relevant pseudo-element rather than on a DOM node of
//     their own.
static Optional<DOM::AbstractElement> abstract_element_for_box(Box const& box)
{
    if (box.is_generated_for_pseudo_element())
        return DOM::AbstractElement { *box.pseudo_element_generator(), box.generated_for_pseudo_element() };
    if (auto const* element = as_if<DOM::Element>(box.dom_node()))
        return DOM::AbstractElement { *element };
    return {};
}

bool FormattingContext::box_inset_properties_contain_anchor_functions(Box const& box)
{
    auto abstract_element = abstract_element_for_box(box);
    if (!abstract_element.has_value())
        return false;

    auto const* computed = abstract_element->computed_values();
    if (!computed)
        return false;
    // Anchor functions in insets only survive to used-value time inside calculated values, so
    // when no inset is calculated (the common case), skip reconstructing the style values.
    auto const& inset = computed->inset();
    if (!inset.top().is_calculated() && !inset.right().is_calculated() && !inset.bottom().is_calculated() && !inset.left().is_calculated())
        return false;

    auto top = computed->computed_style_value(CSS::PropertyID::Top);
    auto right = computed->computed_style_value(CSS::PropertyID::Right);
    auto bottom = computed->computed_style_value(CSS::PropertyID::Bottom);
    auto left = computed->computed_style_value(CSS::PropertyID::Left);
    VERIFY(top && right && bottom && left);
    return style_value_contains_anchor(*top)
        || style_value_contains_anchor(*right)
        || style_value_contains_anchor(*bottom)
        || style_value_contains_anchor(*left);
}

static Box const* nearest_scroll_container_ancestor(Box const& box)
{
    for (auto const* ancestor = box.containing_block(); ancestor; ancestor = ancestor->containing_block()) {
        if (ancestor->is_scroll_container())
            return ancestor;
    }
    return nullptr;
}

namespace {

template<typename ResolveAnchorSide, typename NoteResolvedAnchorFunction>
class AnchorInsetResolver final : public CSS::AnchorResolver {
public:
    AnchorInsetResolver(ResolveAnchorSide const& resolve_anchor_side, NoteResolvedAnchorFunction const& note_resolved_anchor_function, bool box_is_absolutely_positioned, bool is_from_end, bool is_horizontal_axis, CSSPixels containing_block_extent)
        : m_resolve_anchor_side(resolve_anchor_side)
        , m_note_resolved_anchor_function(note_resolved_anchor_function)
        , m_box_is_absolutely_positioned(box_is_absolutely_positioned)
        , m_is_from_end(is_from_end)
        , m_is_horizontal_axis(is_horizontal_axis)
        , m_containing_block_extent(containing_block_extent)
    {
    }

    virtual Optional<CSSPixels> resolve(CSS::AnchorStyleValue const& anchor) const override
    {
        if (!m_box_is_absolutely_positioned)
            return {};

        auto side_px = m_resolve_anchor_side(anchor, m_is_from_end, m_is_horizontal_axis);
        if (!side_px.has_value())
            return {};

        m_note_resolved_anchor_function(anchor, m_is_horizontal_axis);

        // For inset properties measuring from the end edge (right, bottom), the resolved length is the distance from
        // the anchor side to the corresponding edge of the containing block's padding box.
        if (m_is_from_end)
            return m_containing_block_extent - side_px.value();
        return side_px.value();
    }

private:
    ResolveAnchorSide const& m_resolve_anchor_side;
    NoteResolvedAnchorFunction const& m_note_resolved_anchor_function;
    bool m_box_is_absolutely_positioned { false };
    bool m_is_from_end { false };
    bool m_is_horizontal_axis { false };
    CSSPixels m_containing_block_extent { 0 };
};

}

// https://drafts.csswg.org/css-anchor-position-1/#anchor-pos
void FormattingContext::resolve_anchor_insets(Box& box) const
{
    // https://drafts.csswg.org/css-anchor-position-1/#resolving-anchor
    // An anchor() function is a resolvable anchor function only if all the following conditions are true:
    //   - It's applied to an absolutely positioned box.
    //   - If its <anchor-side> specifies a physical keyword, it's specified in an inset property applicable to that
    //     axis.
    //   - There is a target anchor element for the box it's used on, and the <anchor-name> value specified in the
    //     function.
    // NB: The first two conditions are guaranteed: this function is only called for absolutely positioned boxes,
    //     and we only resolve anchor() values in inset properties.
    // FIXME: Support anchor-scope, position-try-fallbacks, anchor-size(), and other anchor positioning features.

    // Cleared up front so a box that is no longer anchored does not retain a stale default scroll shift from a
    // previous layout.
    box.set_default_scroll_shift({}, false, false);

    auto abstract_element = abstract_element_for_box(box);
    if (!abstract_element.has_value())
        return;

    auto const* computed = abstract_element->computed_values();
    if (!computed)
        return;
    auto top = computed->computed_style_value(CSS::PropertyID::Top);
    auto right = computed->computed_style_value(CSS::PropertyID::Right);
    auto bottom = computed->computed_style_value(CSS::PropertyID::Bottom);
    auto left = computed->computed_style_value(CSS::PropertyID::Left);
    VERIFY(top && right && bottom && left);

    bool top_contains_anchor = style_value_contains_anchor(*top);
    bool right_contains_anchor = style_value_contains_anchor(*right);
    bool bottom_contains_anchor = style_value_contains_anchor(*bottom);
    bool left_contains_anchor = style_value_contains_anchor(*left);
    if (!top_contains_anchor && !right_contains_anchor && !bottom_contains_anchor && !left_contains_anchor)
        return;

    auto containing_block = box.containing_block();
    if (!containing_block)
        return;

    auto const& default_anchor_name = box.computed_values().position_anchor();
    auto const& containing_block_state = m_state.get(*containing_block);

    // https://drafts.csswg.org/css-anchor-position-1/#acceptable-anchor-element
    // An element possible anchor is an acceptable anchor element for an absolutely positioned element positioned el
    // if all of the following are true:
    //   - possible anchor is either an element or a fully styleable pseudo-elements.
    //   - possible anchor is in scope for positioned el, per the effects of anchor-scope on possible anchor or its
    //     ancestors.
    //   - possible anchor is laid out strictly before positioned el, aka one of the following is true:
    //     - possible anchor and positioned el have the same original containing block and either
    //       - possible anchor is in a lower top layer than positioned el, or
    //       - they both exist in the same top layer, but possible anchor is either not absolutely positioned or
    //         occurs earlier in the flat tree order than positioned el
    //     - The element generating possible anchor’s containing block (if one exists) is an acceptable anchor
    //       element for positioned el
    //   - If possible anchor is in the skipped contents of another element, then positioned el is in the skipped
    //     contents of that same element.
    // NB: Applied recursively, the "laid out strictly before" conditions require the anchor's containing block
    //     chain to pass through positioned el's containing block. We implement that scoping, and approximate the
    //     ordering conditions by only accepting anchors that already have layout results.
    // FIXME: Implement the top layer conditions, the flat tree ordering between absolutely positioned boxes,
    //        anchor-scope, and the skipped contents condition.
    Function<bool(DOM::Element&)> is_acceptable_anchor_element = [&](DOM::Element& candidate) {
        // NB: We use unsafe_layout_node() because we are in the middle of layout.
        auto const* anchor_box = as_if<Box>(candidate.unsafe_layout_node());
        if (!anchor_box || anchor_box == &box)
            return false;

        // The anchor element may not have been laid out (it must be laid out strictly before
        // the positioned element to be an acceptable anchor).
        if (!m_state.try_get(*anchor_box))
            return false;

        for (auto const* ancestor = anchor_box->containing_block(); ancestor; ancestor = ancestor->containing_block()) {
            if (ancestor == containing_block)
                return true;
        }
        return false;
    };

    // https://drafts.csswg.org/css-anchor-position-1/#target
    // Otherwise, anchor spec is a <dashed-ident>. If an ancestor of query el satisfies the following conditions,
    // return the nearest such element to query el. Otherwise, return the last element el in tree order that
    // satisfies the conditions.
    //   - el is an anchor element with an anchor name of anchor spec.
    //   - el’s anchor name loosely matches anchor spec.
    //   - el is an acceptable anchor element for query el.
    // If no element satisfies these conditions, return nothing.
    // FIXME: Prefer the nearest ancestor of query el that satisfies the conditions over the last element in tree
    //        order.
    auto target_anchor_box = [&](Utf16FlyString const& anchor_name) -> Box* {
        auto anchor_element = abstract_element->element().document().element_by_anchor_name(anchor_name, abstract_element->element(), is_acceptable_anchor_element);
        if (!anchor_element)
            return nullptr;
        return as_if<Box>(anchor_element->unsafe_layout_node());
    };

    // https://drafts.csswg.org/css-anchor-position-1/#determining
    // Several features of this specification refer to the position and size of an anchor box. Unless otherwise
    // specified, this refers to the border box edge of the principal box of relevant anchor element.
    // FIXME: Implement remembered scroll offsets. Anchor references are currently always resolved as if all scroll
    //        containers were at their initial scroll position.
    auto resolve_anchor_rect = [&](CSS::AnchorStyleValue const& anchor) -> Optional<CSSPixelRect> {
        auto name = anchor.anchor_name();
        if (!name.has_value() && default_anchor_name.has_value())
            name = default_anchor_name.value();
        if (!name.has_value())
            return {};

        auto const* anchor_box = target_anchor_box(*name);
        if (!anchor_box)
            return {};

        auto const& anchor_state = m_state.get(*anchor_box);
        // Accumulate offsets from the anchor only up to the containing block: acceptability guarantees the
        // containing block is on the anchor's containing block chain, and boxes above it must not be read
        // because they may not be placed yet while this formatting context is still laying out.
        CSSPixelPoint anchor_offset_from_containing_block;
        for (auto const* node = anchor_box; node != containing_block; node = node->containing_block()) {
            VERIFY(node);
            anchor_offset_from_containing_block += m_state.get(*node).content_offset();
        }
        // The anchor rect is expressed relative to the containing block's padding box.
        auto anchor_border_box_origin = anchor_offset_from_containing_block
            - CSSPixelPoint { anchor_state.border_box_left(), anchor_state.border_box_top() }
            + CSSPixelPoint { containing_block_state.padding_left, containing_block_state.padding_top };
        return CSSPixelRect {
            anchor_border_box_origin,
            { anchor_state.border_box_inline_size(), anchor_state.border_box_block_size() },
        };
    };

    // https://drafts.csswg.org/css-anchor-position-1/#anchor-pos
    // An anchor() function representing a resolvable anchor function resolves at computed value time (using style &
    // layout interleaving) to the <length> that would align the edge of the positioned boxes' inset-modified containing
    // block corresponding to the property the function appears in with the specified edge of the target anchor
    // element's anchor box.
    auto containing_block_direction = containing_block->computed_values().direction();
    auto box_direction = box.computed_values().direction();
    auto resolve_anchor_side = [&](CSS::AnchorStyleValue const& anchor, bool is_from_end, bool is_horizontal_axis)
        -> Optional<CSSPixels> {
        auto maybe_rect = resolve_anchor_rect(anchor);
        if (!maybe_rect.has_value())
            return {};
        auto const& rect = maybe_rect.value();
        auto const& side = *anchor.anchor_side();
        if (side.is_keyword()) {
            switch (side.to_keyword()) {
            // https://drafts.csswg.org/css-anchor-position-1/#typedef-anchor-side
            // top | right | bottom | left
            //     Refers to the specified side of the anchor box.
            // If its <anchor-side> specifies a physical keyword, it's specified in an inset property applicable to that
            // axis.
            case CSS::Keyword::Top:
                return is_horizontal_axis ? Optional<CSSPixels> {} : rect.top();
            case CSS::Keyword::Bottom:
                return is_horizontal_axis ? Optional<CSSPixels> {} : rect.bottom();
            case CSS::Keyword::Left:
                return is_horizontal_axis ? rect.left() : Optional<CSSPixels> {};
            case CSS::Keyword::Right:
                return is_horizontal_axis ? rect.right() : Optional<CSSPixels> {};

            // center
            //     Equivalent to 50%.
            case CSS::Keyword::Center:
                if (is_horizontal_axis)
                    return rect.left() + rect.width() / 2;
                return rect.top() + rect.height() / 2;

            // start | end
            //     Refers to one of the sides of the anchor box in the same axis as the inset property it's used in,
            //     by resolving the keyword against the writing mode of the positioned box's containing block.
            case CSS::Keyword::Start:
            case CSS::Keyword::End: {
                bool is_start = side.to_keyword() == CSS::Keyword::Start;
                if (is_horizontal_axis) {
                    bool use_left = (containing_block_direction == CSS::Direction::Ltr) == is_start;
                    return use_left ? rect.left() : rect.right();
                }
                return is_start ? rect.top() : rect.bottom();
            }

            // self-start | self-end
            //     Refers to one of the sides of the anchor box in the same axis as the inset property it's used in,
            //     by resolving the keyword against the writing mode of the positioned box.
            case CSS::Keyword::SelfStart:
            case CSS::Keyword::SelfEnd: {
                bool is_start = side.to_keyword() == CSS::Keyword::SelfStart;
                if (is_horizontal_axis) {
                    bool use_left = (box_direction == CSS::Direction::Ltr) == is_start;
                    return use_left ? rect.left() : rect.right();
                }
                return is_start ? rect.top() : rect.bottom();
            }

            // inside | outside
            //     Resolves to one of the anchor box's sides, depending on which inset property it's used in. inside
            //     refers to the same side as the inset property, while outside refers to the opposite.
            case CSS::Keyword::Inside:
            case CSS::Keyword::Outside: {
                bool same_side = side.to_keyword() == CSS::Keyword::Inside;
                if (is_horizontal_axis) {
                    return (is_from_end == same_side) ? rect.right() : rect.left();
                }
                return (is_from_end == same_side) ? rect.bottom() : rect.top();
            }

            default:
                VERIFY_NOT_REACHED();
            }
        }
        if (side.is_percentage()) {
            // <percentage>
            //     Refers to a position a corresponding percentage between the start and end sides, with 0% being
            //     equivalent to start and 100% being equivalent to end.
            auto percentage = side.as_percentage().percentage().as_fraction();
            if (is_horizontal_axis) {
                auto start = containing_block_direction == CSS::Direction::Ltr ? rect.left() : rect.right();
                auto end = containing_block_direction == CSS::Direction::Ltr ? rect.right() : rect.left();
                return start + CSSPixels::nearest_value_for((end - start).to_double() * percentage);
            }
            return rect.top() + CSSPixels::nearest_value_for(rect.height().to_double() * percentage);
        }
        return {};
    };

    auto* default_anchor_box = default_anchor_name.has_value() ? target_anchor_box(default_anchor_name.value()) : nullptr;
    bool compensates_for_horizontal_scroll = false;
    bool compensates_for_vertical_scroll = false;

    // https://drafts.csswg.org/css-anchor-position-1/#compensate-for-scroll
    // An absolutely positioned box abspos compensates for scroll in the horizontal or vertical axis if both of the
    // following conditions are true:
    // - abspos has a default anchor box.
    // - abspos has an anchor reference to its default anchor box or at least to something in the same scrolling
    //   context, aka at least one of:
    //   - abspos's used self-alignment property value in that axis is anchor-center;
    //   - abspos has a non-none value for position-area
    //   - at least one anchor() function on abspos's used inset properties in the axis refers to a target anchor
    //     element with the same nearest scroll container ancestor with that axis as a scrollable axis as abspos's
    //     default anchor box.
    // FIXME: Account for anchor-center and position-area once they are supported.
    // AD-HOC: Like Blink and WebKit, we do not require the axis to be a scrollable axis of the shared scroll
    //         container.
    auto note_resolved_anchor_function = [&](CSS::AnchorStyleValue const& anchor, bool is_horizontal_axis) {
        if (!default_anchor_box)
            return;

        auto name = anchor.anchor_name();
        if (!name.has_value() && default_anchor_name.has_value())
            name = default_anchor_name.value();
        auto const* anchor_box = name.has_value() ? target_anchor_box(*name) : nullptr;
        if (!anchor_box)
            return;
        if (anchor_box != default_anchor_box
            && nearest_scroll_container_ancestor(*anchor_box) != nearest_scroll_container_ancestor(*default_anchor_box))
            return;

        if (is_horizontal_axis)
            compensates_for_horizontal_scroll = true;
        else
            compensates_for_vertical_scroll = true;
    };

    auto resolve_inset = [&](bool contains_anchor, CSS::StyleValue const& value, CSS::LengthPercentageOrAuto const& existing_value, CSS::PropertyID property_id, bool is_from_end, bool is_horizontal_axis) -> CSS::LengthPercentageOrAuto {
        if (!contains_anchor)
            return existing_value;

        auto containing_block_extent = is_horizontal_axis
            ? containing_block_state.padding_box_inline_size()
            : containing_block_state.padding_box_block_size();

        AnchorInsetResolver anchor_resolver { resolve_anchor_side, note_resolved_anchor_function, box.is_absolutely_positioned(), is_from_end, is_horizontal_axis, containing_block_extent };

        CSS::CalculationResolutionContext resolution_context {
            .percentage_basis = CSS::Length::make_px(containing_block_extent),
            .length_resolution_context = CSS::Length::ResolutionContext::for_layout_node(box),
            .anchor_resolver = &anchor_resolver,
        };

        auto to_inset = [](Optional<CSS::Length> resolved_length) -> CSS::LengthPercentageOrAuto {
            if (!resolved_length.has_value())
                return CSS::LengthPercentageOrAuto::make_auto();
            return { CSS::LengthPercentage { resolved_length.release_value() } };
        };

        // A bare anchor() inset is wrapped in a calculation so it resolves through the same path as calc(anchor()).
        if (value.is_anchor()) {
            auto calculation_context = CSS::CalculationContext::for_property(CSS::PropertyNameAndID::from_id(property_id));
            auto calculation_node = CSS::CalcNodeRef::non_math_function(value.as_anchor(), CSS::NumericType { CSS::NumericType::BaseType::Length, 1 });
            auto calculated_value = CSS::CalculatedStyleValue::create(move(calculation_node), CSS::NumericType { CSS::NumericType::BaseType::Length, 1 }, calculation_context);
            return to_inset(calculated_value->resolve_length(resolution_context));
        }

        return to_inset(value.as_calculated().resolve_length(resolution_context));
    };

    auto const& existing_inset = box.computed_values().inset();
    box.modify_computed_values([&](auto& values) {
        values.set_inset({
            resolve_inset(top_contains_anchor, *top, existing_inset.top(), CSS::PropertyID::Top, false, false),
            resolve_inset(right_contains_anchor, *right, existing_inset.right(), CSS::PropertyID::Right, true, true),
            resolve_inset(bottom_contains_anchor, *bottom, existing_inset.bottom(), CSS::PropertyID::Bottom, true, false),
            resolve_inset(left_contains_anchor, *left, existing_inset.left(), CSS::PropertyID::Left, false, true),
        });
    });

    if (compensates_for_horizontal_scroll || compensates_for_vertical_scroll)
        box.set_default_scroll_shift(default_anchor_box->make_weak_ptr(), compensates_for_horizontal_scroll, compensates_for_vertical_scroll);
}

void FormattingContext::layout_absolutely_positioned_children()
{
    // TableFormattingContext handles cell abspos layout after vertical alignment.
    if (context_box().display().is_table_cell())
        return;
    layout_absolutely_positioned_children(context_box());
}

void FormattingContext::layout_absolutely_positioned_children(Box const& box)
{
    if (m_layout_mode != LayoutMode::Normal)
        return;
    if (m_state.is_for_measurement())
        return;
    // Laying out one child can register further children onto this box (a fixed position
    // box inside an absolutely positioned one shares its viewport containing block), so
    // take one at a time instead of iterating a snapshot.
    while (true) {
        auto child = m_state.take_next_contained_abspos_child(box);
        if (!child.has_value())
            break;
        auto& child_box = const_cast<Box&>(*child->box);
        if (!m_state.try_get(child_box))
            m_state.create(child_box, {}, {});
        resolve_anchor_insets(child_box);
        AbsposLayoutInputs inputs {
            .static_position_rect = resolve_static_position_relative_to_containing_block(child_box, child->static_position_rect),
            .containing_block_info = resolve_abspos_containing_block_info(child_box),
        };
        layout_absolutely_positioned_element(child_box, inputs);
    }
}

StaticPositionRect FormattingContext::resolve_static_position_relative_to_containing_block(Box const& box, StaticPositionRect static_position_rect) const
{
    auto const* static_position_cb = box.static_position_containing_block();
    auto const* actual_containing_block = box.containing_block();
    if (!static_position_cb || static_position_cb == actual_containing_block)
        return static_position_rect;

    // The offset between the static position containing block and the actual containing block only depends on
    // boxes at or below the point where their containing block chains merge. Accumulate offsets up to that
    // point instead of comparing ICB-relative offsets: containing blocks above the merge point may be outside
    // the scope of the current layout (e.g. during intrinsic sizing) and have no used values at all.
    auto const* merge_point = static_position_cb;
    while (merge_point != actual_containing_block && !merge_point->is_ancestor_of(*actual_containing_block))
        merge_point = merge_point->containing_block();
    auto offset_relative_to_merge_point = [&](Box const& descendant) {
        CSSPixelPoint offset;
        for (auto const* node = &descendant; node != merge_point; node = node->containing_block())
            offset += m_state.get(*node).content_offset();
        return offset;
    };
    auto physical_offset = offset_relative_to_merge_point(*static_position_cb) - offset_relative_to_merge_point(*actual_containing_block);
    static_position_rect.rect.offset.inline_offset += physical_offset.x();
    static_position_rect.rect.offset.block_offset += physical_offset.y();
    return static_position_rect;
}

// Hosts replays of the absolutely positioned element layout algorithm outside any ancestor
// formatting context run; the algorithm is defined on the FormattingContext base and consumes
// nothing from the concrete context hosting it.
class AbsposLayoutReplayContext final : public FormattingContext {
public:
    AbsposLayoutReplayContext(LayoutState& state, Box const& containing_block)
        : FormattingContext(Type::AbsposReplay, LayoutMode::Normal, state, containing_block)
    {
    }

    virtual CSSPixels automatic_content_inline_size() const override { VERIFY_NOT_REACHED(); }
    virtual CSSPixels automatic_content_block_size() const override { VERIFY_NOT_REACHED(); }
    virtual void run(LayoutInput const&) override { VERIFY_NOT_REACHED(); }
};

bool FormattingContext::can_replay_saved_abspos_layout_inputs_after_style_change(Box const& box)
{
    if (!box.containing_block())
        return false;

    auto const& inputs = *box.saved_abspos_layout_inputs();
    if (inputs.containing_block_info.derives_from_own_computed_values)
        return false;

    auto axis_modes = abspos_axis_modes_from_computed_insets(box.computed_values());
    bool uses_static_position = axis_modes.inline_axis == AbsposAxisMode::StaticPosition
        || axis_modes.block_axis == AbsposAxisMode::StaticPosition;
    if (uses_static_position && inputs.static_position_rect.alignment_derives_from_own_computed_values)
        return false;

    return true;
}

void FormattingContext::layout_absolutely_positioned_element_from_saved_inputs(LayoutState& state, Box& box)
{
    auto* containing_block = box.containing_block();
    VERIFY(containing_block);
    VERIFY(box.saved_abspos_layout_inputs());
    auto inputs = *box.saved_abspos_layout_inputs();

    AbsposLayoutReplayContext context(state, *containing_block);

    // The axis modes are the only replay-relevant input derived from the box's own computed
    // values, which a style change on the box itself may have altered since capture, so
    // recompute them from the live insets. Inputs that record deriving from the box's own
    // computed values pin their axis modes structurally and never replay after such a change.
    if (!inputs.containing_block_info.derives_from_own_computed_values) {
        auto axis_modes = abspos_axis_modes_from_computed_insets(box.computed_values());
        inputs.containing_block_info.inline_axis_mode = axis_modes.inline_axis;
        inputs.containing_block_info.block_axis_mode = axis_modes.block_axis;
    }

    // Mirror how the ancestor formatting context prepares an absolutely positioned child
    // during a full pass: create its used values first.
    state.create(box, {}, {});

    // Runs the full pipeline: sizing, inside layout, and the box's own absolutely positioned
    // children via parent_context_did_dimension_child_root_box().
    context.layout_absolutely_positioned_element(box, inputs);
}

void FormattingContext::layout_absolutely_positioned_element(Box& box, AbsposLayoutInputs const& inputs)
{
    // SVG elements cannot be absolutely positioned.
    VERIFY(!box.is_svg_box());

    auto const& static_position_rect = inputs.static_position_rect;
    auto const& containing_block_info = inputs.containing_block_info;

    auto& box_state = m_state.get_mutable(box);

    LogicalSize const containing_block_size {
        clamp_to_max_dimension_value(containing_block_info.rect.size.inline_size),
        clamp_to_max_dimension_value(containing_block_info.rect.size.block_size),
    };
    auto const available_space = AvailableSpace(AvailableSize::make_definite(containing_block_size.inline_size), AvailableSize::make_definite(containing_block_size.block_size));

    auto const& computed_values = box.computed_values();

    auto const containing_block_inline_size = available_space.inline_size.to_px_or_zero();
    auto const containing_block_block_size = available_space.block_size.to_px_or_zero();
    // The percentage basis of an absolutely positioned box is the padding box of its absolute
    // positioning containing block, which need not match the basis its used values were created
    // with (grid places absolutely positioned children into their grid area).
    ContainingBlockConstraints const absolutely_positioned_constraints { containing_block_inline_size, containing_block_block_size, {} };

    // The border computed values are not changed by the size calculations below.
    // The spec only adjusts and computes sizes, insets and margins.
    box_state.border_left = computed_values.border_left().width;
    box_state.border_right = computed_values.border_right().width;
    box_state.border_top = computed_values.border_top().width;
    box_state.border_bottom = computed_values.border_bottom().width;

    box_state.padding_left = computed_values.padding().left().to_px_or_zero(containing_block_inline_size);
    box_state.padding_right = computed_values.padding().right().to_px_or_zero(containing_block_inline_size);
    box_state.padding_top = computed_values.padding().top().to_px_or_zero(containing_block_inline_size);
    box_state.padding_bottom = computed_values.padding().bottom().to_px_or_zero(containing_block_inline_size);

    compute_inline_size_for_absolutely_positioned_element(box, available_space, absolutely_positioned_constraints, static_position_rect);

    // NOTE: We compute the block size before and after inside layout so percentages can resolve during child layout.
    //       In some situations, such as non-auto top and bottom values, the block size can be determined early.
    compute_block_size_for_absolutely_positioned_element(box, available_space, absolutely_positioned_constraints, static_position_rect, BeforeOrAfterInsideLayout::Before);

    // If either box axis is fixed or resolved from inset properties,
    // mark the size as being definite (since layout was not required to resolve it, per CSS-SIZING-3).
    auto is_non_auto = [](auto const& length_percentage) {
        return !length_percentage.is_auto();
    };
    if (is_non_auto(computed_values.inset().left()) && is_non_auto(computed_values.inset().right())) {
        box_state.set_has_definite_inline_size(true);
    }
    if (is_non_auto(computed_values.inset().top()) && is_non_auto(computed_values.inset().bottom())
        && (computed_values.height().is_auto() || computed_block_size_establishes_definite_containing_block_size(computed_values.height()))) {
        box_state.set_has_definite_block_size(true);
    }

    // NOTE: BFC is special, as its automatic block size depends on performing inside layout.
    //       For other formatting contexts, the block size resolved early is sufficient.
    //       See FormattingContext::compute_automatic_block_size_for_absolutely_positioned_element()
    //       for the special-casing of BFC roots.
    if (!creates_block_formatting_context(box)) {
        auto block_size_resolved_from_aspect_ratio = computed_values.height().is_auto()
            && box.has_preferred_aspect_ratio()
            && box_state.has_definite_inline_size();
        box_state.set_has_definite_inline_size(true);
        if ((!computed_values.height().is_auto() && computed_block_size_establishes_definite_containing_block_size(computed_values.height())) || block_size_resolved_from_aspect_ratio)
            box_state.set_has_definite_block_size(true);
    }

    make_button_content_box_definite(box, available_space, absolutely_positioned_constraints);

    auto independent_formatting_context = layout_inside(box, LayoutMode::Normal, LayoutInput { box_state.available_inner_space_or_constraints_from(available_space), absolutely_positioned_constraints });

    if (computed_values.height().is_auto()) {
        compute_block_size_for_absolutely_positioned_element(box, available_space, absolutely_positioned_constraints, static_position_rect, BeforeOrAfterInsideLayout::After);
    }

    // Apply grid alignment for auto inset axes
    if (containing_block_info.inline_alignment.has_value() && computed_values.inset().left().is_auto() && computed_values.inset().right().is_auto()) {
        auto available_inline_size_for_alignment = containing_block_size.inline_size - box_state.margin_box_inline_size();
        switch (*containing_block_info.inline_alignment) {
        case Alignment::Center:
            box_state.inset_left = available_inline_size_for_alignment / 2;
            box_state.inset_right = available_inline_size_for_alignment / 2;
            break;
        case Alignment::Start:
            box_state.inset_right = available_inline_size_for_alignment;
            break;
        case Alignment::End:
            box_state.inset_left = available_inline_size_for_alignment;
            break;
        case Alignment::Normal:
        case Alignment::Stretch:
        default:
            break;
        }
    }

    if (containing_block_info.block_alignment.has_value() && computed_values.inset().top().is_auto() && computed_values.inset().bottom().is_auto()) {
        auto available_block_size_for_alignment = containing_block_size.block_size - box_state.margin_box_block_size();
        switch (*containing_block_info.block_alignment) {
        case Alignment::Center:
            box_state.inset_top = available_block_size_for_alignment / 2;
            box_state.inset_bottom = available_block_size_for_alignment / 2;
            break;
        case Alignment::Start:
        case Alignment::SelfStart:
            box_state.inset_bottom = available_block_size_for_alignment;
            break;
        case Alignment::End:
        case Alignment::SelfEnd:
            box_state.inset_top = available_block_size_for_alignment;
            break;
        case Alignment::Normal:
        case Alignment::Stretch:
        case Alignment::Baseline:
        default:
            break;
        }
    }

    LogicalOffset used_offset;

    auto static_offset = aligned_static_offset(static_position_rect, box_state);

    // Inline axis
    if (containing_block_info.inline_axis_mode == AbsposAxisMode::StaticPosition)
        used_offset.inline_offset = static_offset.inline_offset;
    else
        used_offset.inline_offset = containing_block_info.rect.offset.inline_offset + box_state.inset_left;

    // Block axis
    if (containing_block_info.block_axis_mode == AbsposAxisMode::StaticPosition)
        used_offset.block_offset = static_offset.block_offset;
    else
        used_offset.block_offset = containing_block_info.rect.offset.block_offset + box_state.inset_top;

    used_offset.inline_offset += box_state.margin_box_left();
    used_offset.block_offset += box_state.margin_box_top();

    place_child(box, { used_offset.inline_offset, used_offset.block_offset });

    // The inputs reach the box itself only through LayoutState::commit(), so recording them
    // into a throwaway measurement state would just be discarded work.
    if (m_layout_mode == LayoutMode::Normal && !m_state.is_for_measurement())
        box_state.set_abspos_layout_inputs(inputs);

    if (independent_formatting_context)
        independent_formatting_context->parent_context_did_dimension_child_root_box();
}

void FormattingContext::compute_block_size_for_absolutely_positioned_replaced_element(Box const& box, AvailableSpace const& available_space, ContainingBlockConstraints const& containing_block_constraints, StaticPositionRect const& static_position_rect, BeforeOrAfterInsideLayout before_or_after_inside_layout)
{
    // 10.6.5 Absolutely positioned, replaced elements
    // This situation is similar to 10.6.4, except that the element has an intrinsic height.

    // The used value of 'height' is determined as for inline replaced elements.
    auto block_size = compute_block_size_for_replaced_element(box, available_space, containing_block_constraints);

    auto containing_block_block_size = available_space.block_size.to_px_or_zero();
    auto const& computed_values = box.computed_values();
    auto& box_state = m_state.get_mutable(box);
    auto const border_top = computed_values.border_top().width;
    auto const border_bottom = computed_values.border_bottom().width;
    auto const padding_top = box_state.padding_top;
    auto const padding_bottom = box_state.padding_bottom;
    auto available = containing_block_block_size - block_size - border_top - padding_top - padding_bottom - border_bottom;
    auto top = computed_values.inset().top();
    auto margin_top = computed_values.margin().top();
    auto bottom = computed_values.inset().bottom();
    auto margin_bottom = computed_values.margin().bottom();
    auto static_offset = aligned_static_offset(static_position_rect, box_state);

    auto to_px = [&](CSS::LengthPercentageOrAuto const& l) {
        return l.to_px_or_zero(containing_block_block_size);
    };

    // If 'margin-top' or 'margin-bottom' is specified as 'auto' its used value is determined by the rules below.
    // 2. If both 'top' and 'bottom' have the value 'auto', replace 'top' with the element's static position.
    if (top.is_auto() && bottom.is_auto()) {
        top = CSS::Length::make_px(static_offset.block_offset);
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

    box_state.set_content_block_size(block_size);

    // do not set calculated insets or margins on the first pass, there will be a second pass
    if (box.computed_values().height().is_auto() && before_or_after_inside_layout == BeforeOrAfterInsideLayout::Before)
        return;
    if (computed_block_size_establishes_definite_containing_block_size(box.computed_values().height()))
        box_state.set_has_definite_block_size(true);
    box_state.inset_top = to_px(top);
    box_state.inset_bottom = to_px(bottom);
    box_state.margin_top = to_px(margin_top);
    box_state.margin_bottom = to_px(margin_bottom);
}

// https://www.w3.org/TR/css-position-3/#relpos-insets
void FormattingContext::compute_inset(NodeWithStyleAndBoxModelMetrics const& box, CSSPixelSize containing_block_size)
{
    // anchor() functions are unresolvable in the insets of non-absolutely-positioned boxes. Substitute them with their
    // fallback value (or auto) here so the resulting calc() does not reach length resolution unresolved and crash. This
    // also covers sticky boxes, whose insets are read later from these computed values.
    // NB: The box is logically mutable during layout (resolve_anchor_insets rewrites its computed insets), it is only
    //     passed as const& through the compute_inset() call chain.
    if (auto const* anchored_box = as_if<Box>(box)) {
        auto inset_contains_anchor = [](CSS::LengthPercentageOrAuto const& value) {
            return value.is_calculated() && value.calculated()->contains_anchor_function();
        };
        auto const& inset = anchored_box->computed_values().inset();
        if (inset_contains_anchor(inset.top()) || inset_contains_anchor(inset.right())
            || inset_contains_anchor(inset.bottom()) || inset_contains_anchor(inset.left()))
            resolve_anchor_insets(const_cast<Box&>(*anchored_box));
    }

    if (box.computed_values().position() != CSS::Positioning::Relative)
        return;

    auto resolve_two_opposing_insets = [&](CSS::LengthPercentageOrAuto const& computed_first, CSS::LengthPercentageOrAuto const& computed_second, CSSPixels& used_start, CSSPixels& used_end, CSSPixels reference_for_percentage) {
        auto resolved_first = computed_first.to_px_or_zero(reference_for_percentage);
        auto resolved_second = computed_second.to_px_or_zero(reference_for_percentage);

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

    // NOTE: Percentage heights resolve against the containing block's used height. If the containing block's height is
    //       indefinite, percentage insets behave as auto.
    auto treat_percentage_as_auto = [&](CSS::LengthPercentageOrAuto const& value) -> CSS::LengthPercentageOrAuto {
        if (value.contains_percentage()) {
            auto containing_block = box.containing_block();
            while (containing_block && containing_block->is_anonymous() && !containing_block->display().is_table_cell())
                containing_block = containing_block->containing_block();
            if (containing_block && !m_state.get(*containing_block).has_definite_block_size())
                return CSS::LengthPercentageOrAuto::make_auto();
        }
        return value;
    };

    // FIXME: Respect the containing block's writing-mode.
    resolve_two_opposing_insets(computed_values.inset().left(), computed_values.inset().right(), box_state.inset_left, box_state.inset_right, containing_block_size.width());
    resolve_two_opposing_insets(treat_percentage_as_auto(computed_values.inset().top()), treat_percentage_as_auto(computed_values.inset().bottom()), box_state.inset_top, box_state.inset_bottom, containing_block_size.height());
}

// https://drafts.csswg.org/css-sizing-3/#fit-content-size
CSSPixels FormattingContext::calculate_fit_content_inline_size(Layout::Box const& box, AvailableSpace const& available_space, ContainingBlockConstraints const& containing_block_constraints) const
{
    // If the available space in a given axis is definite, equal to clamp(min-content size, stretch-fit size,
    // max-content size) (i.e. max(min-content size, min(max-content size, stretch-fit size))).
    if (available_space.inline_size.is_definite()) {
        auto stretch_fit_inline_size = calculate_stretch_fit_inline_size(box, available_space.inline_size);
        auto max_content_inline_size = calculate_max_content_inline_size(box, containing_block_constraints);
        if (max_content_inline_size <= stretch_fit_inline_size)
            return max_content_inline_size;
        return max(calculate_min_content_inline_size(box, containing_block_constraints), stretch_fit_inline_size);
    }

    // When sizing under a min-content constraint, equal to the min-content size.
    if (available_space.inline_size.is_min_content())
        return calculate_min_content_inline_size(box, containing_block_constraints);

    // Otherwise, equal to the max-content size in that axis.
    return calculate_max_content_inline_size(box, containing_block_constraints);
}

// https://drafts.csswg.org/css-sizing-3/#fit-content-size
CSSPixels FormattingContext::calculate_fit_content_block_size(Layout::Box const& box, AvailableSpace const& available_space, ContainingBlockConstraints const& containing_block_constraints) const
{
    // If the available space in a given axis is definite,
    // equal to clamp(min-content size, stretch-fit size, max-content size)
    // (i.e. max(min-content size, min(max-content size, stretch-fit size))).
    if (available_space.block_size.is_definite()) {
        auto inline_size = available_space.inline_size.to_px_or_zero();
        auto stretch_fit_block_size = calculate_stretch_fit_block_size(box, available_space.block_size);
        auto max_content_block_size = calculate_max_content_block_size(box, inline_size, containing_block_constraints);
        if (max_content_block_size <= stretch_fit_block_size)
            return max_content_block_size;
        return max(calculate_min_content_block_size(box, inline_size, containing_block_constraints), stretch_fit_block_size);
    }

    // When sizing under a min-content constraint, equal to the min-content size.
    if (available_space.block_size.is_min_content())
        return calculate_min_content_block_size(box, available_space.inline_size.to_px_or_zero(), containing_block_constraints);

    // Otherwise, equal to the max-content size in that axis.
    return calculate_max_content_block_size(box, available_space.inline_size.to_px_or_zero(), containing_block_constraints);
}

static IntrinsicSizeCacheKey intrinsic_size_cache_key(ContainingBlockConstraints const& containing_block_constraints)
{
    return {
        .measured_at_inline_size = {},
        .percentage_basis_inline_size = containing_block_constraints.percentage_basis_inline_size,
        .percentage_basis_block_size = containing_block_constraints.percentage_basis_block_size,
        .quirks_mode_percentage_basis_block_size = containing_block_constraints.quirks_mode_percentage_basis_block_size,
    };
}

CSSPixels FormattingContext::calculate_min_content_inline_size(Layout::Box const& box, ContainingBlockConstraints const& containing_block_constraints) const
{
    if (box.is_replaced_box()) {
        // https://www.w3.org/TR/css-sizing-3/#replaced-percentage-min-contribution
        // NOTE: If the box is replaced, a cyclic percentage in the value of any max size property or
        //       preferred size property (width/max-width/height/max-height), is resolved against zero
        //       when calculating the min-content contribution in the corresponding axis.
        // FIXME: If the box also has a preferred aspect ratio, then this min-content contribution is
        //        floored by any <length-percentage> minimum size from the opposite axis—resolving any
        //        such percentage against zero—transferred through the preferred aspect ratio.
        // Note: The min-content contribution is, as always, also floored by the minimum size in its own axis.
        if (box.computed_values().width().contains_percentage() || box.computed_values().max_width().contains_percentage()) {
            auto const& min_inline_size = box.computed_values().min_width();
            if (!min_inline_size.is_length_percentage())
                return 0;

            auto zero_percentage_basis_constraints = containing_block_constraints;
            zero_percentage_basis_constraints.percentage_basis_inline_size = 0;
            return calculate_inner_inline_size(box, AvailableSize::make_min_content(), min_inline_size, zero_percentage_basis_constraints);
        }
    }
    if (auto transferred_inline_size = calculate_transferred_inline_size_for_replaced_element(box, containing_block_constraints); transferred_inline_size.has_value())
        return transferred_inline_size.value();
    auto auto_size = box.auto_content_box_size();
    if (auto_size.has_width())
        return auto_size.width.value();
    if (box.is_replaced_box() && !box.has_preferred_aspect_ratio()) {
        if (auto fallback_inline_size = max_content_size_for_replaced_element_without_natural_size(box, auto_size, m_state.get(box), SizeDimension::Inline); fallback_inline_size.has_value())
            return fallback_inline_size.value();
    }

    // Boxes with no children have zero intrinsic inline size.
    if (!box.has_children())
        return 0;

    auto cache_key = intrinsic_size_cache_key(containing_block_constraints);
    auto& cache = box.cached_intrinsic_sizes().min_content_inline_size;
    if (auto cached_value = cache.get(cache_key); cached_value.has_value())
        return cached_value.value();

    LayoutState throwaway_state(box, LayoutState::Purpose::Measurement);

    auto& box_state = throwaway_state.create(box, containing_block_constraints.percentage_basis_inline_size, containing_block_constraints.percentage_basis_block_size);
    box_state.inline_size_constraint = SizeConstraint::MinContent;
    box_state.set_indefinite_content_inline_size();

    auto context = create_independent_formatting_context(throwaway_state, LayoutMode::IntrinsicSizing, box, const_cast<FormattingContext*>(this));

    auto available_inline_size = AvailableSize::make_min_content();
    auto available_block_size = box_state.has_definite_block_size()
        ? AvailableSize::make_definite(box_state.content_block_size())
        : AvailableSize::make_indefinite();

    auto available_space = AvailableSpace(available_inline_size, available_block_size);
    context->run(LayoutInput { available_space, containing_block_constraints });

    auto min_content_inline_size = clamp_to_max_dimension_value(context->automatic_content_inline_size());
    cache.set(cache_key, min_content_inline_size);
    return min_content_inline_size;
}

// https://drafts.csswg.org/css-sizing-3/#intrinsic-sizes
// "size constraints in the opposite dimension will transfer through and can affect the auto size in the considered one"
Optional<CSSPixels> FormattingContext::calculate_transferred_inline_size_for_replaced_element(Layout::Box const& box, ContainingBlockConstraints const& containing_block_constraints) const
{
    if (!box.is_replaced_box() || !box.has_preferred_aspect_ratio())
        return {};

    // https://drafts.csswg.org/css2/#inline-replaced-width
    // "'width' has a computed value of 'auto', 'height' has some other computed value, and the element does have an intrinsic ratio"
    if (!box.computed_values().width().is_auto())
        return {};

    auto const& computed_block_size = box.computed_values().height();
    if (computed_block_size.is_auto() || computed_block_size.is_intrinsic_sizing_constraint())
        return {};

    auto available_space = m_state.get(box).available_inner_space_or_constraints_from(
        AvailableSpace(AvailableSize::make_max_content(), AvailableSize::make_indefinite()));
    if (should_treat_block_size_as_auto(box, available_space, containing_block_constraints))
        return {};

    // https://drafts.csswg.org/css2/#inline-replaced-width
    // "(used height) * (intrinsic ratio)"
    return compute_inline_size_for_replaced_element(box, available_space, {});
}

CSSPixels FormattingContext::calculate_max_content_inline_size(Layout::Box const& box, ContainingBlockConstraints const& containing_block_constraints) const
{
    auto auto_size = box.auto_content_box_size();
    if (auto transferred_inline_size = calculate_transferred_inline_size_for_replaced_element(box, containing_block_constraints); transferred_inline_size.has_value())
        return transferred_inline_size.value();
    if (!auto_size.has_width()) {
        // https://drafts.csswg.org/css-sizing-3/#cyclic-percentage-contribution
        // "If the box is non-replaced, then the entire value of any max size property or preferred size property
        // ('width'/'max-width'/'height'/'max-height') specified as an expression containing a percentage [...] that is
        // cyclic is treated for the purpose of calculating the box's intrinsic size contributions only as that
        // property's initial value."
        //
        // This means an `appearance: none` text input with a cyclic `width: 100%` still contributes its `width: auto`
        // size to max-content sizing. Do not use this for min-content sizing: CSS Sizing's "Compressible Replaced
        // Elements" section considers non-button-like <input> controls replaced for the percentage-sized replaced
        // element rule, so their cyclic-percentage min-content contribution can still compress toward zero.
        if (auto default_preferred_size = default_preferred_size_for_appearance_none_text_input(box);
            default_preferred_size.has_value()) {
            auto_size = default_preferred_size.value();
        }
    }
    if (auto_size.has_width())
        return auto_size.width.value();

    Optional<CSSPixels> definite_block_size;
    if (box.is_replaced_box() && !auto_size.has_height()) {
        if (auto const& box_state = m_state.get(box); box_state.has_definite_block_size())
            definite_block_size = box_state.content_block_size();
    }

    auto max_content_available_inline_size = AvailableSize::make_max_content();
    auto intrinsic_available_space = AvailableSpace(max_content_available_inline_size, AvailableSize::make_indefinite());

    auto resolve_destination_inline_size = [&](CSS::Size const& size, CyclicPercentageSizeProperty size_property) -> Optional<CSSPixels> {
        if (!size.is_length_percentage())
            return {};

        switch (cyclic_percentage_intrinsic_contribution(box, size, max_content_available_inline_size, size_property)) {
        case CyclicPercentageIntrinsicContribution::TreatAsInitialValue:
            return {};
        case CyclicPercentageIntrinsicContribution::ResolveAsZero: {
            auto zero_percentage_basis_constraints = containing_block_constraints;
            zero_percentage_basis_constraints.percentage_basis_inline_size = 0;
            return calculate_inner_inline_size(box, max_content_available_inline_size, size, zero_percentage_basis_constraints);
        }
        case CyclicPercentageIntrinsicContribution::NotCyclic:
            if (size.contains_percentage() && !containing_block_constraints.percentage_basis_inline_size.has_value())
                return {};
            return calculate_inner_inline_size(box, max_content_available_inline_size, size, containing_block_constraints);
        }
        VERIFY_NOT_REACHED();
    };

    auto resolve_block_size = [&](CSS::Size const& size, CyclicPercentageSizeProperty size_property) -> Optional<CSSPixels> {
        if (!size.is_length_percentage())
            return {};
        if (!size.contains_percentage() || containing_block_constraints.percentage_basis_block_size.has_value())
            return calculate_inner_block_size(box, intrinsic_available_space, size, containing_block_constraints);

        switch (cyclic_percentage_intrinsic_contribution(box, size, max_content_available_inline_size, size_property)) {
        case CyclicPercentageIntrinsicContribution::TreatAsInitialValue:
            return {};
        case CyclicPercentageIntrinsicContribution::ResolveAsZero: {
            auto zero_percentage_basis_constraints = containing_block_constraints;
            zero_percentage_basis_constraints.percentage_basis_block_size = 0;
            return calculate_inner_block_size(box, intrinsic_available_space, size, zero_percentage_basis_constraints);
        }
        case CyclicPercentageIntrinsicContribution::NotCyclic:
            return {};
        }
        VERIFY_NOT_REACHED();
    };

    auto definite_minimum_inline_size = resolve_destination_inline_size(box.computed_values().min_width(), CyclicPercentageSizeProperty::MinSize);
    auto definite_minimum_block_size = resolve_block_size(box.computed_values().min_height(), CyclicPercentageSizeProperty::MinSize);
    ReplacedMaxContentSizeConstraints constraints {
        .definite_size_in_ratio_determining_axis = definite_block_size,
        .minimum_inline_size = definite_minimum_inline_size,
        .minimum_block_size = definite_minimum_block_size,
    };

    if (auto max_content_inline_size = max_content_size_for_replaced_element_without_natural_size(box, auto_size, m_state.get(box), SizeDimension::Inline, constraints); max_content_inline_size.has_value()) {
        if (!definite_block_size.has_value() && box.has_preferred_aspect_ratio()) {
            if (auto definite_maximum_block_size = resolve_block_size(box.computed_values().max_height(), CyclicPercentageSizeProperty::PreferredOrMaxSize); definite_maximum_block_size.has_value()) {
                // https://drafts.csswg.org/css-sizing-4/#aspect-ratio-size-transfers
                // First, any definite minimum size is converted and transferred from the origin to destination axis.
                // This transferred minimum is capped by any definite preferred or maximum size in the destination axis.
                Optional<CSSPixels> transferred_minimum;
                if (definite_minimum_block_size.has_value()) {
                    transferred_minimum = content_inline_size_from_aspect_ratio(box, m_state.get(box), definite_minimum_block_size.value());

                    auto cap_transferred_minimum = [&](CSS::Size const& size, CyclicPercentageSizeProperty size_property) {
                        if (auto resolved_size = resolve_destination_inline_size(size, size_property); resolved_size.has_value())
                            transferred_minimum = min(transferred_minimum.value(), resolved_size.value());
                    };
                    cap_transferred_minimum(box.computed_values().width(), CyclicPercentageSizeProperty::PreferredOrMaxSize);
                    cap_transferred_minimum(box.computed_values().max_width(), CyclicPercentageSizeProperty::PreferredOrMaxSize);
                }

                // Then, any definite maximum size is converted and transferred from the origin to destination.
                // This transferred maximum is floored by any definite preferred or minimum size in the destination axis
                // as well as by the transferred minimum, if any.
                auto transferred_maximum = content_inline_size_from_aspect_ratio(box, m_state.get(box), definite_maximum_block_size.value());

                auto floor_transferred_maximum = [&](CSS::Size const& size, CyclicPercentageSizeProperty size_property) {
                    if (auto resolved_size = resolve_destination_inline_size(size, size_property); resolved_size.has_value())
                        transferred_maximum = max(transferred_maximum, resolved_size.value());
                };
                floor_transferred_maximum(box.computed_values().width(), CyclicPercentageSizeProperty::PreferredOrMaxSize);
                floor_transferred_maximum(box.computed_values().min_width(), CyclicPercentageSizeProperty::MinSize);
                if (transferred_minimum.has_value())
                    transferred_maximum = max(transferred_maximum, transferred_minimum.value());

                return min(max_content_inline_size.value(), transferred_maximum);
            }
        }
        return max_content_inline_size.value();
    }

    // Boxes with no children have zero intrinsic inline size.
    if (!box.has_children())
        return 0;

    auto cache_key = intrinsic_size_cache_key(containing_block_constraints);
    auto& cache = box.cached_intrinsic_sizes().max_content_inline_size;
    if (auto cached_value = cache.get(cache_key); cached_value.has_value())
        return cached_value.value();

    LayoutState throwaway_state(box, LayoutState::Purpose::Measurement);

    auto& box_state = throwaway_state.create(box, containing_block_constraints.percentage_basis_inline_size, containing_block_constraints.percentage_basis_block_size);
    box_state.inline_size_constraint = SizeConstraint::MaxContent;
    box_state.set_indefinite_content_inline_size();

    auto context = create_independent_formatting_context(throwaway_state, LayoutMode::IntrinsicSizing, box, const_cast<FormattingContext*>(this));

    auto available_inline_size = AvailableSize::make_max_content();
    auto available_block_size = box_state.has_definite_block_size()
        ? AvailableSize::make_definite(box_state.content_block_size())
        : AvailableSize::make_indefinite();

    auto available_space = AvailableSpace(available_inline_size, available_block_size);
    context->run(LayoutInput { available_space, containing_block_constraints });

    auto max_content_inline_size = clamp_to_max_dimension_value(context->automatic_content_inline_size());
    cache.set(cache_key, max_content_inline_size);
    return max_content_inline_size;
}

// https://www.w3.org/TR/css-sizing-3/#min-content-block-size
CSSPixels FormattingContext::calculate_min_content_block_size(Layout::Box const& box, CSSPixels inline_size, ContainingBlockConstraints const& containing_block_constraints) const
{
    // For block containers, tables, and inline boxes, this is equivalent to the max-content block size.
    if (box.is_block_container() || box.display().is_table_inside())
        return calculate_max_content_block_size(box, inline_size, containing_block_constraints);

    if (auto auto_size = box.auto_content_box_size(); auto_size.has_height()) {
        if (auto_size.has_aspect_ratio())
            return inline_size / auto_size.aspect_ratio.value();
        return auto_size.height.value();
    }

    // Boxes with no children have zero intrinsic height.
    if (!box.has_children())
        return 0;

    auto cache_key = intrinsic_size_cache_key(containing_block_constraints);
    cache_key.measured_at_inline_size = inline_size;
    auto& cache = box.cached_intrinsic_sizes().min_content_block_size;
    if (auto cached_value = cache.get(cache_key); cached_value.has_value())
        return cached_value.value();

    LayoutState throwaway_state(box, LayoutState::Purpose::Measurement);

    auto& box_state = throwaway_state.create(box, containing_block_constraints.percentage_basis_inline_size, containing_block_constraints.percentage_basis_block_size);
    box_state.block_size_constraint = SizeConstraint::MinContent;
    box_state.set_indefinite_content_block_size();
    box_state.set_content_inline_size(inline_size);

    auto context = create_independent_formatting_context(throwaway_state, LayoutMode::IntrinsicSizing, box, const_cast<FormattingContext*>(this));

    auto available_space = AvailableSpace(AvailableSize::make_definite(inline_size), AvailableSize::make_min_content());
    context->run(LayoutInput { available_space, containing_block_constraints });

    auto min_content_block_size = clamp_to_max_dimension_value(context->automatic_content_block_size());
    cache.set(cache_key, min_content_block_size);
    return min_content_block_size;
}

CSSPixels FormattingContext::calculate_max_content_block_size(Layout::Box const& box, CSSPixels inline_size, ContainingBlockConstraints const& containing_block_constraints) const
{
    if (box.has_preferred_aspect_ratio())
        return inline_size / *box.preferred_aspect_ratio();

    if (auto auto_size = box.auto_content_box_size(); auto_size.has_height())
        return auto_size.height.value();
    if (auto max_content_block_size = max_content_size_for_replaced_element_without_natural_size(box, box.auto_content_box_size(), m_state.get(box), SizeDimension::Block); max_content_block_size.has_value())
        return max_content_block_size.value();

    // Boxes with no children have zero intrinsic height.
    if (!box.has_children())
        return 0;

    auto cache_key = intrinsic_size_cache_key(containing_block_constraints);
    cache_key.measured_at_inline_size = inline_size;
    auto& cache = box.cached_intrinsic_sizes().max_content_block_size;
    if (auto cached_value = cache.get(cache_key); cached_value.has_value())
        return cached_value.value();

    LayoutState throwaway_state(box, LayoutState::Purpose::Measurement);

    auto& box_state = throwaway_state.create(box, containing_block_constraints.percentage_basis_inline_size, containing_block_constraints.percentage_basis_block_size);
    box_state.block_size_constraint = SizeConstraint::MaxContent;
    box_state.set_indefinite_content_block_size();
    box_state.set_content_inline_size(inline_size);

    auto context = create_independent_formatting_context(throwaway_state, LayoutMode::IntrinsicSizing, box, const_cast<FormattingContext*>(this));

    auto available_space = AvailableSpace(AvailableSize::make_definite(inline_size), AvailableSize::make_max_content());
    context->run(LayoutInput { available_space, containing_block_constraints });

    auto max_content_block_size = clamp_to_max_dimension_value(context->automatic_content_block_size());
    cache.set(cache_key, max_content_block_size);
    return max_content_block_size;
}

CSSPixels FormattingContext::calculate_inner_inline_size(Layout::Box const& box, AvailableSize const& available_inline_size, CSS::Size const& preferred_size, ContainingBlockConstraints const& containing_block_constraints) const
{
    VERIFY(!preferred_size.is_auto());

    auto const& box_state = m_state.get(box);
    auto containing_block_inline_size = preferred_size.contains_percentage()
        ? containing_block_constraints.percentage_basis_inline_size.value_or(available_inline_size.to_px_or_zero())
        : available_inline_size.to_px_or_zero();
    if (preferred_size.is_fit_content()) {
        return calculate_fit_content_inline_size(box, AvailableSpace { available_inline_size, AvailableSize::make_indefinite() }, containing_block_constraints);
    }
    if (preferred_size.is_max_content()) {
        return calculate_max_content_inline_size(box, containing_block_constraints);
    }
    if (preferred_size.is_min_content()) {
        return calculate_min_content_inline_size(box, containing_block_constraints);
    }

    auto& computed_values = box.computed_values();
    if (computed_values.box_sizing() == CSS::BoxSizing::BorderBox) {
        auto inner_inline_size = preferred_size.to_px(containing_block_inline_size)
            - computed_values.border_left().width
            - box_state.padding_left
            - computed_values.border_right().width
            - box_state.padding_right;
        return max(inner_inline_size, 0);
    }

    return preferred_size.to_px(containing_block_inline_size);
}

CSSPixels FormattingContext::calculate_inner_block_size(Box const& box, AvailableSpace const& available_space, CSS::Size const& preferred_size, ContainingBlockConstraints const& containing_block_constraints) const
{
    auto const& box_state = m_state.get(box);

    if (preferred_size.is_auto() && box.has_preferred_aspect_ratio())
        return content_block_size_from_aspect_ratio(box, box_state);

    VERIFY(!preferred_size.is_auto());

    if (preferred_size.is_fit_content()) {
        return calculate_fit_content_block_size(box, available_space, containing_block_constraints);
    }
    if (preferred_size.is_max_content()) {
        return calculate_max_content_block_size(box, available_space.inline_size.to_px_or_zero(), containing_block_constraints);
    }
    if (preferred_size.is_min_content()) {
        return calculate_min_content_block_size(box, available_space.inline_size.to_px_or_zero(), containing_block_constraints);
    }

    CSSPixels containing_block_block_size = available_space.block_size.to_px_or_zero();
    // NOTE: Percentage heights are resolved against the containing block's used height,
    //       not the available space height. The containing block's height must be definite
    //       for percentage resolution to work (otherwise should_treat_block_size_as_auto
    //       should have returned true and we wouldn't be here).
    // NOTE: We only do this when available space height is indefinite. If it's definite,
    //       we trust that the caller has set it up correctly (e.g., grid/flex items get
    //       their cell/area size as available space).
    if (preferred_size.contains_percentage() && available_space.block_size.is_indefinite()) {
        // https://quirks.spec.whatwg.org/#the-percentage-height-calculation-quirk
        // NOTE: Flex/grid items resolve percentage heights against their container, not via quirk.
        bool is_flex_or_grid_item = box.parent() && (box.parent()->display().is_flex_inside() || box.parent()->display().is_grid_inside());
        auto shadow_root = box.dom_node() ? box.dom_node()->containing_shadow_root() : nullptr;
        bool is_in_ua_shadow_tree = shadow_root && shadow_root->is_user_agent_internal();
        if (box.document().in_quirks_mode() && !box.is_anonymous() && !is_flex_or_grid_item && !is_in_ua_shadow_tree) {
            containing_block_block_size = containing_block_constraints.quirks_mode_percentage_basis_block_size.value_or(0);
        } else if (containing_block_constraints.percentage_basis_block_size.has_value()) {
            containing_block_block_size = containing_block_constraints.percentage_basis_block_size.value();
        }
    }
    auto& computed_values = box.computed_values();

    if (computed_values.box_sizing() == CSS::BoxSizing::BorderBox) {
        auto inner_block_size = preferred_size.to_px(containing_block_block_size)
            - computed_values.border_top().width
            - box_state.padding_top
            - computed_values.border_bottom().width
            - box_state.padding_bottom;
        return max(inner_block_size, 0);
    }

    return preferred_size.to_px(containing_block_block_size);
}

// https://drafts.csswg.org/css-sizing-3/#stretch-fit-size
CSSPixels FormattingContext::calculate_stretch_fit_inline_size(Box const& box, AvailableSize const& available_inline_size) const
{
    // The size a box would take if its outer size filled the available space in the given axis;
    // in other words, the stretch fit into the available space, if that is definite.

    // Undefined if the available space is indefinite.
    if (!available_inline_size.is_definite())
        return 0;

    auto const& box_state = m_state.get(box);
    return available_inline_size.to_px_or_zero()
        - box_state.margin_left
        - box_state.margin_right
        - box_state.padding_left
        - box_state.padding_right
        - box_state.border_left
        - box_state.border_right;
}

// https://drafts.csswg.org/css-sizing-3/#stretch-fit-size
CSSPixels FormattingContext::calculate_stretch_fit_block_size(Box const& box, AvailableSize const& available_block_size) const
{
    // The size a box would take if its outer size filled the available space in the given axis;
    // in other words, the stretch fit into the available space, if that is definite.
    // Undefined if the available space is indefinite.
    auto const& box_state = m_state.get(box);
    return available_block_size.to_px_or_zero()
        - box_state.margin_top
        - box_state.margin_bottom
        - box_state.padding_top
        - box_state.padding_bottom
        - box_state.border_top
        - box_state.border_bottom;
}

bool FormattingContext::should_treat_inline_size_as_auto(Box const& box, AvailableSpace const& available_space) const
{
    auto const& computed_inline_size = box.computed_values().width();
    if (computed_inline_size.is_auto())
        return true;

    // https://drafts.csswg.org/css-sizing-3/#cyclic-percentage-contribution
    if (computed_inline_size.contains_percentage()) {
        switch (cyclic_percentage_intrinsic_contribution(box, computed_inline_size, available_space.inline_size, CyclicPercentageSizeProperty::PreferredOrMaxSize)) {
        case CyclicPercentageIntrinsicContribution::ResolveAsZero:
            return false;
        case CyclicPercentageIntrinsicContribution::TreatAsInitialValue:
            return true;
        case CyclicPercentageIntrinsicContribution::NotCyclic:
            break;
        }
        if (available_space.inline_size.is_indefinite())
            return true;
    }
    // AD-HOC: If the box has a preferred aspect ratio and an intrinsic keyword for width...
    if (box.has_preferred_aspect_ratio() && computed_inline_size.is_intrinsic_sizing_constraint()) {
        // If the box has no natural height to resolve the aspect ratio, we treat the width as auto.
        if (!box.auto_content_box_size().has_height())
            return true;
        // If the box has definite height, we can resolve the width through the aspect ratio.
        if (m_state.get(box).has_definite_block_size())
            return true;
    }
    return false;
}

bool FormattingContext::should_treat_block_size_as_auto(Box const& box, AvailableSpace const& available_space, ContainingBlockConstraints const& containing_block_constraints) const
{
    auto computed_block_size = box.computed_values().height();
    if (computed_block_size.is_auto()) {
        auto const& box_state = m_state.get(box);
        if (box_state.has_definite_inline_size() && box.has_preferred_aspect_ratio())
            return false;
        return true;
    }

    // https://drafts.csswg.org/css-sizing-3/#cyclic-percentage-contribution
    if (computed_block_size.contains_percentage()) {
        switch (cyclic_percentage_intrinsic_contribution(box, computed_block_size, available_space.block_size, CyclicPercentageSizeProperty::PreferredOrMaxSize)) {
        case CyclicPercentageIntrinsicContribution::ResolveAsZero:
            return false;
        case CyclicPercentageIntrinsicContribution::TreatAsInitialValue:
            return true;
        case CyclicPercentageIntrinsicContribution::NotCyclic:
            break;
        }
        // https://www.w3.org/TR/CSS22/visudet.html#the-height-property
        // If the height of the containing block is not specified explicitly (i.e., it depends on
        // content height), and this element is not absolutely positioned, the percentage value
        // is treated as 'auto'.
        // https://quirks.spec.whatwg.org/#the-percentage-height-calculation-quirk
        // In quirks mode, percentage heights can resolve even without explicit containing block
        // height. The quirk applies to DOM elements only (not anonymous boxes), and excludes
        // table-related display types.
        if (!box.is_absolutely_positioned()) {
            auto percentage_block_size_quirk_applies = [&] {
                if (!box.document().in_quirks_mode() || box.is_anonymous())
                    return false;
                if (box.display().is_table_inside())
                    return false;
                // Flex/grid items resolve percentage heights against their container, not via quirk.
                if (auto* parent = box.parent(); parent && parent->display().is_flex_inside())
                    return false;
                if (auto* parent = box.parent(); parent && parent->display().is_grid_inside())
                    return false;
                // The quirk should not apply inside user agent shadow trees.
                if (auto const* dom_node = box.dom_node()) {
                    if (auto shadow_root = dom_node->containing_shadow_root(); shadow_root && shadow_root->is_user_agent_internal())
                        return false;
                }
                return true;
            }();
            if (!percentage_block_size_quirk_applies) {
                if (!containing_block_constraints.percentage_basis_block_size.has_value())
                    return true;
            }
        }
    }

    // AD-HOC: If the box has a preferred aspect ratio and an intrinsic keyword for height...
    if (box.has_preferred_aspect_ratio() && computed_block_size.is_intrinsic_sizing_constraint()) {
        // If the box has no natural width to resolve the aspect ratio, we treat the height as auto.
        if (!box.auto_content_box_size().has_width())
            return true;
        // If the box has definite width, we can resolve the height through the aspect ratio.
        if (m_state.get(box).has_definite_inline_size())
            return true;
    }
    return false;
}

bool FormattingContext::can_skip_is_anonymous_text_run(Box& box)
{
    if (box.is_anonymous() && !box.is_generated_for_pseudo_element() && !box.first_child_of_type<BlockContainer>()) {
        bool contains_only_white_space = true;
        box.for_each_in_subtree([&](auto const& node) {
            if (!is<TextNode>(node) || !static_cast<TextNode const&>(node).text().is_ascii_whitespace()) {
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

void FormattingContext::compute_and_store_baselines(LayoutState::UsedValues& used_values) const
{
    // NOTE: This may run more than once for the same UsedValues (e.g. table cells are laid out twice),
    //       so reset both baselines before deriving them anew.
    used_values.first_baseline = {};
    used_values.last_baseline = {};

    auto const& box = as<Box>(used_values.node());

    if (!used_values.line_boxes.is_empty()) {
        auto baseline_for_line_box = [&](LineBox const& line_box, BaselineSet baseline_set) -> CSSPixels {
            if (!line_box.has_block_level_box()) {
                auto line_box_block_start = line_box.physical_vertical_end() - line_box.block_length();
                return line_box_block_start + line_box.baseline();
            }

            VERIFY(line_box.fragments().size() == 1);
            auto const& block_child = as<Box>(line_box.fragments().first().layout_node());
            auto const& block_child_state = m_state.get(block_child);
            auto child_offset_from_margin_edge = block_child_state.content_logical_offset().block_offset - block_child_state.margin_box_top();
            return child_offset_from_margin_edge + box_baseline(block_child, baseline_set);
        };

        auto first_line_box = used_values.line_boxes.first_matching([](auto& line_box) { return !line_box.is_empty(); });
        used_values.first_baseline = baseline_for_line_box(first_line_box.value_or(used_values.line_boxes.first()), BaselineSet::First);
        auto last_line_box = used_values.line_boxes.last_matching([](auto& line_box) { return !line_box.is_empty(); });
        used_values.last_baseline = baseline_for_line_box(last_line_box.value_or(used_values.line_boxes.last()), BaselineSet::Last);
        return;
    }

    if (!box.has_children() || box.children_are_inline())
        return;

    // Derive baselines from the first/last in-flow child that has a baseline set of its own.
    // https://drafts.csswg.org/css-flexbox-1/#flex-baselines
    // Otherwise, if the flex container has at least one flex item, the flex container's first/last main-axis baseline
    // set is generated from the alignment baseline of the startmost/endmost flex item.
    // https://drafts.csswg.org/css-grid-1/#grid-baselines
    // Otherwise, the grid container's first (last) baseline set is generated from the alignment baseline of the first
    // (last) grid item in row-major grid order.
    // FIXME: This does not yet select the spec-defined startmost/endmost flex item, or the first/last grid item in
    //        row-major grid order.
    auto baseline_from_children = [&](BaselineSet baseline_set) -> Optional<CSSPixels> {
        auto deriving_first_baseline = baseline_set == BaselineSet::First;
        for (auto child = deriving_first_baseline ? box.first_child() : box.last_child(); child;
            child = deriving_first_baseline ? child->next_sibling() : child->previous_sibling()) {
            auto const* child_box = as_if<Box>(*child);
            if (!child_box)
                continue;
            if (child_box->is_out_of_flow(*this))
                continue;
            auto const* child_state = m_state.try_get(*child_box);
            if (!child_state)
                continue;
            auto const& child_baseline = deriving_first_baseline ? child_state->first_baseline : child_state->last_baseline;
            if (!child_baseline.has_value())
                continue;
            auto child_offset_from_margin_edge = child_state->content_logical_offset().block_offset - child_state->margin_box_top();
            return child_offset_from_margin_edge + box_baseline(*child_box, baseline_set);
        }
        return {};
    };
    used_values.first_baseline = baseline_from_children(BaselineSet::First);
    used_values.last_baseline = baseline_from_children(BaselineSet::Last);
}

CSSPixels FormattingContext::box_baseline(Box const& box, BaselineSet baseline_set) const
{
    auto const& box_state = m_state.get(box);

    // https://drafts.csswg.org/css2/#propdef-vertical-align
    auto const& vertical_align = box.computed_values().vertical_align();
    if (box.vertical_align_applies() && vertical_align.has<CSS::VerticalAlign>()) {
        switch (vertical_align.get<CSS::VerticalAlign>()) {
        case CSS::VerticalAlign::Top:
            // Top: Align the top of the aligned subtree with the top of the line box.
            return box_state.border_box_top();
        case CSS::VerticalAlign::Middle:
            // Middle: Align the vertical midpoint of the box with the baseline of the parent box plus half the x-height of the parent.
            return box_state.margin_box_block_size() / 2 + CSSPixels::nearest_value_for(box.containing_block()->first_available_font().pixel_metrics().x_height / 2);
        case CSS::VerticalAlign::Bottom:
            // Bottom: Align the bottom of the aligned subtree with the bottom of the line box.
            return box_state.content_block_size() + box_state.margin_box_top();
        case CSS::VerticalAlign::TextTop:
            // TextTop: Align the top of the box with the top of the parent's content area (see 10.6.1).
            return box.computed_values().font_size();
        case CSS::VerticalAlign::TextBottom:
            // TextBottom: Align the bottom of the box with the bottom of the parent's content area (see 10.6.1).
            return box_state.margin_box_block_size() - CSSPixels::nearest_value_for(box.containing_block()->first_available_font().pixel_metrics().descent);
        default:
            break;
        }
    }

    // https://drafts.csswg.org/css-inline-3/#baseline-source
    // auto: Specifies last-baseline alignment for inline-block, first-baseline alignment for everything else.
    // NB: Callers ask an inline-level box for its last baseline set, since that is what CSS2's inline-block rule below
    //     describes; inline-level flex and grid containers participate with their first baseline set instead.
    auto const& display = box.display();
    bool is_flex_or_grid_container = display.is_flex_inside() || display.is_grid_inside();
    if (display.is_inline_outside() && is_flex_or_grid_container)
        baseline_set = BaselineSet::First;

    // https://drafts.csswg.org/css2/#propdef-vertical-align
    // The baseline of an 'inline-block' is the baseline of its last line box in the normal flow, unless it has either
    // no in-flow line boxes or if its 'overflow' property has a computed value other than 'visible', in which case the
    // baseline is the bottom margin edge.
    // https://drafts.csswg.org/css-align-3/#baseline-rules
    // CSS Align restates this overflow exception as only applying to the last baseline set: "for legacy reasons if its
    // baseline-source is auto (the initial value) a block-level or inline-level block container that is a scroll
    // container always has a last baseline set, whose baselines all correspond to its block-end margin edge". First
    // baseline sets always derive from content; so do flex and grid containers, which are not block containers.
    // FIXME: Per CSS Align, a scroll container's content-derived baseline position should be clamped to its border
    //        edge.
    auto const& overflow_x = box.computed_values().overflow_x();
    auto const& overflow_y = box.computed_values().overflow_y();
    bool has_visible_overflow = overflow_x == CSS::Overflow::Visible && overflow_y == CSS::Overflow::Visible;
    bool derive_baseline_from_content = baseline_set == BaselineSet::First || is_flex_or_grid_container || has_visible_overflow;

    // AD-HOC: We also use the content-derived baseline for <input> elements with block children. Per the HTML spec,
    //         inputs have `overflow: clip !important`, so CSS2 says to use bottom margin edge. However, the internal
    //         shadow tree baseline should determine the control's baseline for proper alignment with adjacent text.
    //         https://html.spec.whatwg.org/multipage/rendering.html#form-controls
    bool input_derives_from_children = is<HTML::HTMLInputElement>(box.dom_node()) && !box.children_are_inline();

    auto const& content_baseline = baseline_set == BaselineSet::First ? box_state.first_baseline : box_state.last_baseline;
    if (content_baseline.has_value() && (derive_baseline_from_content || input_derives_from_children))
        return box_state.margin_box_top() + *content_baseline;

    // If the box has no baseline set, the bottom margin edge of the box is used.
    return box_state.margin_box_block_size();
}

CSSPixelRect FormattingContext::margin_box_rect(LayoutState::UsedValues const& used_values)
{
    return {
        {
            -max(used_values.margin_box_left(), 0),
            -max(used_values.margin_box_top(), 0),
        },
        {
            max(used_values.margin_box_left(), 0) + used_values.content_inline_size() + max(used_values.margin_box_right(), 0),
            max(used_values.margin_box_top(), 0) + used_values.content_block_size() + max(used_values.margin_box_bottom(), 0),
        },
    };
}

CSSPixelRect FormattingContext::content_box_rect(Box const& box) const
{
    return content_box_rect(m_state.get(box));
}

CSSPixelRect FormattingContext::content_box_rect(LayoutState::UsedValues const& used_values) const
{
    return CSSPixelRect { used_values.content_offset(), used_values.content_size() };
}

CSSPixelRect FormattingContext::content_box_rect_in_ancestor_coordinate_space(LayoutState::UsedValues const& used_values, Box const& ancestor_box) const
{
    CSSPixelRect rect = { { 0, 0 }, used_values.content_size() };
    for (auto const* current = &used_values; current;) {
        if (&current->node() == &ancestor_box)
            return rect;
        rect.translate_by(current->content_offset());
        auto const* containing_block = current->node().containing_block();
        if (!containing_block)
            break;
        current = &m_state.get(*containing_block);
    }
    // If we get here, ancestor_box was not a containing block ancestor of `box`!
    VERIFY_NOT_REACHED();
}

CSSPixelRect FormattingContext::margin_box_rect_in_ancestor_coordinate_space(LayoutState::UsedValues const& used_values, Box const& ancestor_box) const
{
    auto rect = margin_box_rect(used_values);
    for (auto const* current = &used_values; current;) {
        if (&current->node() == &ancestor_box)
            return rect;
        rect.translate_by(current->content_offset());
        auto const* containing_block = current->node().containing_block();
        if (!containing_block)
            break;
        current = &m_state.get(*containing_block);
    }
    // If we get here, ancestor_box was not a containing block ancestor of `box`!
    VERIFY_NOT_REACHED();
}

CSSPixelRect FormattingContext::margin_box_rect_in_ancestor_coordinate_space(Box const& box, Box const& ancestor_box) const
{
    return margin_box_rect_in_ancestor_coordinate_space(m_state.get(box), ancestor_box);
}

bool FormattingContext::box_is_sized_as_replaced_element(Box const& box, AvailableSpace const& available_space, ContainingBlockConstraints const& containing_block_constraints) const
{
    if (box.has_replaced_element_table_display_adjustment())
        return true;

    // When a box has a preferred aspect ratio, its automatic sizes are calculated the same as for a
    // replaced element with a natural aspect ratio and no natural size in that axis, see e.g. CSS2 §10
    // and CSS Flexible Box Model Level 1 §9.2.
    // https://drafts.csswg.org/css-sizing-4/#aspect-ratio-automatic
    if (box.is_replaced_box() && box.has_auto_content_box_size())
        return true;

    if (box.has_preferred_aspect_ratio() || box.has_auto_content_box_size()) {
        // From CSS2:
        // If height and width both have computed values of auto and the element has an intrinsic ratio but no intrinsic height or width,
        // then the used value of width is undefined in CSS 2.
        // However, it is suggested that, if the containing block’s width does not itself depend on the replaced element’s width,
        // then the used value of width is calculated from the constraint equation used for block-level, non-replaced elements in normal flow.

        // AD-HOC: If box has preferred aspect ratio but width and height are not specified, then we should
        //         size it as a normal box to match other browsers.

        auto auto_size = box.auto_content_box_size();
        if (should_treat_inline_size_as_auto(box, available_space)
            && should_treat_block_size_as_auto(box, available_space, containing_block_constraints)
            && !auto_size.has_width()
            && !auto_size.has_height()) {
            return false;
        }
        return true;
    }

    return false;
}

bool FormattingContext::should_treat_max_inline_size_as_none(Box const& box, AvailableSize const& available_inline_size, ContainingBlockConstraints const& containing_block_constraints) const
{
    auto const& max_inline_size = box.computed_values().max_width();
    if (max_inline_size.is_none())
        return true;
    if (available_inline_size.is_max_content() && max_inline_size.is_max_content())
        return true;
    // https://drafts.csswg.org/css-sizing-3/#cyclic-percentage-contribution
    if (max_inline_size.contains_percentage()) {
        switch (cyclic_percentage_intrinsic_contribution(box, max_inline_size, available_inline_size, CyclicPercentageSizeProperty::PreferredOrMaxSize)) {
        case CyclicPercentageIntrinsicContribution::ResolveAsZero:
            return false;
        case CyclicPercentageIntrinsicContribution::TreatAsInitialValue:
            return true;
        case CyclicPercentageIntrinsicContribution::NotCyclic:
            break;
        }
        if (!containing_block_constraints.percentage_basis_inline_size.has_value())
            return true;
    }
    if (max_inline_size.is_fit_content() && available_inline_size.is_intrinsic_sizing_constraint())
        return true;
    if (max_inline_size.is_max_content() && available_inline_size.is_max_content())
        return true;
    if (max_inline_size.is_min_content() && available_inline_size.is_min_content())
        return true;
    return false;
}

bool FormattingContext::should_treat_max_block_size_as_none(Box const& box, AvailableSize const& available_block_size, ContainingBlockConstraints const& containing_block_constraints) const
{
    // https://www.w3.org/TR/CSS22/visudet.html#min-max-heights
    // If the height of the containing block is not specified explicitly (i.e., it depends on content height),
    // and this element is not absolutely positioned, the percentage value is treated as '0' (for 'min-height')
    // or 'none' (for 'max-height').
    auto const& max_block_size = box.computed_values().max_height();
    if (max_block_size.is_none())
        return true;
    if (max_block_size.contains_percentage()) {
        if (available_block_size.is_min_content())
            return false;
        if (!containing_block_constraints.percentage_basis_block_size.has_value())
            return true;
    }
    if (max_block_size.is_fit_content() && available_block_size.is_intrinsic_sizing_constraint())
        return true;
    if (max_block_size.is_max_content() && available_block_size.is_max_content())
        return true;
    if (max_block_size.is_min_content() && available_block_size.is_min_content())
        return true;
    return false;
}

FormattingContext::CyclicPercentageIntrinsicContribution FormattingContext::cyclic_percentage_intrinsic_contribution(Box const& box, CSS::Size const& size, AvailableSize const& available_size, CyclicPercentageSizeProperty size_property) const
{
    if (!size.contains_percentage())
        return CyclicPercentageIntrinsicContribution::NotCyclic;

    // https://drafts.csswg.org/css-sizing-3/#cyclic-percentage-contribution
    // For the min size properties, as well as for margins and paddings (and gutters), a cyclic percentage is resolved
    // against zero for determining intrinsic size contributions.
    if (size_property == CyclicPercentageSizeProperty::MinSize && available_size.is_intrinsic_sizing_constraint())
        return CyclicPercentageIntrinsicContribution::ResolveAsZero;

    // If the box is non-replaced, then the entire value of any max size property or preferred size property
    // ('width'/'max-width'/'height'/'max-height') specified as an expression containing a percentage (such as '10%' or
    // 'calc(10px + 0%)') that is cyclic is treated for the purpose of calculating the box's intrinsic size contributions
    // only as that property's initial value.
    if (available_size.is_min_content()) {
        // If the box is replaced, a cyclic percentage in the value of any max size property or preferred size property
        // ('width'/'max-width'/'height'/'max-height'), is resolved against zero when calculating the min-content
        // contribution in the corresponding axis.
        if (box.is_replaced_box())
            return CyclicPercentageIntrinsicContribution::ResolveAsZero;
        return CyclicPercentageIntrinsicContribution::TreatAsInitialValue;
    }

    if (available_size.is_max_content()) {
        // Likewise, if the box is replaced, then the entire value of any max size property or preferred size property
        // specified as an expression containing a percentage that is cyclic is treated for the purpose of calculating
        // the box's max-content contributions only as that property's initial value.
        return CyclicPercentageIntrinsicContribution::TreatAsInitialValue;
    }

    return CyclicPercentageIntrinsicContribution::NotCyclic;
}

CSSPixels FormattingContext::gap_to_px(Variant<CSS::LengthPercentage, CSS::NormalGap> const& gap, CSSPixels reference_value) const
{
    return gap.visit(
        [](CSS::NormalGap) { return CSSPixels(0); },
        [&](auto const& gap) { return gap.to_px(reference_value); });
}

}
