/*
 * Copyright (c) 2021-2023, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2022-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Format.h>
#include <AK/NonnullRefPtr.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/ResolvedCSSStyleDeclaration.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/BackgroundRepeatStyleValue.h>
#include <LibWeb/CSS/StyleValues/BackgroundSizeStyleValue.h>
#include <LibWeb/CSS/StyleValues/BorderRadiusStyleValue.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/EdgeStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackPlacementStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackSizeListStyleValue.h>
#include <LibWeb/CSS/StyleValues/IdentifierStyleValue.h>
#include <LibWeb/CSS/StyleValues/InitialStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/CSS/StyleValues/RatioStyleValue.h>
#include <LibWeb/CSS/StyleValues/RectStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShadowStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShorthandStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/CSS/StyleValues/URLStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/StackingContext.h>
#include <LibWeb/Painting/ViewportPaintable.h>

namespace Web::CSS {

JS_DEFINE_ALLOCATOR(ResolvedCSSStyleDeclaration);

JS::NonnullGCPtr<ResolvedCSSStyleDeclaration> ResolvedCSSStyleDeclaration::create(DOM::Element& element)
{
    return element.realm().heap().allocate<ResolvedCSSStyleDeclaration>(element.realm(), element);
}

ResolvedCSSStyleDeclaration::ResolvedCSSStyleDeclaration(DOM::Element& element)
    : CSSStyleDeclaration(element.realm())
    , m_element(element)
{
}

void ResolvedCSSStyleDeclaration::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_element);
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-length
size_t ResolvedCSSStyleDeclaration::length() const
{
    // The length attribute must return the number of CSS declarations in the declarations.
    // FIXME: Include the number of custom properties.
    return to_underlying(last_longhand_property_id) - to_underlying(first_longhand_property_id) + 1;
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-item
String ResolvedCSSStyleDeclaration::item(size_t index) const
{
    // The item(index) method must return the property name of the CSS declaration at position index.
    // FIXME: Return custom properties if index > last_longhand_property_id.
    if (index > length())
        return {};
    auto property_id = static_cast<PropertyID>(index + to_underlying(first_longhand_property_id));
    return string_from_property_id(property_id).to_string();
}

static NonnullRefPtr<StyleValue const> style_value_for_background_property(Layout::NodeWithStyle const& layout_node, Function<NonnullRefPtr<StyleValue const>(BackgroundLayerData const&)> callback, Function<NonnullRefPtr<StyleValue const>()> default_value)
{
    auto const& background_layers = layout_node.background_layers();
    if (background_layers.is_empty())
        return default_value();
    if (background_layers.size() == 1)
        return callback(background_layers.first());
    StyleValueVector values;
    values.ensure_capacity(background_layers.size());
    for (auto const& layer : background_layers)
        values.unchecked_append(callback(layer));
    return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
}

static NonnullRefPtr<StyleValue const> style_value_for_length_percentage(LengthPercentage const& length_percentage)
{
    if (length_percentage.is_auto())
        return IdentifierStyleValue::create(ValueID::Auto);
    if (length_percentage.is_percentage())
        return PercentageStyleValue::create(length_percentage.percentage());
    if (length_percentage.is_length())
        return LengthStyleValue::create(length_percentage.length());
    return length_percentage.calculated();
}

static NonnullRefPtr<StyleValue const> style_value_for_size(Size const& size)
{
    if (size.is_none())
        return IdentifierStyleValue::create(ValueID::None);
    if (size.is_percentage())
        return PercentageStyleValue::create(size.percentage());
    if (size.is_length())
        return LengthStyleValue::create(size.length());
    if (size.is_auto())
        return IdentifierStyleValue::create(ValueID::Auto);
    if (size.is_calculated())
        return size.calculated();
    if (size.is_min_content())
        return IdentifierStyleValue::create(ValueID::MinContent);
    if (size.is_max_content())
        return IdentifierStyleValue::create(ValueID::MaxContent);
    // FIXME: Support fit-content(<length>)
    if (size.is_fit_content())
        return IdentifierStyleValue::create(ValueID::FitContent);
    TODO();
}

static NonnullRefPtr<StyleValue const> style_value_for_sided_shorthand(ValueComparingNonnullRefPtr<StyleValue const> top, ValueComparingNonnullRefPtr<StyleValue const> right, ValueComparingNonnullRefPtr<StyleValue const> bottom, ValueComparingNonnullRefPtr<StyleValue const> left)
{
    bool top_and_bottom_same = top == bottom;
    bool left_and_right_same = left == right;

    if (top_and_bottom_same && left_and_right_same && top == left)
        return top;

    if (top_and_bottom_same && left_and_right_same)
        return StyleValueList::create(StyleValueVector { move(top), move(right) }, StyleValueList::Separator::Space);

    if (left_and_right_same)
        return StyleValueList::create(StyleValueVector { move(top), move(right), move(bottom) }, StyleValueList::Separator::Space);

    return StyleValueList::create(StyleValueVector { move(top), move(right), move(bottom), move(left) }, StyleValueList::Separator::Space);
}

enum class LogicalSide {
    BlockStart,
    BlockEnd,
    InlineStart,
    InlineEnd,
};
static RefPtr<StyleValue const> style_value_for_length_box_logical_side(Layout::NodeWithStyle const&, LengthBox const& box, LogicalSide logical_side)
{
    // FIXME: Actually determine the logical sides based on layout_node's writing-mode and direction.
    switch (logical_side) {
    case LogicalSide::BlockStart:
        return style_value_for_length_percentage(box.top());
    case LogicalSide::BlockEnd:
        return style_value_for_length_percentage(box.bottom());
    case LogicalSide::InlineStart:
        return style_value_for_length_percentage(box.left());
    case LogicalSide::InlineEnd:
        return style_value_for_length_percentage(box.right());
    }
    VERIFY_NOT_REACHED();
}

static RefPtr<StyleValue const> style_value_for_shadow(Vector<ShadowData> const& shadow_data)
{
    if (shadow_data.is_empty())
        return IdentifierStyleValue::create(ValueID::None);

    auto make_shadow_style_value = [](ShadowData const& shadow) {
        return ShadowStyleValue::create(
            shadow.color,
            style_value_for_length_percentage(shadow.offset_x),
            style_value_for_length_percentage(shadow.offset_y),
            style_value_for_length_percentage(shadow.blur_radius),
            style_value_for_length_percentage(shadow.spread_distance),
            shadow.placement);
    };

    if (shadow_data.size() == 1)
        return make_shadow_style_value(shadow_data.first());

    StyleValueVector style_values;
    style_values.ensure_capacity(shadow_data.size());
    for (auto& shadow : shadow_data)
        style_values.unchecked_append(make_shadow_style_value(shadow));

    return StyleValueList::create(move(style_values), StyleValueList::Separator::Comma);
}

RefPtr<StyleValue const> ResolvedCSSStyleDeclaration::style_value_for_property(Layout::NodeWithStyle const& layout_node, PropertyID property_id) const
{
    auto used_value_for_property = [&layout_node, property_id](Function<CSSPixels(Painting::PaintableBox const&)>&& used_value_getter) -> Optional<CSSPixels> {
        auto const& display = layout_node.computed_values().display();
        if (!display.is_none() && !display.is_contents() && layout_node.paintable()) {
            if (layout_node.paintable()->is_paintable_box()) {
                auto const& paintable_box = static_cast<Painting::PaintableBox const&>(*layout_node.paintable());
                return used_value_getter(paintable_box);
            }
            dbgln("FIXME: Support getting used value for property `{}` on {}", string_from_property_id(property_id), layout_node.debug_description());
        }
        return {};
    };

    // A limited number of properties have special rules for producing their "resolved value".
    // We also have to manually construct shorthands from their longhands here.
    // Everything else uses the computed value.
    // https://www.w3.org/TR/cssom-1/#resolved-values

    // The resolved value for a given longhand property can be determined as follows:
    switch (property_id) {
        // -> background-color
        // FIXME: -> border-block-end-color
        // FIXME: -> border-block-start-color
        // -> border-bottom-color
        // FIXME: -> border-inline-end-color
        // FIXME: -> border-inline-start-color
        // -> border-left-color
        // -> border-right-color
        // -> border-top-color
        // -> box-shadow
        // FIXME: -> caret-color
        // -> color
        // -> outline-color
        // -> A resolved value special case property like color defined in another specification
        //    The resolved value is the used value.
    case PropertyID::BackgroundColor:
        return ColorStyleValue::create(layout_node.computed_values().background_color());
    case PropertyID::BorderBottomColor:
        return ColorStyleValue::create(layout_node.computed_values().border_bottom().color);
    case PropertyID::BorderLeftColor:
        return ColorStyleValue::create(layout_node.computed_values().border_left().color);
    case PropertyID::BorderRightColor:
        return ColorStyleValue::create(layout_node.computed_values().border_right().color);
    case PropertyID::BorderTopColor:
        return ColorStyleValue::create(layout_node.computed_values().border_top().color);
    case PropertyID::BoxShadow:
        return style_value_for_shadow(layout_node.computed_values().box_shadow());
    case PropertyID::Color:
        return ColorStyleValue::create(layout_node.computed_values().color());
    case PropertyID::OutlineColor:
        return ColorStyleValue::create(layout_node.computed_values().outline_color());
    case PropertyID::TextDecorationColor:
        return ColorStyleValue::create(layout_node.computed_values().text_decoration_color());
        // NOTE: text-shadow isn't listed, but is computed the same as box-shadow.
    case PropertyID::TextShadow:
        return style_value_for_shadow(layout_node.computed_values().text_shadow());

        // -> line-height
        //    The resolved value is normal if the computed value is normal, or the used value otherwise.
    case PropertyID::LineHeight: {
        auto line_height = static_cast<DOM::Element const&>(*layout_node.dom_node()).computed_css_values()->property(property_id);
        if (line_height->is_identifier() && line_height->to_identifier() == ValueID::Normal)
            return line_height;
        return LengthStyleValue::create(Length::make_px(layout_node.computed_values().line_height()));
    }

        // FIXME: -> block-size
        // -> height
        // FIXME: -> inline-size
        // -> margin-block-end
        // -> margin-block-start
        // -> margin-bottom
        // -> margin-inline-end
        // -> margin-inline-start
        // -> margin-left
        // -> margin-right
        // -> margin-top
        // -> padding-block-end
        // -> padding-block-start
        // -> padding-bottom
        // -> padding-inline-end
        // -> padding-inline-start
        // -> padding-left
        // -> padding-right
        // -> padding-top
        // -> width
        // If the property applies to the element or pseudo-element and the resolved value of the
        // display property is not none or contents, then the resolved value is the used value.
        // Otherwise the resolved value is the computed value.
    case PropertyID::Height: {
        auto maybe_used_height = used_value_for_property([](auto const& paintable_box) { return paintable_box.content_height(); });
        if (maybe_used_height.has_value())
            return style_value_for_size(Size::make_px(maybe_used_height.release_value()));
        return style_value_for_size(layout_node.computed_values().height());
    }
    case PropertyID::MarginBlockEnd:
        return style_value_for_length_box_logical_side(layout_node, layout_node.computed_values().margin(), LogicalSide::BlockEnd);
    case PropertyID::MarginBlockStart:
        return style_value_for_length_box_logical_side(layout_node, layout_node.computed_values().margin(), LogicalSide::BlockStart);
    case PropertyID::MarginBottom:
        return style_value_for_length_percentage(layout_node.computed_values().margin().bottom());
    case PropertyID::MarginInlineEnd:
        return style_value_for_length_box_logical_side(layout_node, layout_node.computed_values().margin(), LogicalSide::InlineEnd);
    case PropertyID::MarginInlineStart:
        return style_value_for_length_box_logical_side(layout_node, layout_node.computed_values().margin(), LogicalSide::InlineStart);
    case PropertyID::MarginLeft:
        return style_value_for_length_percentage(layout_node.computed_values().margin().left());
    case PropertyID::MarginRight:
        return style_value_for_length_percentage(layout_node.computed_values().margin().right());
    case PropertyID::MarginTop:
        return style_value_for_length_percentage(layout_node.computed_values().margin().top());
    case PropertyID::PaddingBlockEnd:
        return style_value_for_length_box_logical_side(layout_node, layout_node.computed_values().padding(), LogicalSide::BlockEnd);
    case PropertyID::PaddingBlockStart:
        return style_value_for_length_box_logical_side(layout_node, layout_node.computed_values().padding(), LogicalSide::BlockStart);
    case PropertyID::PaddingBottom:
        return style_value_for_length_percentage(layout_node.computed_values().padding().bottom());
    case PropertyID::PaddingInlineEnd:
        return style_value_for_length_box_logical_side(layout_node, layout_node.computed_values().padding(), LogicalSide::InlineEnd);
    case PropertyID::PaddingInlineStart:
        return style_value_for_length_box_logical_side(layout_node, layout_node.computed_values().padding(), LogicalSide::InlineStart);
    case PropertyID::PaddingLeft:
        return style_value_for_length_percentage(layout_node.computed_values().padding().left());
    case PropertyID::PaddingRight:
        return style_value_for_length_percentage(layout_node.computed_values().padding().right());
    case PropertyID::PaddingTop:
        return style_value_for_length_percentage(layout_node.computed_values().padding().top());
    case PropertyID::Width: {
        auto maybe_used_width = used_value_for_property([](auto const& paintable_box) { return paintable_box.content_width(); });
        if (maybe_used_width.has_value())
            return style_value_for_size(Size::make_px(maybe_used_width.release_value()));
        return style_value_for_size(layout_node.computed_values().width());
    }

        // -> bottom
        // -> left
        // -> inset-block-end
        // -> inset-block-start
        // -> inset-inline-end
        // -> inset-inline-start
        // -> right
        // -> top
        // -> A resolved value special case property like top defined in another specification
        // FIXME: If the property applies to a positioned element and the resolved value of the display property is not
        //    none or contents, and the property is not over-constrained, then the resolved value is the used value.
        //    Otherwise the resolved value is the computed value.
    case PropertyID::Bottom:
        return style_value_for_length_percentage(layout_node.computed_values().inset().bottom());
    case PropertyID::InsetBlockEnd:
        return style_value_for_length_box_logical_side(layout_node, layout_node.computed_values().inset(), LogicalSide::BlockEnd);
    case PropertyID::InsetBlockStart:
        return style_value_for_length_box_logical_side(layout_node, layout_node.computed_values().inset(), LogicalSide::BlockStart);
    case PropertyID::InsetInlineEnd:
        return style_value_for_length_box_logical_side(layout_node, layout_node.computed_values().inset(), LogicalSide::InlineEnd);
    case PropertyID::InsetInlineStart:
        return style_value_for_length_box_logical_side(layout_node, layout_node.computed_values().inset(), LogicalSide::InlineStart);
    case PropertyID::Left:
        return style_value_for_length_percentage(layout_node.computed_values().inset().left());
    case PropertyID::Right:
        return style_value_for_length_percentage(layout_node.computed_values().inset().right());
    case PropertyID::Top:
        return style_value_for_length_percentage(layout_node.computed_values().inset().top());

        // -> A resolved value special case property defined in another specification
        //    As defined in the relevant specification.
    case PropertyID::Transform: {
        auto transformations = layout_node.computed_values().transformations();
        if (transformations.is_empty())
            return IdentifierStyleValue::create(ValueID::None);

        // https://drafts.csswg.org/css-transforms-2/#serialization-of-the-computed-value
        // The transform property is a resolved value special case property. [CSSOM]
        // When the computed value is a <transform-list>, the resolved value is one <matrix()> function or one <matrix3d()> function computed by the following algorithm:
        // 1. Let transform be a 4x4 matrix initialized to the identity matrix.
        //    The elements m11, m22, m33 and m44 of transform must be set to 1; all other elements of transform must be set to 0.
        auto transform = FloatMatrix4x4::identity();

        // 2. Post-multiply all <transform-function>s in <transform-list> to transform.
        VERIFY(layout_node.paintable());
        auto const& paintable_box = verify_cast<Painting::PaintableBox const>(*layout_node.paintable());
        for (auto transformation : transformations) {
            transform = transform * transformation.to_matrix(paintable_box).release_value();
        }

        // https://drafts.csswg.org/css-transforms-1/#2d-matrix
        auto is_2d_matrix = [](Gfx::FloatMatrix4x4 const& matrix) -> bool {
            // A 3x2 transformation matrix,
            // or a 4x4 matrix where the items m31, m32, m13, m23, m43, m14, m24, m34 are equal to 0
            // and m33, m44 are equal to 1.
            // NOTE: We only care about 4x4 matrices here.
            // NOTE: Our elements are 0-indexed not 1-indexed, and in the opposite order.
            if (matrix.elements()[0][2] != 0     // m31
                || matrix.elements()[1][2] != 0  // m32
                || matrix.elements()[2][0] != 0  // m13
                || matrix.elements()[2][1] != 0  // m23
                || matrix.elements()[2][3] != 0  // m43
                || matrix.elements()[3][0] != 0  // m14
                || matrix.elements()[3][1] != 0  // m24
                || matrix.elements()[3][2] != 0) // m34
                return false;

            if (matrix.elements()[2][2] != 1     // m33
                || matrix.elements()[3][3] != 1) // m44
                return false;

            return true;
        };

        // 3. Chose between <matrix()> or <matrix3d()> serialization:
        // -> If transform is a 2D matrix
        //        Serialize transform to a <matrix()> function.
        if (is_2d_matrix(transform)) {
            StyleValueVector parameters {
                NumberStyleValue::create(transform.elements()[0][0]),
                NumberStyleValue::create(transform.elements()[1][0]),
                NumberStyleValue::create(transform.elements()[0][1]),
                NumberStyleValue::create(transform.elements()[1][1]),
                NumberStyleValue::create(transform.elements()[0][3]),
                NumberStyleValue::create(transform.elements()[1][3]),
            };
            return TransformationStyleValue::create(TransformFunction::Matrix, move(parameters));
        }
        // -> Otherwise
        //        Serialize transform to a <matrix3d()> function.
        else {
            StyleValueVector parameters {
                NumberStyleValue::create(transform.elements()[0][0]),
                NumberStyleValue::create(transform.elements()[1][0]),
                NumberStyleValue::create(transform.elements()[2][0]),
                NumberStyleValue::create(transform.elements()[3][0]),
                NumberStyleValue::create(transform.elements()[0][1]),
                NumberStyleValue::create(transform.elements()[1][1]),
                NumberStyleValue::create(transform.elements()[2][1]),
                NumberStyleValue::create(transform.elements()[3][1]),
                NumberStyleValue::create(transform.elements()[0][2]),
                NumberStyleValue::create(transform.elements()[1][2]),
                NumberStyleValue::create(transform.elements()[2][2]),
                NumberStyleValue::create(transform.elements()[3][2]),
                NumberStyleValue::create(transform.elements()[0][3]),
                NumberStyleValue::create(transform.elements()[1][3]),
                NumberStyleValue::create(transform.elements()[2][3]),
                NumberStyleValue::create(transform.elements()[3][3]),
            };
            return TransformationStyleValue::create(TransformFunction::Matrix3d, move(parameters));
        }
    }

        // -> Any other property
        //    The resolved value is the computed value.
        //    NOTE: This is handled inside the `default` case.

        // NOTE: Everything below is a shorthand that requires some manual construction.
    case PropertyID::BackgroundPosition:
        return style_value_for_background_property(
            layout_node,
            [](auto& layer) -> NonnullRefPtr<StyleValue> {
                return PositionStyleValue::create(
                    EdgeStyleValue::create(layer.position_edge_x, layer.position_offset_x),
                    EdgeStyleValue::create(layer.position_edge_y, layer.position_offset_y));
            },
            []() -> NonnullRefPtr<StyleValue> {
                return PositionStyleValue::create(
                    EdgeStyleValue::create(PositionEdge::Left, Percentage(0)),
                    EdgeStyleValue::create(PositionEdge::Top, Percentage(0)));
            });
    case PropertyID::Border: {
        auto width = style_value_for_property(layout_node, PropertyID::BorderWidth);
        auto style = style_value_for_property(layout_node, PropertyID::BorderStyle);
        auto color = style_value_for_property(layout_node, PropertyID::BorderColor);
        // `border` only has a reasonable value if all four sides are the same.
        if (width->is_value_list() || style->is_value_list() || color->is_value_list())
            return nullptr;
        return ShorthandStyleValue::create(property_id,
            { PropertyID::BorderWidth, PropertyID::BorderStyle, PropertyID::BorderColor },
            { width.release_nonnull(), style.release_nonnull(), color.release_nonnull() });
    }
    case PropertyID::BorderColor: {
        auto top = style_value_for_property(layout_node, PropertyID::BorderTopColor);
        auto right = style_value_for_property(layout_node, PropertyID::BorderRightColor);
        auto bottom = style_value_for_property(layout_node, PropertyID::BorderBottomColor);
        auto left = style_value_for_property(layout_node, PropertyID::BorderLeftColor);
        return style_value_for_sided_shorthand(top.release_nonnull(), right.release_nonnull(), bottom.release_nonnull(), left.release_nonnull());
    }
    case PropertyID::BorderStyle: {
        auto top = style_value_for_property(layout_node, PropertyID::BorderTopStyle);
        auto right = style_value_for_property(layout_node, PropertyID::BorderRightStyle);
        auto bottom = style_value_for_property(layout_node, PropertyID::BorderBottomStyle);
        auto left = style_value_for_property(layout_node, PropertyID::BorderLeftStyle);
        return style_value_for_sided_shorthand(top.release_nonnull(), right.release_nonnull(), bottom.release_nonnull(), left.release_nonnull());
    }
    case PropertyID::BorderWidth: {
        auto top = style_value_for_property(layout_node, PropertyID::BorderTopWidth);
        auto right = style_value_for_property(layout_node, PropertyID::BorderRightWidth);
        auto bottom = style_value_for_property(layout_node, PropertyID::BorderBottomWidth);
        auto left = style_value_for_property(layout_node, PropertyID::BorderLeftWidth);
        return style_value_for_sided_shorthand(top.release_nonnull(), right.release_nonnull(), bottom.release_nonnull(), left.release_nonnull());
    }
    case PropertyID::Margin: {
        auto top = style_value_for_property(layout_node, PropertyID::MarginTop);
        auto right = style_value_for_property(layout_node, PropertyID::MarginRight);
        auto bottom = style_value_for_property(layout_node, PropertyID::MarginBottom);
        auto left = style_value_for_property(layout_node, PropertyID::MarginLeft);
        return style_value_for_sided_shorthand(top.release_nonnull(), right.release_nonnull(), bottom.release_nonnull(), left.release_nonnull());
    }
    case PropertyID::Padding: {
        auto top = style_value_for_property(layout_node, PropertyID::PaddingTop);
        auto right = style_value_for_property(layout_node, PropertyID::PaddingRight);
        auto bottom = style_value_for_property(layout_node, PropertyID::PaddingBottom);
        auto left = style_value_for_property(layout_node, PropertyID::PaddingLeft);
        return style_value_for_sided_shorthand(top.release_nonnull(), right.release_nonnull(), bottom.release_nonnull(), left.release_nonnull());
    }
    case PropertyID::Invalid:
        return IdentifierStyleValue::create(ValueID::Invalid);
    case PropertyID::Custom:
        dbgln_if(LIBWEB_CSS_DEBUG, "Computed style for custom properties was requested (?)");
        return nullptr;
    default:
        if (!property_is_shorthand(property_id))
            return static_cast<DOM::Element const&>(*layout_node.dom_node()).computed_css_values()->property(property_id);

        // Handle shorthands in a generic way
        auto longhand_ids = longhands_for_shorthand(property_id);
        StyleValueVector longhand_values;
        longhand_values.ensure_capacity(longhand_ids.size());
        for (auto longhand_id : longhand_ids)
            longhand_values.append(style_value_for_property(layout_node, longhand_id).release_nonnull());
        return ShorthandStyleValue::create(property_id, move(longhand_ids), move(longhand_values));
    }
}

Optional<StyleProperty> ResolvedCSSStyleDeclaration::property(PropertyID property_id) const
{
    // https://www.w3.org/TR/cssom-1/#dom-window-getcomputedstyle
    // NOTE: This is a partial enforcement of step 5 ("If elt is connected, ...")
    if (!m_element->is_connected())
        return {};

    // FIXME: Be smarter about updating layout if there's no layout node.
    //        We may legitimately have no layout node if we're not visible, but this protects against situations
    //        where we're requesting the computed style before layout has happened.
    if (!m_element->layout_node() || property_affects_layout(property_id)) {
        const_cast<DOM::Document&>(m_element->document()).update_layout();
    } else {
        // FIXME: If we had a way to update style for a single element, this would be a good place to use it.
        const_cast<DOM::Document&>(m_element->document()).update_style();
    }

    if (!m_element->layout_node()) {
        auto style = m_element->document().style_computer().compute_style(const_cast<DOM::Element&>(*m_element));

        // FIXME: This is a stopgap until we implement shorthand -> longhand conversion.
        auto value = style->maybe_null_property(property_id);
        if (!value) {
            dbgln("FIXME: ResolvedCSSStyleDeclaration::property(property_id={:#x}) No value for property ID in newly computed style case.", to_underlying(property_id));
            return {};
        }
        return StyleProperty {
            .property_id = property_id,
            .value = value.release_nonnull(),
        };
    }

    auto& layout_node = *m_element->layout_node();
    auto value = style_value_for_property(layout_node, property_id);
    if (!value)
        return {};
    return StyleProperty {
        .property_id = property_id,
        .value = value.release_nonnull(),
    };
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-setproperty
WebIDL::ExceptionOr<void> ResolvedCSSStyleDeclaration::set_property(PropertyID, StringView, StringView)
{
    // 1. If the computed flag is set, then throw a NoModificationAllowedError exception.
    return WebIDL::NoModificationAllowedError::create(realm(), "Cannot modify properties in result of getComputedStyle()"_fly_string);
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-removeproperty
WebIDL::ExceptionOr<String> ResolvedCSSStyleDeclaration::remove_property(PropertyID)
{
    // 1. If the computed flag is set, then throw a NoModificationAllowedError exception.
    return WebIDL::NoModificationAllowedError::create(realm(), "Cannot remove properties from result of getComputedStyle()"_fly_string);
}

String ResolvedCSSStyleDeclaration::serialized() const
{
    // https://www.w3.org/TR/cssom/#dom-cssstyledeclaration-csstext
    // If the computed flag is set, then return the empty string.

    // NOTE: ResolvedCSSStyleDeclaration is something you would only get from window.getComputedStyle(),
    //       which returns what the spec calls "resolved style". The "computed flag" is always set here.
    return String {};
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-csstext
WebIDL::ExceptionOr<void> ResolvedCSSStyleDeclaration::set_css_text(StringView)
{
    // 1. If the computed flag is set, then throw a NoModificationAllowedError exception.
    return WebIDL::NoModificationAllowedError::create(realm(), "Cannot modify properties in result of getComputedStyle()"_fly_string);
}

}
