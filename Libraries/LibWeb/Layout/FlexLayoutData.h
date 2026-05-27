/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Layout {

enum class FlexLayoutGrowthState : u8 {
    Growing,
    Shrinking,
};

enum class FlexLayoutClampState : u8 {
    Unclamped,
    ClampedToMin,
    ClampedToMax,
};

struct FlexLayoutItem {
    Optional<UniqueNodeID> node_id;
    String main_axis_direction;
    String cross_axis_direction;
    CSSPixelRect rect;
    CSSPixels main_base_size { 0 };
    CSSPixels main_delta_size { 0 };
    CSSPixels main_min_size { 0 };
    CSSPixels main_max_size { 0 };
    CSSPixels cross_min_size { 0 };
    CSSPixels cross_max_size { 0 };
    FlexLayoutClampState clamp_state { FlexLayoutClampState::Unclamped };
    String flex_basis;
    String main_size_property;
    String main_min_size_property;
    String main_max_size_property;
    double flex_grow { 0 };
    double flex_shrink { 0 };
};

struct FlexLayoutLine {
    FlexLayoutGrowthState growth_state { FlexLayoutGrowthState::Shrinking };
    CSSPixels cross_start { 0 };
    CSSPixels cross_size { 0 };
    Vector<FlexLayoutItem> items;
};

struct FlexLayoutData {
    CSS::AlignContent align_content { CSS::AlignContent::Normal };
    CSS::AlignItems align_items { CSS::AlignItems::Normal };
    CSS::FlexDirection flex_direction { CSS::FlexDirection::Row };
    CSS::FlexWrap flex_wrap { CSS::FlexWrap::Nowrap };
    CSS::JustifyContent justify_content { CSS::JustifyContent::Normal };
    Vector<FlexLayoutLine> lines;
};

}
