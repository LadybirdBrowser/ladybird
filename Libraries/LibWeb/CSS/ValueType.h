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
    BackgroundPosition,
    BasicShape,
    Color,
    CornerShape,
    Counter,
    CustomIdent,
    EasingFunction,
    FilterValueList,
    FitContent,
    Flex,
    Frequency,
    Image,
    Integer,
    Length,
    Number,
    OpentypeTag,
    Paint,
    Percentage,
    Position,
    Ratio,
    Rect,
    Resolution,
    String,
    Time,
    TransformFunction,
    TransformList,
    Url,
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
