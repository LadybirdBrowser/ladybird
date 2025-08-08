/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringView.h>
#include <LibWeb/CSS/ValueType.h>

namespace Web::CSS {

Optional<ValueType> value_type_from_string(StringView string)
{
    if (string.equals_ignoring_ascii_case("anchor-size"sv))
        return ValueType::AnchorSize;
    if (string.equals_ignoring_ascii_case("angle"sv))
        return ValueType::Angle;
    if (string.equals_ignoring_ascii_case("background-position"sv))
        return ValueType::BackgroundPosition;
    if (string.equals_ignoring_ascii_case("basic-shape"sv))
        return ValueType::BasicShape;
    if (string.equals_ignoring_ascii_case("color"sv))
        return ValueType::Color;
    if (string.equals_ignoring_ascii_case("counter"sv))
        return ValueType::Counter;
    if (string.equals_ignoring_ascii_case("custom-ident"sv))
        return ValueType::CustomIdent;
    if (string.equals_ignoring_ascii_case("easing-function"sv))
        return ValueType::EasingFunction;
    if (string.equals_ignoring_ascii_case("filter-value-list"sv))
        return ValueType::FilterValueList;
    if (string.equals_ignoring_ascii_case("fit-content"sv))
        return ValueType::FitContent;
    if (string.equals_ignoring_ascii_case("flex"sv))
        return ValueType::Flex;
    if (string.equals_ignoring_ascii_case("frequency"sv))
        return ValueType::Frequency;
    if (string.equals_ignoring_ascii_case("image"sv))
        return ValueType::Image;
    if (string.equals_ignoring_ascii_case("integer"sv))
        return ValueType::Integer;
    if (string.equals_ignoring_ascii_case("length"sv))
        return ValueType::Length;
    if (string.equals_ignoring_ascii_case("number"sv))
        return ValueType::Number;
    if (string.equals_ignoring_ascii_case("opentype-tag"sv))
        return ValueType::OpentypeTag;
    if (string.equals_ignoring_ascii_case("paint"sv))
        return ValueType::Paint;
    if (string.equals_ignoring_ascii_case("percentage"sv))
        return ValueType::Percentage;
    if (string.equals_ignoring_ascii_case("position"sv))
        return ValueType::Position;
    if (string.equals_ignoring_ascii_case("ratio"sv))
        return ValueType::Ratio;
    if (string.equals_ignoring_ascii_case("rect"sv))
        return ValueType::Rect;
    if (string.equals_ignoring_ascii_case("resolution"sv))
        return ValueType::Resolution;
    if (string.equals_ignoring_ascii_case("string"sv))
        return ValueType::String;
    if (string.equals_ignoring_ascii_case("time"sv))
        return ValueType::Time;
    if (string.equals_ignoring_ascii_case("url"sv))
        return ValueType::Url;
    return {};
}

StringView value_type_to_string(ValueType value_type)
{
    switch (value_type) {
    case Web::CSS::ValueType::Anchor:
        return "Anchor"sv;
    case Web::CSS::ValueType::AnchorSize:
        return "AnchorSize"sv;
    case Web::CSS::ValueType::Angle:
        return "Angle"sv;
    case Web::CSS::ValueType::BackgroundPosition:
        return "BackgroundPosition"sv;
    case Web::CSS::ValueType::BasicShape:
        return "BasicShape"sv;
    case Web::CSS::ValueType::Color:
        return "Color"sv;
    case Web::CSS::ValueType::Counter:
        return "Counter"sv;
    case Web::CSS::ValueType::CustomIdent:
        return "CustomIdent"sv;
    case Web::CSS::ValueType::EasingFunction:
        return "EasingFunction"sv;
    case Web::CSS::ValueType::FilterValueList:
        return "FilterValueList"sv;
    case Web::CSS::ValueType::FitContent:
        return "FitContent"sv;
    case Web::CSS::ValueType::Flex:
        return "Flex"sv;
    case Web::CSS::ValueType::Frequency:
        return "Frequency"sv;
    case Web::CSS::ValueType::Image:
        return "Image"sv;
    case Web::CSS::ValueType::Integer:
        return "Integer"sv;
    case Web::CSS::ValueType::Length:
        return "Length"sv;
    case Web::CSS::ValueType::Number:
        return "Number"sv;
    case Web::CSS::ValueType::OpentypeTag:
        return "OpenTypeTag"sv;
    case Web::CSS::ValueType::Paint:
        return "Paint"sv;
    case Web::CSS::ValueType::Percentage:
        return "Percentage"sv;
    case Web::CSS::ValueType::Position:
        return "Position"sv;
    case Web::CSS::ValueType::Ratio:
        return "Ratio"sv;
    case Web::CSS::ValueType::Rect:
        return "Rect"sv;
    case Web::CSS::ValueType::Resolution:
        return "Resolution"sv;
    case Web::CSS::ValueType::String:
        return "String"sv;
    case Web::CSS::ValueType::Time:
        return "Time"sv;
    case Web::CSS::ValueType::Url:
        return "Url"sv;
    }

    VERIFY_NOT_REACHED();
}

}
