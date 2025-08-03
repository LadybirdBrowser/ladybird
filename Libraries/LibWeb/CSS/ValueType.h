/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

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
    OpenTypeTag,
    Paint,
    Percentage,
    Position,
    Ratio,
    Rect,
    Resolution,
    String,
    Time,
    Url,
};

Optional<ValueType> value_type_from_string(StringView);

}
