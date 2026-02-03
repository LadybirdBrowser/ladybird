/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Format.h>
#include <AK/Optional.h>
#include <AK/Types.h>

namespace Web::CSS {

enum class ValueType : u8 {
    Anchor,
    AnchorSize,
    Angle,
    AnglePercentage,
    BackgroundPosition,
    BasicShape,
    Color,
    CornerShape,
    Counter,
    CounterStyle,
    CustomIdent,
    DashedIdent,
    EasingFunction,
    FilterValueList,
    FitContent,
    Flex,
    FontStyle,
    Frequency,
    FrequencyPercentage,
    Image,
    Integer,
    Length,
    LengthPercentage,
    Number,
    Opacity,
    OpentypeTag,
    Paint,
    Percentage,
    Position,
    Ratio,
    Rect,
    Resolution,
    ScrollFunction,
    String,
    Time,
    TimePercentage,
    TransformFunction,
    TransformList,
    Url,
    ViewFunction,
    ViewTimelineInset
};

StringView value_type_to_string(ValueType);
Optional<ValueType> value_type_from_string(StringView);

}

template<>
struct AK::Formatter<Web::CSS::ValueType> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::ValueType type)
    {
        return Formatter<StringView>::format(builder, Web::CSS::value_type_to_string(type));
    }
};
