/*
 * Copyright (c) 2021-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Format.h>
#include <AK/NonnullRefPtr.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/ResolvedCSSStyleDeclaration.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/BackgroundRepeatStyleValue.h>
#include <LibWeb/CSS/StyleValues/BackgroundSizeStyleValue.h>
#include <LibWeb/CSS/StyleValues/CSSColorValue.h>
#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
#include <LibWeb/CSS/StyleValues/FitContentStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShadowStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShorthandStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/StackingContext.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(ResolvedCSSStyleDeclaration);

GC::Ref<ResolvedCSSStyleDeclaration> ResolvedCSSStyleDeclaration::create(DOM::Element& element, Optional<Selector::PseudoElement::Type> pseudo_element)
{
    return element.realm().create<ResolvedCSSStyleDeclaration>(element, move(pseudo_element));
}

ResolvedCSSStyleDeclaration::ResolvedCSSStyleDeclaration(DOM::Element& element, Optional<CSS::Selector::PseudoElement::Type> pseudo_element)
    : CSSStyleDeclaration(element.realm())
    , m_element(element)
    , m_pseudo_element(move(pseudo_element))
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
    if (index >= length())
        return {};
    auto property_id = static_cast<PropertyID>(index + to_underlying(first_longhand_property_id));
    return string_from_property_id(property_id).to_string();
}

static NonnullRefPtr<CSSStyleValue const> style_value_for_length_percentage(LengthPercentage const& length_percentage)
{
    if (length_percentage.is_auto())
        return CSSKeywordValue::create(Keyword::Auto);
    if (length_percentage.is_percentage())
        return PercentageStyleValue::create(length_percentage.percentage());
    if (length_percentage.is_length())
        return LengthStyleValue::create(length_percentage.length());
    return length_percentage.calculated();
}

static NonnullRefPtr<CSSStyleValue const> style_value_for_size(Size const& size)
{
    if (size.is_none())
        return CSSKeywordValue::create(Keyword::None);
    if (size.is_percentage())
        return PercentageStyleValue::create(size.percentage());
    if (size.is_length())
        return LengthStyleValue::create(size.length());
    if (size.is_auto())
        return CSSKeywordValue::create(Keyword::Auto);
    if (size.is_calculated())
        return size.calculated();
    if (size.is_min_content())
        return CSSKeywordValue::create(Keyword::MinContent);
    if (size.is_max_content())
        return CSSKeywordValue::create(Keyword::MaxContent);
    if (size.is_fit_content())
        return FitContentStyleValue::create(size.fit_content_available_space());
    TODO();
}

enum class LogicalSide {
    BlockStart,
    BlockEnd,
    InlineStart,
    InlineEnd,
};
static RefPtr<CSSStyleValue const> style_value_for_length_box_logical_side(Layout::NodeWithStyle const&, LengthBox const& box, LogicalSide logical_side)
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

static CSSPixels pixels_for_pixel_box_logical_side(Layout::NodeWithStyle const&, Painting::PixelBox const& box, LogicalSide logical_side)
{
    // FIXME: Actually determine the logical sides based on layout_node's writing-mode and direction.
    switch (logical_side) {
    case LogicalSide::BlockStart:
        return box.top;
    case LogicalSide::BlockEnd:
        return box.bottom;
    case LogicalSide::InlineStart:
        return box.left;
    case LogicalSide::InlineEnd:
        return box.right;
    }
    VERIFY_NOT_REACHED();
}

static RefPtr<CSSStyleValue const> style_value_for_shadow(Vector<ShadowData> const& shadow_data)
{
    if (shadow_data.is_empty())
        return CSSKeywordValue::create(Keyword::None);

    auto make_shadow_style_value = [](ShadowData const& shadow) {
        return ShadowStyleValue::create(
            CSSColorValue::create_from_color(shadow.color, ColorSyntax::Modern),
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

RefPtr<CSSStyleValue const> ResolvedCSSStyleDeclaration::style_value_for_property(Layout::NodeWithStyle const& layout_node, PropertyID property_id) const
{
    auto used_value_for_property = [&layout_node, property_id](Function<CSSPixels(Painting::PaintableBox const&)>&& used_value_getter) -> Optional<CSSPixels> {
        auto const& display = layout_node.computed_values().display();
        if (!display.is_none() && !display.is_contents() && layout_node.first_paintable()) {
            if (layout_node.first_paintable()->is_paintable_box()) {
                auto const& paintable_box = static_cast<Painting::PaintableBox const&>(*layout_node.first_paintable());
                return used_value_getter(paintable_box);
            }
            dbgln("FIXME: Support getting used value for property `{}` on {}", string_from_property_id(property_id), layout_node.debug_description());
        }
        return {};
    };

    auto get_computed_value = [this](PropertyID property_id) -> auto const& {
        if (m_pseudo_element.has_value())
            return m_element->pseudo_element_computed_properties(m_pseudo_element.value())->property(property_id);
        return m_element->computed_properties()->property(property_id);
    };

    // A limited number of properties have special rules for producing their "resolved value".
    // We also have to manually construct shorthands from their longhands here.
    // Everything else uses the computed value.
    // https://drafts.csswg.org/cssom/#resolved-values

    // The resolved value for a given longhand property can be determined as follows:
    switch (property_id) {
        // -> background-color
        // FIXME: -> border-block-end-color
        // FIXME: -> border-block-start-color
        // -> border-bottom-color
        // -> border-inline-end-color
        // -> border-inline-start-color
        // -> border-left-color
        // -> border-right-color
        // -> border-top-color
        // -> box-shadow
        // -> caret-color
        // -> color
        // -> outline-color
        // -> A resolved value special case property like color defined in another specification
        //    The resolved value is the used value.
    case PropertyID::BackgroundColor:
        return CSSColorValue::create_from_color(layout_node.computed_values().background_color(), ColorSyntax::Modern);
    case PropertyID::BorderBottomColor:
        return CSSColorValue::create_from_color(layout_node.computed_values().border_bottom().color, ColorSyntax::Modern);
    case PropertyID::BorderInlineEndColor:
        // FIXME: Honor writing-mode, direction and text-orientation.
        return style_value_for_property(layout_node, PropertyID::BorderRightColor);
    case PropertyID::BorderInlineStartColor:
        // FIXME: Honor writing-mode, direction and text-orientation.
        return style_value_for_property(layout_node, PropertyID::BorderLeftColor);
    case PropertyID::BorderLeftColor:
        return CSSColorValue::create_from_color(layout_node.computed_values().border_left().color, ColorSyntax::Modern);
    case PropertyID::BorderRightColor:
        return CSSColorValue::create_from_color(layout_node.computed_values().border_right().color, ColorSyntax::Modern);
    case PropertyID::BorderTopColor:
        return CSSColorValue::create_from_color(layout_node.computed_values().border_top().color, ColorSyntax::Modern);
    case PropertyID::BoxShadow:
        return style_value_for_shadow(layout_node.computed_values().box_shadow());
    case PropertyID::CaretColor:
        return CSSColorValue::create_from_color(layout_node.computed_values().caret_color(), ColorSyntax::Modern);
    case PropertyID::Color:
        return CSSColorValue::create_from_color(layout_node.computed_values().color(), ColorSyntax::Modern);
    case PropertyID::OutlineColor:
        return CSSColorValue::create_from_color(layout_node.computed_values().outline_color(), ColorSyntax::Modern);
    case PropertyID::TextDecorationColor:
        return CSSColorValue::create_from_color(layout_node.computed_values().text_decoration_color(), ColorSyntax::Modern);
        // NOTE: text-shadow isn't listed, but is computed the same as box-shadow.
    case PropertyID::TextShadow:
        return style_value_for_shadow(layout_node.computed_values().text_shadow());

        // -> line-height
        //    The resolved value is normal if the computed value is normal, or the used value otherwise.
    case PropertyID::LineHeight: {
        auto const& line_height = get_computed_value(property_id);
        if (line_height.is_keyword() && line_height.to_keyword() == Keyword::Normal)
            return line_height;
        return LengthStyleValue::create(Length::make_px(layout_node.computed_values().line_height()));
    }

        // -> block-size
        // -> height
        // -> inline-size
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
    case PropertyID::BlockSize: {
        auto writing_mode = layout_node.computed_values().writing_mode();
        auto is_vertically_oriented = first_is_one_of(writing_mode, WritingMode::VerticalLr, WritingMode::VerticalRl);
        if (is_vertically_oriented)
            return style_value_for_property(layout_node, PropertyID::Width);
        return style_value_for_property(layout_node, PropertyID::Height);
    }
    case PropertyID::Height: {
        auto maybe_used_height = used_value_for_property([](auto const& paintable_box) { return paintable_box.content_height(); });
        if (maybe_used_height.has_value())
            return style_value_for_size(Size::make_px(maybe_used_height.release_value()));
        return style_value_for_size(layout_node.computed_values().height());
    }
    case PropertyID::InlineSize: {
        auto writing_mode = layout_node.computed_values().writing_mode();
        auto is_vertically_oriented = first_is_one_of(writing_mode, WritingMode::VerticalLr, WritingMode::VerticalRl);
        if (is_vertically_oriented)
            return style_value_for_property(layout_node, PropertyID::Height);
        return style_value_for_property(layout_node, PropertyID::Width);
    }
    case PropertyID::MarginBlockEnd:
        if (auto maybe_used_value = used_value_for_property([&](auto const& paintable_box) { return pixels_for_pixel_box_logical_side(layout_node, paintable_box.box_model().margin, LogicalSide::BlockEnd); }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_box_logical_side(layout_node, layout_node.computed_values().margin(), LogicalSide::BlockEnd);
    case PropertyID::MarginBlockStart:
        if (auto maybe_used_value = used_value_for_property([&](auto const& paintable_box) { return pixels_for_pixel_box_logical_side(layout_node, paintable_box.box_model().margin, LogicalSide::BlockStart); }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_box_logical_side(layout_node, layout_node.computed_values().margin(), LogicalSide::BlockStart);
    case PropertyID::MarginBottom:
        if (auto maybe_used_value = used_value_for_property([](auto const& paintable_box) { return paintable_box.box_model().margin.bottom; }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_percentage(layout_node.computed_values().margin().bottom());
    case PropertyID::MarginInlineEnd:
        if (auto maybe_used_value = used_value_for_property([&](auto const& paintable_box) { return pixels_for_pixel_box_logical_side(layout_node, paintable_box.box_model().margin, LogicalSide::InlineEnd); }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_box_logical_side(layout_node, layout_node.computed_values().margin(), LogicalSide::InlineEnd);
    case PropertyID::MarginInlineStart:
        if (auto maybe_used_value = used_value_for_property([&](auto const& paintable_box) { return pixels_for_pixel_box_logical_side(layout_node, paintable_box.box_model().margin, LogicalSide::InlineStart); }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_box_logical_side(layout_node, layout_node.computed_values().margin(), LogicalSide::InlineStart);
    case PropertyID::MarginLeft:
        if (auto maybe_used_value = used_value_for_property([](auto const& paintable_box) { return paintable_box.box_model().margin.left; }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_percentage(layout_node.computed_values().margin().left());
    case PropertyID::MarginRight:
        if (auto maybe_used_value = used_value_for_property([](auto const& paintable_box) { return paintable_box.box_model().margin.right; }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_percentage(layout_node.computed_values().margin().right());
    case PropertyID::MarginTop:
        if (auto maybe_used_value = used_value_for_property([](auto const& paintable_box) { return paintable_box.box_model().margin.top; }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_percentage(layout_node.computed_values().margin().top());
    case PropertyID::PaddingBlockEnd:
        if (auto maybe_used_value = used_value_for_property([&](auto const& paintable_box) { return pixels_for_pixel_box_logical_side(layout_node, paintable_box.box_model().padding, LogicalSide::BlockEnd); }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_box_logical_side(layout_node, layout_node.computed_values().padding(), LogicalSide::BlockEnd);
    case PropertyID::PaddingBlockStart:
        if (auto maybe_used_value = used_value_for_property([&](auto const& paintable_box) { return pixels_for_pixel_box_logical_side(layout_node, paintable_box.box_model().padding, LogicalSide::BlockStart); }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_box_logical_side(layout_node, layout_node.computed_values().padding(), LogicalSide::BlockStart);
    case PropertyID::PaddingBottom:
        if (auto maybe_used_value = used_value_for_property([](auto const& paintable_box) { return paintable_box.box_model().padding.bottom; }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_percentage(layout_node.computed_values().padding().bottom());
    case PropertyID::PaddingInlineEnd:
        if (auto maybe_used_value = used_value_for_property([&](auto const& paintable_box) { return pixels_for_pixel_box_logical_side(layout_node, paintable_box.box_model().padding, LogicalSide::InlineEnd); }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_box_logical_side(layout_node, layout_node.computed_values().padding(), LogicalSide::InlineEnd);
    case PropertyID::PaddingInlineStart:
        if (auto maybe_used_value = used_value_for_property([&](auto const& paintable_box) { return pixels_for_pixel_box_logical_side(layout_node, paintable_box.box_model().padding, LogicalSide::InlineStart); }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_box_logical_side(layout_node, layout_node.computed_values().padding(), LogicalSide::InlineStart);
    case PropertyID::PaddingLeft:
        if (auto maybe_used_value = used_value_for_property([](auto const& paintable_box) { return paintable_box.box_model().padding.left; }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_percentage(layout_node.computed_values().padding().left());
    case PropertyID::PaddingRight:
        if (auto maybe_used_value = used_value_for_property([](auto const& paintable_box) { return paintable_box.box_model().padding.right; }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
        return style_value_for_length_percentage(layout_node.computed_values().padding().right());
    case PropertyID::PaddingTop:
        if (auto maybe_used_value = used_value_for_property([](auto const& paintable_box) { return paintable_box.box_model().padding.top; }); maybe_used_value.has_value())
            return LengthStyleValue::create(Length::make_px(maybe_used_value.release_value()));
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
            return CSSKeywordValue::create(Keyword::None);

        // https://drafts.csswg.org/css-transforms-2/#serialization-of-the-computed-value
        // The transform property is a resolved value special case property. [CSSOM]
        // When the computed value is a <transform-list>, the resolved value is one <matrix()> function or one <matrix3d()> function computed by the following algorithm:
        // 1. Let transform be a 4x4 matrix initialized to the identity matrix.
        //    The elements m11, m22, m33 and m44 of transform must be set to 1; all other elements of transform must be set to 0.
        auto transform = FloatMatrix4x4::identity();

        // 2. Post-multiply all <transform-function>s in <transform-list> to transform.
        VERIFY(layout_node.first_paintable());
        auto const& paintable_box = as<Painting::PaintableBox const>(*layout_node.first_paintable());
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
            return TransformationStyleValue::create(PropertyID::Transform, TransformFunction::Matrix, move(parameters));
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
            return TransformationStyleValue::create(PropertyID::Transform, TransformFunction::Matrix3d, move(parameters));
        }
    }

        // -> Any other property
        //    The resolved value is the computed value.
        //    NOTE: This is handled inside the `default` case.
    case PropertyID::WebkitTextFillColor:
        return CSSColorValue::create_from_color(layout_node.computed_values().webkit_text_fill_color(), ColorSyntax::Modern);
    case PropertyID::Invalid:
        return CSSKeywordValue::create(Keyword::Invalid);
    case PropertyID::Custom:
        dbgln_if(LIBWEB_CSS_DEBUG, "Computed style for custom properties was requested (?)");
        return nullptr;
    default:
        // For grid-template-columns and grid-template-rows the resolved value is the used value.
        // https://www.w3.org/TR/css-grid-2/#resolved-track-list-standalone
        if (property_id == PropertyID::GridTemplateColumns) {
            if (layout_node.first_paintable() && layout_node.first_paintable()->is_paintable_box()) {
                auto const& paintable_box = as<Painting::PaintableBox const>(*layout_node.first_paintable());
                if (auto used_values_for_grid_template_columns = paintable_box.used_values_for_grid_template_columns()) {
                    return used_values_for_grid_template_columns;
                }
            }
        } else if (property_id == PropertyID::GridTemplateRows) {
            if (layout_node.first_paintable() && layout_node.first_paintable()->is_paintable_box()) {
                auto const& paintable_box = as<Painting::PaintableBox const>(*layout_node.first_paintable());
                if (auto used_values_for_grid_template_rows = paintable_box.used_values_for_grid_template_rows()) {
                    return used_values_for_grid_template_rows;
                }
            }
        }

        if (!property_is_shorthand(property_id))
            return get_computed_value(property_id);

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

    auto get_layout_node = [&]() {
        if (m_pseudo_element.has_value())
            return m_element->get_pseudo_element_node(m_pseudo_element.value());
        return m_element->layout_node();
    };

    Layout::NodeWithStyle* layout_node = get_layout_node();

    // FIXME: Be smarter about updating layout if there's no layout node.
    //        We may legitimately have no layout node if we're not visible, but this protects against situations
    //        where we're requesting the computed style before layout has happened.
    if (!layout_node || property_affects_layout(property_id)) {
        const_cast<DOM::Document&>(m_element->document()).update_layout(DOM::UpdateLayoutReason::ResolvedCSSStyleDeclarationProperty);
        layout_node = get_layout_node();
    } else {
        // FIXME: If we had a way to update style for a single element, this would be a good place to use it.
        const_cast<DOM::Document&>(m_element->document()).update_style();
    }

    if (!layout_node) {
        auto style = m_element->document().style_computer().compute_style(const_cast<DOM::Element&>(*m_element), m_pseudo_element);

        // FIXME: This is a stopgap until we implement shorthand -> longhand conversion.
        auto const* value = style->maybe_null_property(property_id);
        if (!value) {
            dbgln("FIXME: ResolvedCSSStyleDeclaration::property(property_id={:#x}) No value for property ID in newly computed style case.", to_underlying(property_id));
            return {};
        }
        return StyleProperty {
            .property_id = property_id,
            .value = *value,
        };
    }

    auto value = style_value_for_property(*layout_node, property_id);
    if (!value)
        return {};
    return StyleProperty {
        .property_id = property_id,
        .value = *value,
    };
}

Optional<StyleProperty const&> ResolvedCSSStyleDeclaration::custom_property(FlyString const& name) const
{
    const_cast<DOM::Document&>(m_element->document()).update_style();

    auto const* element_to_check = m_element.ptr();
    while (element_to_check) {
        if (auto property = element_to_check->custom_properties(m_pseudo_element).get(name); property.has_value())
            return *property;

        element_to_check = element_to_check->parent_element();
    }

    return {};
}

static WebIDL::ExceptionOr<void> cannot_modify_computed_property_error(JS::Realm& realm)
{
    return WebIDL::NoModificationAllowedError::create(realm, "Cannot modify properties in result of getComputedStyle()"_string);
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-setproperty
WebIDL::ExceptionOr<void> ResolvedCSSStyleDeclaration::set_property(PropertyID, StringView, StringView)
{
    // 1. If the computed flag is set, then throw a NoModificationAllowedError exception.
    return cannot_modify_computed_property_error(realm());
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-setproperty
WebIDL::ExceptionOr<void> ResolvedCSSStyleDeclaration::set_property(StringView, StringView, StringView)
{
    // 1. If the computed flag is set, then throw a NoModificationAllowedError exception.
    return cannot_modify_computed_property_error(realm());
}

static WebIDL::ExceptionOr<String> cannot_remove_computed_property_error(JS::Realm& realm)
{
    return WebIDL::NoModificationAllowedError::create(realm, "Cannot remove properties from result of getComputedStyle()"_string);
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-removeproperty
WebIDL::ExceptionOr<String> ResolvedCSSStyleDeclaration::remove_property(PropertyID)
{
    // 1. If the computed flag is set, then throw a NoModificationAllowedError exception.
    return cannot_remove_computed_property_error(realm());
}

// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-removeproperty
WebIDL::ExceptionOr<String> ResolvedCSSStyleDeclaration::remove_property(StringView)
{
    // 1. If the computed flag is set, then throw a NoModificationAllowedError exception.
    return cannot_remove_computed_property_error(realm());
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
    return WebIDL::NoModificationAllowedError::create(realm(), "Cannot modify properties in result of getComputedStyle()"_string);
}

}
