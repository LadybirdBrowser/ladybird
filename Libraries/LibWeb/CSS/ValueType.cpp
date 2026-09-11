/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16View.h>
#include <LibWeb/CSS/ValueType.h>

namespace Web::CSS {

StringView value_type_to_string(ValueType value_type)
{
    switch (value_type) {
    case Web::CSS::ValueType::Anchor:
        return "Anchor"sv;
    case Web::CSS::ValueType::AnchorSize:
        return "AnchorSize"sv;
    case Web::CSS::ValueType::Angle:
        return "Angle"sv;
    case Web::CSS::ValueType::AnglePercentage:
        return "AnglePercentage"sv;
    case Web::CSS::ValueType::BackgroundPosition:
        return "BackgroundPosition"sv;
    case Web::CSS::ValueType::BasicShape:
        return "BasicShape"sv;
    case Web::CSS::ValueType::Color:
        return "Color"sv;
    case Web::CSS::ValueType::CornerShape:
        return "CornerShape"sv;
    case Web::CSS::ValueType::Counter:
        return "Counter"sv;
    case Web::CSS::ValueType::CounterStyle:
        return "CounterStyle"sv;
    case Web::CSS::ValueType::CustomIdent:
        return "CustomIdent"sv;
    case Web::CSS::ValueType::DashedIdent:
        return "DashedIdent"sv;
    case Web::CSS::ValueType::EasingFunction:
        return "EasingFunction"sv;
    case Web::CSS::ValueType::FilterValueList:
        return "FilterValueList"sv;
    case Web::CSS::ValueType::FitContent:
        return "FitContent"sv;
    case Web::CSS::ValueType::Flex:
        return "Flex"sv;
    case Web::CSS::ValueType::FontStyle:
        return "FontStyle"sv;
    case Web::CSS::ValueType::FontVariantAlternates:
        return "FontVariantAlternates"sv;
    case Web::CSS::ValueType::FontVariantEastAsian:
        return "FontVariantEastAsian"sv;
    case Web::CSS::ValueType::FontVariantLigatures:
        return "FontVariantLigatures"sv;
    case Web::CSS::ValueType::FontVariantNumeric:
        return "FontVariantNumeric"sv;
    case Web::CSS::ValueType::Frequency:
        return "Frequency"sv;
    case Web::CSS::ValueType::FrequencyPercentage:
        return "FrequencyPercentage"sv;
    case Web::CSS::ValueType::Image:
        return "Image"sv;
    case Web::CSS::ValueType::Integer:
        return "Integer"sv;
    case Web::CSS::ValueType::Length:
        return "Length"sv;
    case Web::CSS::ValueType::LengthPercentage:
        return "LengthPercentage"sv;
    case Web::CSS::ValueType::Number:
        return "Number"sv;
    case Web::CSS::ValueType::OpacityValue:
        return "OpacityValue"sv;
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
    case Web::CSS::ValueType::ScrollFunction:
        return "ScrollFunction"sv;
    case Web::CSS::ValueType::String:
        return "String"sv;
    case Web::CSS::ValueType::Time:
        return "Time"sv;
    case Web::CSS::ValueType::TimePercentage:
        return "TimePercentage"sv;
    case Web::CSS::ValueType::TransformFunction:
        return "TransformFunction"sv;
    case Web::CSS::ValueType::TransformList:
        return "TransformList"sv;
    case Web::CSS::ValueType::Url:
        return "Url"sv;
    case Web::CSS::ValueType::ViewFunction:
        return "ViewFunction"sv;
    case Web::CSS::ValueType::ViewTimelineInset:
        return "ViewTimelineInset"sv;
    }

    VERIFY_NOT_REACHED();
}

}
