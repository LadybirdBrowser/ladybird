/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <LibWeb/SVG/SVGAnimatedEnumeration.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/types.html#InterfaceSVGUnitTypes
enum class SVGUnitType : u16 {
    Unknown = 0,
    UserSpaceOnUse = 1,
    ObjectBoundingBox = 2,
};

inline constexpr Array svg_unit_type_keywords {
    SVGAnimatedEnumeration::Keyword { "userSpaceOnUse"sv, to_underlying(SVGUnitType::UserSpaceOnUse) },
    SVGAnimatedEnumeration::Keyword { "objectBoundingBox"sv, to_underlying(SVGUnitType::ObjectBoundingBox) },
};

}
