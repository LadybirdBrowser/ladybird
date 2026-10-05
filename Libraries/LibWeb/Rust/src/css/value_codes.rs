/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Codes that style value data shares with C++, defined once.

// The `CSS::ValueType` discriminants, as C++ declares them in `CSS/ValueType.h`.
pub(crate) const VALUE_TYPE_ANCHOR: u8 = 0;
pub(crate) const VALUE_TYPE_ANGLE: u8 = 2;
pub(crate) const VALUE_TYPE_BACKGROUND_POSITION: u8 = 4;
pub(crate) const VALUE_TYPE_BASIC_SHAPE: u8 = 5;
pub(crate) const VALUE_TYPE_COLOR: u8 = 6;
pub(crate) const VALUE_TYPE_CORNER_SHAPE: u8 = 7;
pub(crate) const VALUE_TYPE_CUSTOM_IDENT: u8 = 10;
pub(crate) const VALUE_TYPE_DASHED_IDENT: u8 = 11;
pub(crate) const VALUE_TYPE_FIT_CONTENT: u8 = 14;
pub(crate) const VALUE_TYPE_FLEX: u8 = 15;
pub(crate) const VALUE_TYPE_FREQUENCY: u8 = 21;
pub(crate) const VALUE_TYPE_IMAGE: u8 = 23;
pub(crate) const VALUE_TYPE_INTEGER: u8 = 24;
pub(crate) const VALUE_TYPE_LENGTH: u8 = 25;
pub(crate) const VALUE_TYPE_LENGTH_PERCENTAGE: u8 = 26;
pub(crate) const VALUE_TYPE_NUMBER: u8 = 27;
pub(crate) const VALUE_TYPE_OPACITY_VALUE: u8 = 28;
pub(crate) const VALUE_TYPE_PERCENTAGE: u8 = 31;
pub(crate) const VALUE_TYPE_POSITION: u8 = 32;
pub(crate) const VALUE_TYPE_RATIO: u8 = 33;
pub(crate) const VALUE_TYPE_RECT: u8 = 34;
pub(crate) const VALUE_TYPE_RESOLUTION: u8 = 35;
pub(crate) const VALUE_TYPE_STRING: u8 = 37;
pub(crate) const VALUE_TYPE_TIME: u8 = 38;
pub(crate) const VALUE_TYPE_URL: u8 = 42;

// The `CSS::ColorStyleValue::ColorType` discriminants.
pub(crate) const COLOR_TYPE_RGB: u8 = 0;
pub(crate) const COLOR_TYPE_A98_RGB: u8 = 1;
pub(crate) const COLOR_TYPE_DISPLAY_P3: u8 = 2;
pub(crate) const COLOR_TYPE_DISPLAY_P3_LINEAR: u8 = 3;
pub(crate) const COLOR_TYPE_HSL: u8 = 4;
pub(crate) const COLOR_TYPE_HWB: u8 = 5;
pub(crate) const COLOR_TYPE_LAB: u8 = 6;
pub(crate) const COLOR_TYPE_LCH: u8 = 7;
pub(crate) const COLOR_TYPE_OKLAB: u8 = 8;
pub(crate) const COLOR_TYPE_OKLCH: u8 = 9;
pub(crate) const COLOR_TYPE_SRGB: u8 = 10;
pub(crate) const COLOR_TYPE_SRGB_LINEAR: u8 = 11;
pub(crate) const COLOR_TYPE_PROPHOTO_RGB: u8 = 12;
pub(crate) const COLOR_TYPE_REC2020: u8 = 13;
pub(crate) const COLOR_TYPE_XYZ_D50: u8 = 14;
pub(crate) const COLOR_TYPE_XYZ_D65: u8 = 15;

// The `CSS::ColorSyntax` discriminants.
pub(crate) const COLOR_SYNTAX_LEGACY: u8 = 0;
pub(crate) const COLOR_SYNTAX_MODERN: u8 = 1;
