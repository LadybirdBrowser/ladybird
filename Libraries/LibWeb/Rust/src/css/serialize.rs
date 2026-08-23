/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSS style value serialization.
//!
//! Serializes Rust-owned [`StyleValueData`] into text without crossing the FFI per component.
//! The output stays ASCII until the first non-ASCII code unit forces UTF-16 storage, matching
//! AK::Utf16String's ASCII-or-UTF16 representation, then moves a native string owner to C++.
//! Value types whose serialization has not been ported yet return no string, and the C++
//! dispatcher falls back to the legacy per-class serializer.

use std::ffi::c_void;

use crate::css::css_enums::keyword;
use crate::css::css_tokenizer::TokenizerInput;
use crate::css::retained_fly_string::RetainedUtf16FlyString;
use crate::css::style_value::{RetainedColorStopList, RetainedString, StyleValueData};

include!(concat!(env!("OUT_DIR"), "/transform_functions_generated.rs"));

/// Mirrors `Web::CSS::SerializationMode`.
#[derive(Clone, Copy, PartialEq)]
pub(crate) enum SerializationMode {
    Normal,
    ResolvedValue,
}

impl SerializationMode {
    fn from_ffi(mode: u8) -> Self {
        match mode {
            0 => Self::Normal,
            _ => Self::ResolvedValue,
        }
    }
}

/// The decoded storage of a fly string: ASCII bytes or UTF-16 code units.
pub(crate) enum StringUnits<'a> {
    Ascii(&'a [u8]),
    Utf16(&'a [u16]),
}

/// Calls `f` with a view of the fly string's contents.
pub(crate) fn with_fly_string_units<R>(string: &RetainedUtf16FlyString, f: impl FnOnce(StringUnits) -> R) -> R {
    // SAFETY: The retained owner remains borrowed for the returned view's lifetime.
    let units = match unsafe { ak::utf16_string_units(string.raw_word()) } {
        ak::Utf16StringUnits::Ascii(bytes) => StringUnits::Ascii(bytes),
        ak::Utf16StringUnits::Utf16(units) => StringUnits::Utf16(units),
    };
    f(units)
}

/// Iterates the code points of UTF-16 units the way AK::Utf16View does: surrogate pairs decode
/// to their code point, and unpaired surrogates yield their own code unit value.
pub(crate) fn for_each_code_point_utf16(units: &[u16], mut f: impl FnMut(u32)) {
    let mut index = 0;
    while index < units.len() {
        let unit = units[index];
        if (0xD800..=0xDBFF).contains(&unit)
            && let Some(&next) = units.get(index + 1)
            && (0xDC00..=0xDFFF).contains(&next)
        {
            let code_point = 0x10000 + ((u32::from(unit) - 0xD800) << 10) + (u32::from(next) - 0xDC00);
            f(code_point);
            index += 2;
            continue;
        }
        f(u32::from(unit));
        index += 1;
    }
}

/// An ASCII-until-proven-otherwise text accumulator with the shape AK::Utf16String wants.
pub(crate) struct TextSink {
    ascii: Vec<u8>,
    utf16: Vec<u16>,
    is_ascii: bool,
}

impl TextSink {
    pub(crate) fn new() -> Self {
        Self {
            ascii: Vec::new(),
            utf16: Vec::new(),
            is_ascii: true,
        }
    }

    /// The current length in code units.
    fn len(&self) -> usize {
        if self.is_ascii {
            self.ascii.len()
        } else {
            self.utf16.len()
        }
    }

    fn promote_to_utf16(&mut self) {
        debug_assert!(self.is_ascii);
        self.utf16 = self.ascii.iter().map(|&byte| u16::from(byte)).collect();
        self.ascii = Vec::new();
        self.is_ascii = false;
    }

    pub(crate) fn push_ascii(&mut self, text: &str) {
        debug_assert!(text.is_ascii());
        if self.is_ascii {
            self.ascii.extend_from_slice(text.as_bytes());
        } else {
            self.utf16.extend(text.bytes().map(u16::from));
        }
    }

    pub(crate) fn push_code_unit(&mut self, unit: u16) {
        if self.is_ascii {
            if unit < 0x80 {
                self.ascii.push(unit as u8);
                return;
            }
            self.promote_to_utf16();
        }
        self.utf16.push(unit);
    }

    pub(crate) fn into_utf16(self) -> Vec<u16> {
        if self.is_ascii {
            self.ascii.into_iter().map(u16::from).collect()
        } else {
            self.utf16
        }
    }

    /// Appends one code point, encoding non-BMP code points as surrogate pairs. Values in the
    /// surrogate range append as a single (unpaired) code unit, matching AK's builders.
    pub(crate) fn push_code_point(&mut self, code_point: u32) {
        if code_point < 0x10000 {
            self.push_code_unit(code_point as u16);
            return;
        }
        let value = code_point - 0x10000;
        self.push_code_unit(0xD800 + (value >> 10) as u16);
        self.push_code_unit(0xDC00 + (value & 0x3FF) as u16);
    }

    /// Appends another sink's contents.
    pub(crate) fn push_sink(&mut self, other: &TextSink) {
        if other.is_ascii {
            if self.is_ascii {
                self.ascii.extend_from_slice(&other.ascii);
            } else {
                self.utf16.extend(other.ascii.iter().map(|&byte| u16::from(byte)));
            }
            return;
        }
        if self.is_ascii {
            self.promote_to_utf16();
        }
        self.utf16.extend_from_slice(&other.utf16);
    }

    /// Content equality across storage representations.
    pub(crate) fn content_equals(&self, other: &TextSink) -> bool {
        match (self.is_ascii, other.is_ascii) {
            (true, true) => self.ascii == other.ascii,
            (false, false) => self.utf16 == other.utf16,
            (true, false) => {
                other.utf16.len() == self.ascii.len()
                    && self
                        .ascii
                        .iter()
                        .zip(&other.utf16)
                        .all(|(&byte, &unit)| u16::from(byte) == unit)
            }
            (false, true) => other.content_equals(self),
        }
    }

    fn content_equals_ascii_case_insensitive(&self, other: &TextSink) -> bool {
        self.is_ascii && other.is_ascii && self.ascii.eq_ignore_ascii_case(&other.ascii)
    }

    fn into_string(self) -> String {
        if self.is_ascii {
            // SAFETY: The ASCII representation contains only valid UTF-8.
            unsafe { String::from_utf8_unchecked(self.ascii) }
        } else {
            String::from_utf16_lossy(&self.utf16)
        }
    }
}

pub(crate) fn serialize_computed_size(size: &crate::css::computed_value_types::ComputedSize) -> String {
    use crate::css::computed_value_types::ComputedSizeKind;

    let mut sink = TextSink::new();
    match size.kind {
        ComputedSizeKind::Auto => sink.push_ascii("auto"),
        ComputedSizeKind::Calculated | ComputedSizeKind::Length | ComputedSizeKind::Percentage => {
            // SAFETY: These computed size kinds retain a live style value.
            let value = unsafe { &*size.value.pointer.cast::<StyleValueData>() };
            assert!(serialize_style_value(&mut sink, value, SerializationMode::Normal));
        }
        ComputedSizeKind::MinContent => sink.push_ascii("min-content"),
        ComputedSizeKind::MaxContent => sink.push_ascii("max-content"),
        ComputedSizeKind::FitContent => {
            if size.value.pointer.is_null() {
                sink.push_ascii("fit-content");
            } else {
                sink.push_ascii("fit-content(");
                // SAFETY: A non-keyword fit-content size retains its live argument value.
                let value = unsafe { &*size.value.pointer.cast::<StyleValueData>() };
                assert!(serialize_style_value(&mut sink, value, SerializationMode::Normal));
                sink.push_ascii(")");
            }
        }
        ComputedSizeKind::None => sink.push_ascii("none"),
    }
    sink.into_string()
}

pub(crate) fn serialize_style_value_to_utf16(value: &StyleValueData) -> Option<Vec<u16>> {
    let mut sink = TextSink::new();
    serialize_style_value(&mut sink, value, SerializationMode::Normal).then(|| sink.into_utf16())
}

/// Serializes UTF-16 text as a CSS string token's source text.
pub(crate) fn serialize_string(value: &[u16]) -> Vec<u16> {
    let mut sink = TextSink::new();
    serialize_a_string(&mut sink, &StringUnits::Utf16(value));
    sink.into_utf16()
}

pub(crate) fn fly_string_raw_to_string(raw: usize) -> String {
    // SAFETY: Callers hold a retained fly-string owner while converting the raw value.
    match unsafe { ak::utf16_string_units(&raw) } {
        ak::Utf16StringUnits::Ascii(bytes) => {
            // SAFETY: ASCII is valid UTF-8.
            unsafe { String::from_utf8_unchecked(bytes.to_vec()) }
        }
        ak::Utf16StringUnits::Utf16(units) => String::from_utf16_lossy(units),
    }
}

/// https://www.w3.org/TR/cssom-1/#escape-a-character
fn escape_a_character(sink: &mut TextSink, code_point: u32) {
    sink.push_ascii("\\");
    sink.push_code_point(code_point);
}

/// https://www.w3.org/TR/cssom-1/#escape-a-character-as-code-point
fn escape_a_character_as_code_point(sink: &mut TextSink, code_point: u32) {
    sink.push_ascii(&format!("\\{code_point:x} "));
}

/// https://www.w3.org/TR/cssom-1/#serialize-an-identifier
pub(crate) fn serialize_an_identifier(sink: &mut TextSink, ident: &StringUnits) {
    let mut code_points = Vec::new();
    match ident {
        StringUnits::Ascii(bytes) => code_points.extend(bytes.iter().map(|&byte| u32::from(byte))),
        StringUnits::Utf16(units) => for_each_code_point_utf16(units, |code_point| code_points.push(code_point)),
    }

    let first_character = code_points.first().copied().unwrap_or(0);
    for (character_index, &character) in code_points.iter().enumerate() {
        // If the character is NULL (U+0000), then the REPLACEMENT CHARACTER (U+FFFD).
        if character == 0 {
            sink.push_code_point(0xFFFD);
            continue;
        }
        // If the character is in the range [\1-\1f] (U+0001 to U+001F) or is U+007F,
        // then the character escaped as code point.
        if (0x0001..=0x001F).contains(&character) || character == 0x007F {
            escape_a_character_as_code_point(sink, character);
            continue;
        }
        // If the character is the first character and is in the range [0-9],
        // then the character escaped as code point.
        if character_index == 0 && is_ascii_digit_code_point(character) {
            escape_a_character_as_code_point(sink, character);
            continue;
        }
        // If the character is the second character and is in the range [0-9]
        // and the first character is a "-", then the character escaped as code point.
        if character_index == 1 && first_character == u32::from(b'-') && is_ascii_digit_code_point(character) {
            escape_a_character_as_code_point(sink, character);
            continue;
        }
        // If the character is the first character and is a "-", and there is no second
        // character, then the escaped character.
        if character_index == 0 && character == u32::from(b'-') && code_points.len() == 1 {
            escape_a_character(sink, character);
            continue;
        }
        // If the character is greater than or equal to U+0080, is "-" or "_", or is in one of
        // the ranges [0-9], [A-Z], or [a-z], then the character itself.
        if character >= 0x0080
            || character == u32::from(b'-')
            || character == u32::from(b'_')
            || is_ascii_digit_code_point(character)
            || (u32::from(b'A')..=u32::from(b'Z')).contains(&character)
            || (u32::from(b'a')..=u32::from(b'z')).contains(&character)
        {
            sink.push_code_point(character);
            continue;
        }
        // Otherwise, the escaped character.
        escape_a_character(sink, character);
    }
}

fn is_ascii_digit_code_point(code_point: u32) -> bool {
    (u32::from(b'0')..=u32::from(b'9')).contains(&code_point)
}

/// https://www.w3.org/TR/cssom-1/#serialize-a-string
pub(crate) fn serialize_a_string(sink: &mut TextSink, string: &StringUnits) {
    sink.push_ascii("\"");
    let mut emit = |character: u32| {
        // If the character is NULL (U+0000), then the REPLACEMENT CHARACTER (U+FFFD).
        if character == 0 {
            sink.push_code_point(0xFFFD);
            return;
        }
        // If the character is in the range [\1-\1f] (U+0001 to U+001F) or is U+007F,
        // the character escaped as code point.
        if (0x0001..=0x001F).contains(&character) || character == 0x007F {
            escape_a_character_as_code_point(sink, character);
            return;
        }
        // If the character is '"' (U+0022) or "\" (U+005C), the escaped character.
        if character == 0x0022 || character == 0x005C {
            escape_a_character(sink, character);
            return;
        }
        // Otherwise, the character itself.
        sink.push_code_point(character);
    };
    match string {
        StringUnits::Ascii(bytes) => bytes.iter().for_each(|&byte| emit(u32::from(byte))),
        StringUnits::Utf16(units) => for_each_code_point_utf16(units, emit),
    }
    sink.push_ascii("\"");
}

/// Serializes a retained AK::String's UTF-8 bytes with the serialize-a-string rules.
fn serialize_a_string_utf8(sink: &mut TextSink, string: &RetainedString) {
    // SAFETY: AK::String contents are guaranteed valid UTF-8.
    let text = unsafe { std::str::from_utf8_unchecked(string.as_bytes()) };
    if text.is_ascii() {
        serialize_a_string(sink, &StringUnits::Ascii(text.as_bytes()));
        return;
    }
    let units: Vec<u16> = text.encode_utf16().collect();
    serialize_a_string(sink, &StringUnits::Utf16(&units));
}

/// Port of AK::FormatBuilder::put_f64_with_precision for `appendff("{:.6}", value)`: base 10,
/// no zero padding, default display mode, precision 6. The digit-production loop is replicated
/// exactly, quirks included, so Rust and C++ serialize identical text for every double.
fn format_double_with_precision_6(sink: &mut TextSink, value: f64, sign_always: bool) {
    format_double_with_precision(sink, value, 6, sign_always);
}

/// Port of AK::FormatBuilder::put_f64_with_precision for `appendff("{:.N}")`, parameterized
/// over the precision; the hue components serialize with four fraction digits.
fn format_double_with_precision(sink: &mut TextSink, mut value: f64, precision: usize, sign_always: bool) {
    let is_negative = value < 0.0;
    if is_negative {
        value = -value;
    }

    let mut integer_value = value as u64;
    value -= (value as i64) as f64;

    debug_assert!(precision <= 6);
    let mut fraction_digits: [u8; 6] = [0; 6];
    let mut fraction_length = 0usize;

    let mut epsilon = 0.5;
    for _ in 0..precision {
        epsilon /= 10.0;
    }

    for _ in 0..precision {
        if value - ((value as i64) as f64) < epsilon {
            break;
        }
        value *= 10.0;
        epsilon *= 10.0;

        if value > f64::from(u32::MAX) {
            value -= ((value as u64) - ((value as u64) % 10)) as f64;
        }

        fraction_digits[fraction_length] = b'0' + ((value as u32) % 10) as u8;
        fraction_length += 1;
    }

    // Round up if the following decimal is 5 or higher.
    if ((value * 10.0) as u64) % 10 >= 5 {
        let mut carried = true;
        for digit in fraction_digits[..fraction_length].iter_mut().rev() {
            if *digit != b'9' {
                *digit += 1;
                carried = false;
                break;
            }
            *digit = b'0';
        }
        if carried {
            integer_value += 1;
        }
    }

    while fraction_length > 0 && fraction_digits[fraction_length - 1] == b'0' {
        fraction_length -= 1;
    }

    if is_negative {
        sink.push_ascii("-");
    } else if sign_always {
        sink.push_ascii("+");
    }
    sink.push_ascii(&integer_value.to_string());
    if fraction_length > 0 {
        sink.push_ascii(".");
        for &digit in &fraction_digits[..fraction_length] {
            sink.push_code_unit(u16::from(digit));
        }
    }
}

/// https://www.w3.org/TR/cssom-1/#serialize-a-css-value, the <number> branch. Matches
/// Web::CSS::serialize_a_number including its small-value AD-HOC.
pub(crate) fn serialize_a_number(sink: &mut TextSink, value: f64) {
    // AD-HOC: If the number is small enough that it would not print any digits when rounded,
    // serialize it as 0.
    if value.abs() < 0.000_000_5 {
        sink.push_ascii("0");
        return;
    }
    // FIXME: Prevent scientific notation for large values. (Matches the C++ FIXME.)
    format_double_with_precision_6(sink, value, false);
}

fn serialize_a_dimension(sink: &mut TextSink, value: f64, unit_name: &str) {
    serialize_a_number(sink, value);
    sink.push_ascii(unit_name);
}

/// Mirrors Web::CSS::RequestURLModifier::to_utf16_string. Modifier types follow the C++
/// `RequestURLModifier::Type` declaration order: CrossOrigin, Integrity, ReferrerPolicy.
fn serialize_request_url_modifier(sink: &mut TextSink, modifier: &crate::css::style_value::RetainedRequestUrlModifier) {
    use crate::css::css_enums::{cross_origin_modifier_value, referrer_policy_modifier_value};
    match modifier.modifier_type() {
        0 => {
            sink.push_ascii("cross-origin(");
            sink.push_ascii(cross_origin_modifier_value::NAMES[modifier.enum_value() as usize]);
        }
        1 => {
            sink.push_ascii("integrity(");
            with_fly_string_units(modifier.string_value(), |units| serialize_a_string(sink, &units));
        }
        _ => {
            sink.push_ascii("referrer-policy(");
            sink.push_ascii(referrer_policy_modifier_value::NAMES[modifier.enum_value() as usize]);
        }
    }
    sink.push_ascii(")");
}

/// Serializes one style value into `sink`. Returns false when the value's serialization has not
/// been ported yet; the sink contents are unspecified in that case and must be discarded.
pub(crate) fn serialize_style_value(sink: &mut TextSink, value: &StyleValueData, mode: SerializationMode) -> bool {
    use crate::css::calc::{
        ANGLE_UNIT_CANONICAL_RATIOS, ANGLE_UNIT_NAMES, FLEX_UNIT_CANONICAL_RATIOS, FLEX_UNIT_NAMES,
        FREQUENCY_UNIT_CANONICAL_RATIOS, FREQUENCY_UNIT_NAMES, RESOLUTION_UNIT_CANONICAL_RATIOS, RESOLUTION_UNIT_NAMES,
        TIME_UNIT_CANONICAL_RATIOS, TIME_UNIT_NAMES,
    };
    use crate::css::style_compute::{LENGTH_UNIT_NAMES, absolute_length_to_px, px_length_unit};

    match value {
        StyleValueData::Keyword { keyword: code } => {
            sink.push_ascii(keyword::NAMES[*code as usize]);
            true
        }
        StyleValueData::Number { value } => {
            serialize_a_number(sink, *value);
            true
        }
        StyleValueData::Integer { value } => {
            sink.push_ascii(&value.to_string());
            true
        }
        StyleValueData::Percentage { value } => {
            serialize_a_number(sink, *value);
            sink.push_ascii("%");
            true
        }
        StyleValueData::Angle { value, unit } => {
            // https://drafts.csswg.org/cssom/#serialize-a-css-value -> <angle>
            if mode == SerializationMode::ResolvedValue {
                serialize_a_dimension(sink, value * ANGLE_UNIT_CANONICAL_RATIOS[*unit as usize], "deg");
            } else {
                serialize_a_dimension(sink, *value, ANGLE_UNIT_NAMES[*unit as usize]);
            }
            true
        }
        StyleValueData::Flex { value, unit } => {
            // AD-HOC: No spec definition, so copy the other <dimension> definitions.
            if mode == SerializationMode::ResolvedValue {
                serialize_a_dimension(sink, value * FLEX_UNIT_CANONICAL_RATIOS[*unit as usize], "fr");
            } else {
                serialize_a_dimension(sink, *value, FLEX_UNIT_NAMES[*unit as usize]);
            }
            true
        }
        StyleValueData::Frequency { value, unit } => {
            if mode == SerializationMode::ResolvedValue {
                serialize_a_dimension(sink, value * FREQUENCY_UNIT_CANONICAL_RATIOS[*unit as usize], "hz");
            } else {
                serialize_a_dimension(sink, *value, FREQUENCY_UNIT_NAMES[*unit as usize]);
            }
            true
        }
        StyleValueData::Time { value, unit } => {
            // AD-HOC: WPT expects us to serialize using the actual unit, like for other
            // dimensions. https://github.com/w3c/csswg-drafts/issues/12616
            if mode == SerializationMode::ResolvedValue {
                serialize_a_dimension(sink, value * TIME_UNIT_CANONICAL_RATIOS[*unit as usize], "s");
            } else {
                serialize_a_dimension(sink, *value, TIME_UNIT_NAMES[*unit as usize]);
            }
            true
        }
        StyleValueData::Resolution { value, unit } => {
            if mode == SerializationMode::ResolvedValue {
                serialize_a_dimension(sink, value * RESOLUTION_UNIT_CANONICAL_RATIOS[*unit as usize], "dppx");
            } else {
                serialize_a_dimension(sink, *value, RESOLUTION_UNIT_NAMES[*unit as usize]);
            }
            true
        }
        StyleValueData::Length { value, unit } => {
            // FIXME: Manually skip this for px so we avoid rounding errors in
            //        absolute_length_to_px. (Matches the C++ FIXME.)
            if mode == SerializationMode::ResolvedValue
                && *unit != px_length_unit()
                && let Some(px) = absolute_length_to_px(*value, *unit)
            {
                let rounded = crate::css::css_pixels::CssPixels::nearest_value_for(px).to_double();
                serialize_a_dimension(sink, rounded, "px");
            } else {
                serialize_a_dimension(sink, *value, LENGTH_UNIT_NAMES[*unit as usize]);
            }
            true
        }
        StyleValueData::String { string, .. } => {
            with_fly_string_units(string, |units| serialize_a_string(sink, &units));
            true
        }
        StyleValueData::CustomIdent { custom_ident } => {
            with_fly_string_units(custom_ident, |units| serialize_an_identifier(sink, &units));
            true
        }
        StyleValueData::UnicodeRange {
            min_code_point,
            max_code_point,
        } => {
            if min_code_point == max_code_point {
                sink.push_ascii(&format!("U+{min_code_point:X}"));
            } else {
                sink.push_ascii(&format!("U+{min_code_point:X}-{max_code_point:X}"));
            }
            true
        }
        StyleValueData::Ratio { numerator, denominator } => {
            let (Some(numerator), Some(denominator)) = (numerator.optional_data(), denominator.optional_data()) else {
                return false;
            };
            if !serialize_style_value(sink, numerator, mode) {
                return false;
            }
            sink.push_ascii(" / ");
            serialize_style_value(sink, denominator, mode)
        }
        StyleValueData::Url {
            url,
            url_type,
            modifiers,
        } => {
            // https://drafts.csswg.org/cssom-1/#serialize-a-url
            sink.push_ascii(if *url_type == 0 { "url(" } else { "src(" });
            serialize_a_string_utf8(sink, url);
            for modifier in modifiers.as_slice() {
                sink.push_ascii(" ");
                serialize_request_url_modifier(sink, modifier);
            }
            sink.push_ascii(")");
            true
        }
        StyleValueData::GuaranteedInvalid => true,
        StyleValueData::Edge { has_edge, edge, offset } => {
            use crate::css::css_enums::position_edge;
            if *has_edge {
                sink.push_ascii(position_edge::NAMES[*edge as usize]);
            }
            if let Some(offset) = offset.optional_data() {
                if *has_edge {
                    sink.push_ascii(" ");
                }
                if !serialize_style_value(sink, offset, mode) {
                    return false;
                }
            }
            true
        }
        StyleValueData::Rect {
            top,
            right,
            bottom,
            left,
        } => {
            sink.push_ascii("rect(");
            for (index, side) in [top, right, bottom, left].into_iter().enumerate() {
                if index > 0 {
                    sink.push_ascii(", ");
                }
                let Some(side) = side.optional_data() else {
                    return false;
                };
                if !serialize_style_value(sink, side, mode) {
                    return false;
                }
            }
            sink.push_ascii(")");
            true
        }
        StyleValueData::BorderRadius {
            is_elliptical: _,
            horizontal_radius,
            vertical_radius,
        } => {
            let (Some(horizontal), Some(vertical)) =
                (horizontal_radius.optional_data(), vertical_radius.optional_data())
            else {
                return false;
            };
            let mut horizontal_sink = TextSink::new();
            if !serialize_style_value(&mut horizontal_sink, horizontal, mode) {
                return false;
            }
            let mut vertical_sink = TextSink::new();
            if !serialize_style_value(&mut vertical_sink, vertical, mode) {
                return false;
            }
            sink.push_sink(&horizontal_sink);
            // NOTE: The C++ serializer compares the values, not their serializations; value
            //       equality is content equality for style value data, so this matches.
            if horizontal != vertical {
                sink.push_ascii(" ");
                sink.push_sink(&vertical_sink);
            }
            true
        }
        StyleValueData::BorderRadiusRect {
            top_left,
            top_right,
            bottom_right,
            bottom_left,
        } => {
            let corners = [top_left, top_right, bottom_right, bottom_left];
            let mut horizontal = Vec::with_capacity(4);
            let mut vertical = Vec::with_capacity(4);
            for corner in corners {
                let Some(StyleValueData::BorderRadius {
                    horizontal_radius,
                    vertical_radius,
                    ..
                }) = corner.optional_data()
                else {
                    return false;
                };
                let (Some(horizontal_radius), Some(vertical_radius)) =
                    (horizontal_radius.optional_data(), vertical_radius.optional_data())
                else {
                    return false;
                };
                horizontal.push(horizontal_radius);
                vertical.push(vertical_radius);
            }
            let mut horizontal_sink = TextSink::new();
            if !serialize_a_positional_value_list(&mut horizontal_sink, &horizontal, mode) {
                return false;
            }
            let mut vertical_sink = TextSink::new();
            if !serialize_a_positional_value_list(&mut vertical_sink, &vertical, mode) {
                return false;
            }
            sink.push_sink(&horizontal_sink);
            if !horizontal_sink.content_equals(&vertical_sink) {
                sink.push_ascii(" / ");
                sink.push_sink(&vertical_sink);
            }
            true
        }
        StyleValueData::Calculated { .. } => serialize_calculated(sink, value, mode),
        StyleValueData::Display { raw } => {
            serialize_display(sink, *raw);
            true
        }
        StyleValueData::ColorScheme {
            schemes,
            scheme_codes: _,
            only,
        } => {
            if schemes.as_slice().is_empty() {
                sink.push_ascii("normal");
                return true;
            }
            for (index, scheme) in schemes.as_slice().iter().enumerate() {
                if index > 0 {
                    sink.push_ascii(" ");
                }
                with_fly_string_units(scheme, |units| serialize_an_identifier(sink, &units));
            }
            if *only {
                sink.push_ascii(" only");
            }
            true
        }
        StyleValueData::GridAutoFlow { row, dense } => {
            if *row && !*dense {
                sink.push_ascii("row");
            } else if !*row {
                sink.push_ascii("column");
            }
            if *dense {
                if !*row {
                    sink.push_ascii(" ");
                }
                sink.push_ascii("dense");
            }
            true
        }
        StyleValueData::TextUnderlinePosition { horizontal, vertical } => {
            use crate::css::css_enums::{text_underline_position_horizontal, text_underline_position_vertical};
            let horizontal_auto = *horizontal == text_underline_position_horizontal::AUTO;
            let vertical_auto = *vertical == text_underline_position_vertical::AUTO;
            if horizontal_auto && vertical_auto {
                sink.push_ascii("auto");
            } else if vertical_auto {
                sink.push_ascii(text_underline_position_horizontal::NAMES[*horizontal as usize]);
            } else if horizontal_auto {
                sink.push_ascii(text_underline_position_vertical::NAMES[*vertical as usize]);
            } else {
                sink.push_ascii(text_underline_position_horizontal::NAMES[*horizontal as usize]);
                sink.push_ascii(" ");
                sink.push_ascii(text_underline_position_vertical::NAMES[*vertical as usize]);
            }
            true
        }
        StyleValueData::Position { edge_x, edge_y } => {
            let (Some(edge_x), Some(edge_y)) = (edge_x.optional_data(), edge_y.optional_data()) else {
                return false;
            };
            if !serialize_style_value(sink, edge_x, mode) {
                return false;
            }
            sink.push_ascii(" ");
            serialize_style_value(sink, edge_y, mode)
        }
        StyleValueData::ScrollbarColor {
            thumb_color,
            track_color,
        } => {
            let (Some(thumb_color), Some(track_color)) = (thumb_color.optional_data(), track_color.optional_data())
            else {
                return false;
            };
            if !serialize_style_value(sink, thumb_color, mode) {
                return false;
            }
            sink.push_ascii(" ");
            serialize_style_value(sink, track_color, mode)
        }
        StyleValueData::ScrollbarGutter { value } => {
            // CSS::ScrollbarGutter: auto, stable, both-edges.
            sink.push_ascii(match value {
                0 => "auto",
                1 => "stable",
                _ => "stable both-edges",
            });
            true
        }
        StyleValueData::Shadow {
            shadow_type,
            color,
            offset_x,
            offset_y,
            blur_radius,
            spread_distance,
            placement,
        } => {
            if let Some(color) = color.optional_data() {
                if !serialize_style_value(sink, color, mode) {
                    return false;
                }
                sink.push_ascii(" ");
            }
            let (Some(offset_x), Some(offset_y)) = (offset_x.optional_data(), offset_y.optional_data()) else {
                return false;
            };
            if !serialize_style_value(sink, offset_x, mode) {
                return false;
            }
            sink.push_ascii(" ");
            if !serialize_style_value(sink, offset_y, mode) {
                return false;
            }
            if let Some(blur_radius) = blur_radius.optional_data() {
                sink.push_ascii(" ");
                if !serialize_style_value(sink, blur_radius, mode) {
                    return false;
                }
            }
            // ShadowType::Normal is 0; text shadows have no spread.
            if *shadow_type == 0
                && let Some(spread_distance) = spread_distance.optional_data()
            {
                sink.push_ascii(" ");
                if !serialize_style_value(sink, spread_distance, mode) {
                    return false;
                }
            }
            // ShadowPlacement::Inner is 1.
            if *placement == 1 {
                sink.push_ascii(" inset");
            }
            true
        }
        StyleValueData::TextIndent {
            length_percentage,
            hanging,
            each_line,
        } => {
            let Some(length_percentage) = length_percentage.optional_data() else {
                return false;
            };
            if !serialize_style_value(sink, length_percentage, mode) {
                return false;
            }
            if *each_line {
                sink.push_ascii(" each-line");
            }
            if *hanging {
                sink.push_ascii(" hanging");
            }
            true
        }
        StyleValueData::OverflowClipMargin {
            has_visual_box,
            visual_box,
            offset,
        } => {
            use crate::css::css_enums::background_box;
            let Some(offset) = offset.optional_data() else {
                return false;
            };
            let is_default_box = *has_visual_box && *visual_box == background_box::PADDING_BOX;
            let is_zero_offset = matches!(offset, StyleValueData::Length { value, .. } if *value == 0.0);
            if !*has_visual_box || is_default_box {
                return serialize_style_value(sink, offset, mode);
            }
            sink.push_ascii(background_box::NAMES[*visual_box as usize]);
            if !is_zero_offset {
                sink.push_ascii(" ");
                return serialize_style_value(sink, offset, mode);
            }
            true
        }
        StyleValueData::OpacityValue { value } => {
            let Some(value) = value.optional_data() else {
                return false;
            };
            if let StyleValueData::Percentage { value } = value {
                serialize_a_number(sink, value * 0.01);
                return true;
            }
            serialize_style_value(sink, value, mode)
        }
        StyleValueData::OpenTypeTagged {
            mode: tagged_mode,
            tag,
            packed_tag: _,
            value,
        } => {
            with_fly_string_units(tag, |units| serialize_a_string(sink, &units));
            let Some(value) = value.optional_data() else {
                return false;
            };
            let mut value_sink = TextSink::new();
            if !serialize_style_value(&mut value_sink, value, mode) {
                return false;
            }
            // OpenTypeTaggedStyleValue::Mode: FontFeatureSettings is 0.
            if *tagged_mode == 0 {
                if !sink_is_ascii_str(&value_sink, "1") {
                    sink.push_ascii(" ");
                    sink.push_sink(&value_sink);
                }
            } else {
                sink.push_ascii(" ");
                sink.push_sink(&value_sink);
            }
            true
        }
        StyleValueData::Tuple { values } => {
            let mut first = true;
            for value in values.as_slice() {
                let Some(value) = value.optional_data() else {
                    continue;
                };
                if !first {
                    sink.push_ascii(" ");
                }
                first = false;
                if !serialize_style_value(sink, value, mode) {
                    return false;
                }
            }
            true
        }
        StyleValueData::ValueList {
            values,
            separator,
            collapsible,
        } => {
            let entries = values.as_slice();
            if entries.is_empty() {
                return true;
            }
            // StyleValueList::Separator: Space is 0, Comma is 1.
            let separator = if *separator == 0 { " " } else { ", " };
            let first_value = entries[0].optional_data();
            if separator == " "
                && *collapsible
                && let Some(first_value) = first_value
                && !matches!(first_value, StyleValueData::EmptyOptional)
                && entries
                    .iter()
                    .all(|value| value.optional_data().is_some_and(|value| value == first_value))
            {
                return serialize_style_value(sink, first_value, mode);
            }
            let mut first = true;
            for value in entries {
                let Some(value) = value.optional_data() else {
                    continue;
                };
                if matches!(value, StyleValueData::EmptyOptional) {
                    continue;
                }
                if !first {
                    sink.push_ascii(separator);
                }
                first = false;
                if !serialize_style_value(sink, value, mode) {
                    return false;
                }
            }
            true
        }
        StyleValueData::Function { name, value } => {
            with_fly_string_units(name, |units| push_units_raw(sink, &units));
            sink.push_ascii("(");
            let Some(value) = value.optional_data() else {
                return false;
            };
            if !serialize_style_value(sink, value, mode) {
                return false;
            }
            sink.push_ascii(")");
            true
        }
        StyleValueData::PendingSubstitution { .. } => true,
        StyleValueData::TreeCountingFunction { function, .. } => {
            // TreeCountingFunctionStyleValue::TreeCountingFunction: SiblingCount is 0.
            sink.push_ascii(if *function == 0 {
                "sibling-count()"
            } else {
                "sibling-index()"
            });
            true
        }
        StyleValueData::RepeatStyle { repeat_x, repeat_y } => {
            use crate::css::css_enums::repetition;
            if repeat_x == repeat_y {
                sink.push_ascii(repetition::NAMES[*repeat_x as usize]);
            } else if *repeat_x == repetition::REPEAT && *repeat_y == repetition::NO_REPEAT {
                sink.push_ascii("repeat-x");
            } else if *repeat_x == repetition::NO_REPEAT && *repeat_y == repetition::REPEAT {
                sink.push_ascii("repeat-y");
            } else {
                sink.push_ascii(repetition::NAMES[*repeat_x as usize]);
                sink.push_ascii(" ");
                sink.push_ascii(repetition::NAMES[*repeat_y as usize]);
            }
            true
        }
        StyleValueData::Superellipse { parameter } => {
            let Some(parameter) = parameter.optional_data() else {
                return false;
            };
            let number = match parameter {
                StyleValueData::Number { value } => Some(*value),
                _ => None,
            };
            if mode == SerializationMode::ResolvedValue
                && let Some(number) = number
            {
                let keyword = if number == 1.0 {
                    Some("round")
                } else if number == 2.0 {
                    Some("squircle")
                } else if number == f64::INFINITY {
                    Some("square")
                } else if number == 0.0 {
                    Some("bevel")
                } else if number == -1.0 {
                    Some("scoop")
                } else if number == f64::NEG_INFINITY {
                    Some("notch")
                } else {
                    None
                };
                if let Some(keyword) = keyword {
                    sink.push_ascii(keyword);
                    return true;
                }
            }
            sink.push_ascii("superellipse(");
            match number {
                Some(number) if number == f64::INFINITY => sink.push_ascii("infinity"),
                Some(number) if number == f64::NEG_INFINITY => sink.push_ascii("-infinity"),
                _ => {
                    if !serialize_style_value(sink, parameter, mode) {
                        return false;
                    }
                }
            }
            sink.push_ascii(")");
            true
        }
        StyleValueData::FontStyle {
            font_style,
            angle_value,
        } => {
            use crate::css::css_enums::font_style_keyword;
            let angle_sink = match angle_value.optional_data() {
                Some(angle) => {
                    let mut angle_sink = TextSink::new();
                    if !serialize_style_value(&mut angle_sink, angle, mode) {
                        return false;
                    }
                    Some(angle_sink)
                }
                None => None,
            };
            if let Some(angle_sink) = &angle_sink
                && *font_style == font_style_keyword::OBLIQUE
                && sink_is_ascii_str(angle_sink, "0deg")
            {
                sink.push_ascii("normal");
                return true;
            }
            sink.push_ascii(font_style_keyword::NAMES[*font_style as usize]);
            if let Some(angle_sink) = &angle_sink
                && !sink_is_ascii_str(angle_sink, "14deg")
            {
                sink.push_ascii(" ");
                sink.push_sink(angle_sink);
            }
            true
        }
        StyleValueData::Cursor { image, x, y } => {
            let Some(image) = image.optional_data() else {
                return false;
            };
            if !serialize_style_value(sink, image, mode) {
                return false;
            }
            if let Some(x) = x.optional_data() {
                sink.push_ascii(" ");
                if !serialize_style_value(sink, x, mode) {
                    return false;
                }
                let Some(y) = y.optional_data() else {
                    return false;
                };
                sink.push_ascii(" ");
                if !serialize_style_value(sink, y, mode) {
                    return false;
                }
            }
            true
        }
        StyleValueData::Image {
            url,
            url_type,
            url_modifiers,
            ..
        } => {
            sink.push_ascii(if *url_type == 0 { "url(" } else { "src(" });
            serialize_a_string_utf8(sink, url);
            for modifier in url_modifiers.as_slice() {
                sink.push_ascii(" ");
                serialize_request_url_modifier(sink, modifier);
            }
            sink.push_ascii(")");
            true
        }
        StyleValueData::Unresolved { source_text, .. } => {
            match source_text.as_units() {
                TokenizerInput::Ascii(units) => sink.push_ascii(unsafe { std::str::from_utf8_unchecked(units) }),
                TokenizerInput::Utf16(units) => {
                    for &code_unit in units {
                        sink.push_code_unit(code_unit);
                    }
                }
            }
            true
        }
        StyleValueData::BorderImageSlice {
            top,
            right,
            bottom,
            left,
            fill,
        } => {
            let (Some(top), Some(right), Some(bottom), Some(left)) = (
                top.optional_data(),
                right.optional_data(),
                bottom.optional_data(),
                left.optional_data(),
            ) else {
                return false;
            };
            if !serialize_style_value(sink, top, mode) {
                return false;
            }
            // NOTE: The C++ serializer compares values, which is content equality here.
            if !(top == right && top == bottom && top == left) {
                sink.push_ascii(" ");
                if !serialize_style_value(sink, right, mode) {
                    return false;
                }
                if top != bottom || right != left {
                    sink.push_ascii(" ");
                    if !serialize_style_value(sink, bottom, mode) {
                        return false;
                    }
                    if left != right {
                        sink.push_ascii(" ");
                        if !serialize_style_value(sink, left, mode) {
                            return false;
                        }
                    }
                }
            }
            if *fill {
                sink.push_ascii(" fill");
            }
            true
        }
        StyleValueData::Content { content, alt_text } => {
            let Some(content) = content.optional_data() else {
                return false;
            };
            if !serialize_style_value(sink, content, mode) {
                return false;
            }
            if let Some(alt_text) = alt_text.optional_data() {
                sink.push_ascii(" / ");
                if !serialize_style_value(sink, alt_text, mode) {
                    return false;
                }
            }
            true
        }
        StyleValueData::Counter {
            function,
            counter_name,
            counter_style,
            join_string,
        } => {
            let Some(counter_style) = counter_style.optional_data() else {
                return false;
            };
            // CounterStyleValue::CounterFunction: Counter is 0, Counters is 1.
            let is_counters = *function == 1;
            sink.push_ascii(if is_counters { "counters(" } else { "counter(" });
            with_fly_string_units(counter_name, |units| serialize_an_identifier(sink, &units));
            if is_counters {
                sink.push_ascii(", ");
                with_fly_string_units(join_string, |units| serialize_a_string(sink, &units));
            }
            let mut style_sink = TextSink::new();
            if !serialize_style_value(&mut style_sink, counter_style, mode) {
                return false;
            }
            if !sink_is_ascii_str(&style_sink, "decimal") {
                sink.push_ascii(", ");
                sink.push_sink(&style_sink);
            }
            sink.push_ascii(")");
            true
        }
        StyleValueData::CounterStyle {
            is_symbols,
            name,
            symbols_type,
            symbols,
        } => {
            use crate::css::css_enums::symbols_type as symbols_type_enum;
            if !*is_symbols {
                with_fly_string_units(name, |units| push_units_raw(sink, &units));
                return true;
            }
            sink.push_ascii("symbols(");
            if *symbols_type != symbols_type_enum::SYMBOLIC {
                sink.push_ascii(symbols_type_enum::NAMES[*symbols_type as usize]);
                sink.push_ascii(" ");
            }
            for (index, symbol) in symbols.as_slice().iter().enumerate() {
                if index > 0 {
                    sink.push_ascii(" ");
                }
                with_fly_string_units(symbol, |units| serialize_a_string(sink, &units));
            }
            sink.push_ascii(")");
            true
        }
        StyleValueData::CounterStyleSystem {
            kind,
            system,
            first_symbol,
            name,
        } => {
            use crate::css::css_enums::counter_style_system;
            match kind {
                0 => sink.push_ascii(counter_style_system::NAMES[*system as usize]),
                1 => {
                    sink.push_ascii("fixed");
                    if let Some(first_symbol) = first_symbol.optional_data() {
                        sink.push_ascii(" ");
                        if !serialize_style_value(sink, first_symbol, mode) {
                            return false;
                        }
                    }
                }
                _ => {
                    sink.push_ascii("extends ");
                    with_fly_string_units(name, |units| serialize_an_identifier(sink, &units));
                }
            }
            true
        }
        StyleValueData::CounterDefinitions { counter_definitions } => {
            for (index, definition) in counter_definitions.as_slice().iter().enumerate() {
                if index > 0 {
                    sink.push_ascii(" ");
                }
                if definition.is_reversed() {
                    sink.push_ascii("reversed(");
                    with_fly_string_units(definition.name(), |units| serialize_an_identifier(sink, &units));
                    sink.push_ascii(")");
                } else {
                    with_fly_string_units(definition.name(), |units| serialize_an_identifier(sink, &units));
                }
                if let Some(value) = definition.value().optional_data() {
                    sink.push_ascii(" ");
                    if !serialize_style_value(sink, value, mode) {
                        return false;
                    }
                }
            }
            true
        }
        StyleValueData::GridTrackPlacement {
            kind,
            value,
            has_name,
            name,
            ..
        } => {
            // GridTrackPlacement::Type: Auto is 0, Span is 1, AreaOrLine is 2.
            match kind {
                0 => sink.push_ascii("auto"),
                1 => {
                    sink.push_ascii("span");
                    let value = value.optional_data();
                    let is_one = matches!(value, Some(StyleValueData::Integer { value: 1 }));
                    if !(*has_name && is_one)
                        && let Some(value) = value
                    {
                        sink.push_ascii(" ");
                        if !serialize_style_value(sink, value, mode) {
                            return false;
                        }
                    }
                    if *has_name {
                        sink.push_ascii(" ");
                        with_fly_string_units(name, |units| serialize_an_identifier(sink, &units));
                    }
                }
                _ => {
                    if let Some(value) = value.optional_data() {
                        if !serialize_style_value(sink, value, mode) {
                            return false;
                        }
                        if *has_name {
                            sink.push_ascii(" ");
                            with_fly_string_units(name, |units| serialize_an_identifier(sink, &units));
                        }
                    } else if *has_name {
                        with_fly_string_units(name, |units| serialize_an_identifier(sink, &units));
                    }
                }
            }
            true
        }
        StyleValueData::RandomValueSharing {
            fixed_value,
            is_auto,
            has_name: _,
            name,
            element_shared,
        } => {
            if let Some(fixed_value) = fixed_value.optional_data() {
                sink.push_ascii("fixed ");
                return serialize_style_value(sink, fixed_value, mode);
            }
            let mut first = true;
            if !*is_auto {
                with_fly_string_units(name, |units| serialize_an_identifier(sink, &units));
                first = false;
            }
            if *element_shared {
                if !first {
                    sink.push_ascii(" ");
                }
                sink.push_ascii("element-shared");
            }
            true
        }
        StyleValueData::Anchor {
            has_anchor_name,
            anchor_name,
            anchor_side,
            fallback_value,
        } => {
            sink.push_ascii("anchor(");
            if *has_anchor_name {
                with_fly_string_units(anchor_name, |units| serialize_an_identifier(sink, &units));
                sink.push_ascii(" ");
            }
            let Some(anchor_side) = anchor_side.optional_data() else {
                return false;
            };
            if !serialize_style_value(sink, anchor_side, mode) {
                return false;
            }
            if let Some(fallback_value) = fallback_value.optional_data() {
                sink.push_ascii(", ");
                if !serialize_style_value(sink, fallback_value, mode) {
                    return false;
                }
            }
            sink.push_ascii(")");
            true
        }
        StyleValueData::AnchorSize {
            has_anchor_name,
            anchor_name,
            has_anchor_size,
            anchor_size,
            fallback_value,
        } => {
            use crate::css::css_enums::anchor_size as anchor_size_enum;
            sink.push_ascii("anchor-size(");
            if *has_anchor_name {
                with_fly_string_units(anchor_name, |units| serialize_an_identifier(sink, &units));
            }
            if *has_anchor_size {
                if *has_anchor_name {
                    sink.push_ascii(" ");
                }
                sink.push_ascii(anchor_size_enum::NAMES[*anchor_size as usize]);
            }
            if let Some(fallback_value) = fallback_value.optional_data() {
                if *has_anchor_name || *has_anchor_size {
                    sink.push_ascii(", ");
                }
                if !serialize_style_value(sink, fallback_value, mode) {
                    return false;
                }
            }
            sink.push_ascii(")");
            true
        }
        StyleValueData::BackgroundSize { size_x, size_y } => {
            let (Some(size_x), Some(size_y)) = (size_x.optional_data(), size_y.optional_data()) else {
                return false;
            };
            if value_has_auto(size_x) && value_has_auto(size_y) {
                sink.push_ascii("auto");
                return true;
            }
            if !serialize_style_value(sink, size_x, mode) {
                return false;
            }
            sink.push_ascii(" ");
            serialize_style_value(sink, size_y, mode)
        }
        StyleValueData::LightDark { light, dark, .. } => {
            // FIXME: We don't have enough information to determine the computed value here.
            //        (Matches the C++ FIXME.)
            let (Some(light), Some(dark)) = (light.optional_data(), dark.optional_data()) else {
                return false;
            };
            sink.push_ascii("light-dark(");
            if !serialize_style_value(sink, light, mode) {
                return false;
            }
            sink.push_ascii(", ");
            if !serialize_style_value(sink, dark, mode) {
                return false;
            }
            sink.push_ascii(")");
            true
        }
        StyleValueData::ContrastColor { color, .. } => {
            let Some(color) = color.optional_data() else {
                return false;
            };
            sink.push_ascii("contrast-color(");
            if !serialize_style_value(sink, color, mode) {
                return false;
            }
            sink.push_ascii(")");
            true
        }
        StyleValueData::Easing {
            kind,
            linear_stops,
            x1,
            y1,
            x2,
            y2,
            number_of_intervals,
            step_position,
        } => {
            use crate::css::css_enums::step_position as step_position_enum;
            match kind {
                // EasingStyleValue kinds: linear is 0, cubic-bezier is 1, steps is 2.
                0 => {
                    sink.push_ascii("linear(");
                    for (index, stop) in linear_stops.as_slice().iter().enumerate() {
                        if index > 0 {
                            sink.push_ascii(", ");
                        }
                        let Some(output) = stop.output().optional_data() else {
                            return false;
                        };
                        if !serialize_style_value(sink, output, mode) {
                            return false;
                        }
                        if let Some(input) = stop.input().optional_data() {
                            sink.push_ascii(" ");
                            if !serialize_style_value(sink, input, mode) {
                                return false;
                            }
                        }
                    }
                    sink.push_ascii(")");
                }
                1 => {
                    sink.push_ascii("cubic-bezier(");
                    for (index, coordinate) in [x1, y1, x2, y2].into_iter().enumerate() {
                        if index > 0 {
                            sink.push_ascii(", ");
                        }
                        let Some(coordinate) = coordinate.optional_data() else {
                            return false;
                        };
                        if !serialize_style_value(sink, coordinate, mode) {
                            return false;
                        }
                    }
                    sink.push_ascii(")");
                }
                _ => {
                    sink.push_ascii("steps(");
                    let Some(number_of_intervals) = number_of_intervals.optional_data() else {
                        return false;
                    };
                    if !serialize_style_value(sink, number_of_intervals, mode) {
                        return false;
                    }
                    if *step_position != step_position_enum::JUMP_END && *step_position != step_position_enum::END {
                        sink.push_ascii(", ");
                        sink.push_ascii(step_position_enum::NAMES[*step_position as usize]);
                    }
                    sink.push_ascii(")");
                }
            }
            true
        }
        StyleValueData::Filter {
            kind,
            color_operation,
            value,
        } => {
            let Some(value) = value.optional_data() else {
                return false;
            };
            // FilterStyleValue kinds: blur is 0, drop-shadow is 1, hue-rotate is 2, color is 3;
            // color operations follow Gfx::ColorFilterType.
            sink.push_ascii(match kind {
                0 => "blur(",
                1 => "drop-shadow(",
                2 => "hue-rotate(",
                _ => match color_operation {
                    0 => "brightness(",
                    1 => "contrast(",
                    2 => "grayscale(",
                    3 => "invert(",
                    4 => "opacity(",
                    5 => "saturate(",
                    _ => "sepia(",
                },
            });
            if !serialize_style_value(sink, value, mode) {
                return false;
            }
            sink.push_ascii(")");
            true
        }
        StyleValueData::RadialSize {
            component_count,
            is_extent_0,
            extent_0,
            value_0,
            is_extent_1,
            extent_1,
            value_1,
        } => {
            use crate::css::css_enums::radial_extent;
            let components = [(is_extent_0, extent_0, value_0), (is_extent_1, extent_1, value_1)];
            for (index, (is_extent, extent, value)) in
                components.into_iter().enumerate().take(*component_count as usize)
            {
                if index > 0 {
                    sink.push_ascii(" ");
                }
                if *is_extent {
                    sink.push_ascii(radial_extent::NAMES[*extent as usize]);
                } else {
                    let Some(value) = value.optional_data() else {
                        return false;
                    };
                    if !serialize_style_value(sink, value, mode) {
                        return false;
                    }
                }
            }
            true
        }
        StyleValueData::ColorInterpolationMethod {
            is_polar,
            color_space,
            hue_interpolation_method,
        } => {
            serialize_color_interpolation_method(sink, *is_polar, *color_space, *hue_interpolation_method);
            true
        }
        StyleValueData::LinearGradient {
            has_direction_value,
            direction_value,
            side_or_corner,
            color_stop_list,
            gradient_type,
            repeating,
            color_interpolation_method,
            color_syntax,
        } => {
            // LinearGradientStyleValue::GradientType: Standard is 0, WebKit is 1; SideOrCorner:
            // Top, Bottom, Left, Right, TopLeft, TopRight, BottomLeft, BottomRight.
            let is_webkit = *gradient_type == 1;
            let default_side = if is_webkit { 0 } else { 1 };
            let has_direction = *has_direction_value || *side_or_corner != default_side;
            if is_webkit {
                sink.push_ascii("-webkit-");
            }
            if *repeating {
                sink.push_ascii("repeating-");
            }
            sink.push_ascii("linear-gradient(");
            if has_direction {
                if let Some(direction_value) = direction_value.optional_data() {
                    if !serialize_style_value(sink, direction_value, mode) {
                        return false;
                    }
                } else {
                    if !is_webkit {
                        sink.push_ascii("to ");
                    }
                    sink.push_ascii(match side_or_corner {
                        0 => "top",
                        1 => "bottom",
                        2 => "left",
                        3 => "right",
                        4 => "left top",
                        5 => "right top",
                        6 => "left bottom",
                        _ => "right bottom",
                    });
                }
            }
            let mut method_sink = TextSink::new();
            let Some(has_color_space) = serialize_gradient_interpolation_method(
                &mut method_sink,
                color_interpolation_method,
                *color_syntax,
                mode,
            ) else {
                return false;
            };
            if has_color_space {
                if has_direction {
                    sink.push_ascii(" ");
                }
                sink.push_sink(&method_sink);
            }
            if has_direction || has_color_space {
                sink.push_ascii(", ");
            }
            if !serialize_color_stop_list(sink, color_stop_list, mode) {
                return false;
            }
            sink.push_ascii(")");
            true
        }
        StyleValueData::ConicGradient {
            from_angle,
            position,
            color_stop_list,
            repeating,
            color_interpolation_method,
            color_syntax,
        } => {
            if *repeating {
                sink.push_ascii("repeating-");
            }
            sink.push_ascii("conic-gradient(");
            let has_from_angle = from_angle.optional_data().is_some();
            let has_at_position = match position.optional_data() {
                Some(position) => match position_is_center(position, mode) {
                    Some(is_center) => !is_center,
                    None => return false,
                },
                None => false,
            };
            if let Some(from_angle) = from_angle.optional_data() {
                sink.push_ascii("from ");
                if !serialize_style_value(sink, from_angle, mode) {
                    return false;
                }
            }
            if has_at_position {
                if has_from_angle {
                    sink.push_ascii(" ");
                }
                sink.push_ascii("at ");
                if !serialize_style_value(sink, position.optional_data().unwrap(), mode) {
                    return false;
                }
            }
            let mut method_sink = TextSink::new();
            let Some(has_color_space) = serialize_gradient_interpolation_method(
                &mut method_sink,
                color_interpolation_method,
                *color_syntax,
                mode,
            ) else {
                return false;
            };
            if has_color_space {
                if has_from_angle || has_at_position {
                    sink.push_ascii(" ");
                }
                sink.push_sink(&method_sink);
            }
            if has_from_angle || has_at_position || has_color_space {
                sink.push_ascii(", ");
            }
            if !serialize_color_stop_list(sink, color_stop_list, mode) {
                return false;
            }
            sink.push_ascii(")");
            true
        }
        StyleValueData::RadialGradient {
            ending_shape: _,
            size,
            position,
            color_stop_list,
            repeating,
            color_interpolation_method,
            color_syntax,
        } => {
            if *repeating {
                sink.push_ascii("repeating-");
            }
            sink.push_ascii("radial-gradient(");
            let mut size_sink = TextSink::new();
            let has_size = match size.optional_data() {
                Some(size) => {
                    if !serialize_style_value(&mut size_sink, size, mode) {
                        return false;
                    }
                    !sink_is_ascii_str(&size_sink, "farthest-corner")
                }
                None => false,
            };
            let has_at_position = match position.optional_data() {
                Some(position) => match position_is_center(position, mode) {
                    Some(is_center) => !is_center,
                    None => return false,
                },
                None => false,
            };
            if has_size {
                sink.push_sink(&size_sink);
            }
            if has_at_position {
                if has_size {
                    sink.push_ascii(" ");
                }
                sink.push_ascii("at ");
                if !serialize_style_value(sink, position.optional_data().unwrap(), mode) {
                    return false;
                }
            }
            let mut method_sink = TextSink::new();
            let Some(has_color_space) = serialize_gradient_interpolation_method(
                &mut method_sink,
                color_interpolation_method,
                *color_syntax,
                mode,
            ) else {
                return false;
            };
            if has_color_space {
                if has_size || has_at_position {
                    sink.push_ascii(" ");
                }
                sink.push_sink(&method_sink);
            }
            if has_size || has_at_position || has_color_space {
                sink.push_ascii(", ");
            }
            if !serialize_color_stop_list(sink, color_stop_list, mode) {
                return false;
            }
            sink.push_ascii(")");
            true
        }
        StyleValueData::ImageSet { options } => {
            sink.push_ascii("image-set(");
            for (index, option) in options.as_slice().iter().enumerate() {
                if index > 0 {
                    sink.push_ascii(", ");
                }
                let [image, resolution] = option.values();
                let (Some(image), Some(resolution)) = (image.optional_data(), resolution.optional_data()) else {
                    return false;
                };
                if !serialize_style_value(sink, image, mode) {
                    return false;
                }
                sink.push_ascii(" ");
                if !serialize_style_value(sink, resolution, mode) {
                    return false;
                }
                if let Some(type_string) = option.type_string() {
                    sink.push_ascii(" type(\"");
                    with_fly_string_units(type_string, |units| push_escaped_for_json(sink, &units));
                    sink.push_ascii("\")");
                }
            }
            sink.push_ascii(")");
            true
        }
        StyleValueData::GridTemplateArea {
            grid_areas,
            row_count,
            column_count,
        } => {
            if *row_count == 0 {
                sink.push_ascii("none");
                return true;
            }
            for row in 0..*row_count {
                if row > 0 {
                    sink.push_ascii(" ");
                }
                let mut row_sink = TextSink::new();
                for column in 0..*column_count {
                    if column > 0 {
                        row_sink.push_ascii(" ");
                    }
                    match grid_areas.as_slice().iter().find(|area| area.covers_cell(row, column)) {
                        Some(area) => with_fly_string_units(area.name(), |units| push_units_raw(&mut row_sink, &units)),
                        None => row_sink.push_ascii("."),
                    }
                }
                serialize_a_string_sink(sink, &row_sink);
            }
            true
        }
        StyleValueData::GridTrackSizeList {
            is_subgrid,
            preserve_line_name_sets: _,
            entries,
        } => serialize_grid_track_size_list(sink, *is_subgrid, entries.as_slice(), mode),
        StyleValueData::BasicShape {
            kind,
            v0,
            v1,
            v2,
            v3,
            v4,
            fill_rule,
            points,
            path_string,
        } => {
            // BasicShape kinds: inset 0, xywh 1, rect 2, circle 3, ellipse 4, polygon 5, path 6;
            // fill rules follow Gfx::WindingRule (Nonzero 0, EvenOdd 1).
            let round_clause =
                |sink: &mut TextSink, radius: &crate::css::style_value::RetainedStyleValueData| -> bool {
                    let Some(radius) = radius.optional_data() else {
                        return true;
                    };
                    let mut radius_sink = TextSink::new();
                    if !serialize_style_value(&mut radius_sink, radius, mode) {
                        return false;
                    }
                    if !sink_is_ascii_str(&radius_sink, "0px") {
                        sink.push_ascii(" round ");
                        sink.push_sink(&radius_sink);
                    }
                    true
                };
            match kind {
                0 => {
                    let (Some(top), Some(right), Some(bottom), Some(left)) = (
                        v0.optional_data(),
                        v1.optional_data(),
                        v2.optional_data(),
                        v3.optional_data(),
                    ) else {
                        return false;
                    };
                    sink.push_ascii("inset(");
                    if !serialize_a_positional_value_list(sink, &[top, right, bottom, left], mode) {
                        return false;
                    }
                    if !round_clause(sink, v4) {
                        return false;
                    }
                    sink.push_ascii(")");
                }
                1 | 2 => {
                    sink.push_ascii(if *kind == 1 { "xywh(" } else { "rect(" });
                    for (index, side) in [v0, v1, v2, v3].into_iter().enumerate() {
                        if index > 0 {
                            sink.push_ascii(" ");
                        }
                        let Some(side) = side.optional_data() else {
                            return false;
                        };
                        if !serialize_style_value(sink, side, mode) {
                            return false;
                        }
                    }
                    if !round_clause(sink, v4) {
                        return false;
                    }
                    sink.push_ascii(")");
                }
                3 | 4 => {
                    let is_circle = *kind == 3;
                    sink.push_ascii(if is_circle { "circle(" } else { "ellipse(" });
                    let Some(radius) = v0.optional_data() else {
                        return false;
                    };
                    let mut radius_sink = TextSink::new();
                    if !serialize_style_value(&mut radius_sink, radius, mode) {
                        return false;
                    }
                    let omission_sentinel = if is_circle {
                        "closest-side"
                    } else {
                        "closest-side closest-side"
                    };
                    let has_radius = !sink_is_ascii_str(&radius_sink, omission_sentinel);
                    if has_radius {
                        sink.push_sink(&radius_sink);
                    }
                    if let Some(position) = v1.optional_data() {
                        if has_radius {
                            sink.push_ascii(" ");
                        }
                        sink.push_ascii("at ");
                        if !serialize_style_value(sink, position, mode) {
                            return false;
                        }
                    }
                    sink.push_ascii(")");
                }
                5 => {
                    sink.push_ascii("polygon(");
                    let mut first = true;
                    if *fill_rule == 1 {
                        sink.push_ascii("evenodd");
                        first = false;
                    }
                    for point in points.as_slice() {
                        if !first {
                            sink.push_ascii(", ");
                        }
                        first = false;
                        let [x, y] = point.values();
                        let (Some(x), Some(y)) = (x.optional_data(), y.optional_data()) else {
                            return false;
                        };
                        if !serialize_style_value(sink, x, mode) {
                            return false;
                        }
                        sink.push_ascii(" ");
                        if !serialize_style_value(sink, y, mode) {
                            return false;
                        }
                    }
                    sink.push_ascii(")");
                }
                _ => {
                    sink.push_ascii("path(");
                    if !(mode == SerializationMode::ResolvedValue && *fill_rule == 0) {
                        sink.push_ascii(if *fill_rule == 0 { "nonzero, " } else { "evenodd, " });
                    }
                    with_fly_string_units(path_string, |units| serialize_a_string(sink, &units));
                    sink.push_ascii(")");
                }
            }
            true
        }
        StyleValueData::Transformation {
            property,
            transform_function,
            values,
        } => {
            use crate::css::property_metadata::property_id;
            let entries = values.as_slice();
            let entry_data = |index: usize| entries.get(index).and_then(|value| value.optional_data());
            if *property == property_id::ROTATE {
                // TransformFunction codes are generated from TransformFunctions.json.
                let name = TRANSFORM_FUNCTION_NAMES[*transform_function as usize];
                match name {
                    "rotateX" | "rotateY" | "rotate" | "rotateZ" => {
                        if name == "rotateX" {
                            sink.push_ascii("x ");
                        } else if name == "rotateY" {
                            sink.push_ascii("y ");
                        }
                        let Some(angle) = entry_data(0) else {
                            return false;
                        };
                        return serialize_style_value(sink, angle, mode);
                    }
                    _ => {
                        // rotate3d: resolve the axis to plain numbers; calc axes fall back to C++.
                        let axis = |index: usize| match entry_data(index) {
                            Some(StyleValueData::Number { value }) => Some(*value),
                            None => Some(0.0),
                            _ => None,
                        };
                        let (Some(x), Some(y), Some(z), Some(angle)) = (axis(0), axis(1), axis(2), entry_data(3))
                        else {
                            return false;
                        };
                        if x > 0.0 && y == 0.0 && z == 0.0 {
                            sink.push_ascii("x ");
                            return serialize_style_value(sink, angle, mode);
                        }
                        if x == 0.0 && y > 0.0 && z == 0.0 {
                            sink.push_ascii("y ");
                            return serialize_style_value(sink, angle, mode);
                        }
                        if x == 0.0 && y == 0.0 && z > 0.0 {
                            return serialize_style_value(sink, angle, mode);
                        }
                        for value in [x, y, z] {
                            serialize_a_number(sink, value);
                            sink.push_ascii(" ");
                        }
                        return serialize_style_value(sink, angle, mode);
                    }
                }
            }
            if *property == property_id::SCALE {
                // Numbers and percentages resolve to plain numbers; calc components fall back.
                let resolve = |index: usize| -> Option<Option<TextSink>> {
                    match entry_data(index) {
                        None => Some(None),
                        Some(StyleValueData::Number { value }) => {
                            let mut resolved = TextSink::new();
                            serialize_a_number(&mut resolved, *value);
                            Some(Some(resolved))
                        }
                        Some(StyleValueData::Percentage { value }) => {
                            let mut resolved = TextSink::new();
                            serialize_a_number(&mut resolved, value * 0.01);
                            Some(Some(resolved))
                        }
                        Some(_) => None,
                    }
                };
                let (Some(Some(x)), Some(Some(y))) = (resolve(0), resolve(1)) else {
                    return false;
                };
                let z = match entries.len() {
                    3 if !matches!(entry_data(2), Some(StyleValueData::Number { value: v }) if *v == 1.0) => {
                        match resolve(2) {
                            Some(z) => z,
                            None => return false,
                        }
                    }
                    _ => None,
                };
                sink.push_sink(&x);
                let z_is_one = z.as_ref().is_none_or(|z| sink_is_ascii_str(z, "1"));
                if !x.content_equals(&y) || !z_is_one {
                    sink.push_ascii(" ");
                    sink.push_sink(&y);
                }
                if let Some(z) = &z
                    && !sink_is_ascii_str(z, "1")
                {
                    sink.push_ascii(" ");
                    sink.push_sink(z);
                }
                return true;
            }
            if *property == property_id::TRANSLATE {
                let resolve = |index: usize| -> Option<Option<TextSink>> {
                    let Some(value) = entry_data(index) else {
                        return Some(None);
                    };
                    let mut resolved = TextSink::new();
                    if !serialize_style_value(&mut resolved, value, mode) {
                        return None;
                    }
                    if sink_is_ascii_str(&resolved, "0px") {
                        return Some(None);
                    }
                    Some(Some(resolved))
                };
                let (Some(x), Some(y)) = (resolve(0), resolve(1)) else {
                    return false;
                };
                let z = if entries.len() == 3
                    && !matches!(entry_data(2), Some(StyleValueData::Length { value, .. }) if *value == 0.0)
                {
                    match resolve(2) {
                        Some(z) => z,
                        None => return false,
                    }
                } else {
                    None
                };
                match &x {
                    Some(x) => sink.push_sink(x),
                    None => sink.push_ascii("0px"),
                }
                if y.is_some() || z.is_some() {
                    sink.push_ascii(" ");
                    match &y {
                        Some(y) => sink.push_sink(y),
                        None => sink.push_ascii("0px"),
                    }
                }
                if let Some(z) = &z {
                    sink.push_ascii(" ");
                    sink.push_sink(z);
                }
                return true;
            }
            let name = TRANSFORM_FUNCTION_NAMES[*transform_function as usize];
            let scale_family = matches!(name, "scale" | "scale3d" | "scaleX" | "scaleY" | "scaleZ");
            sink.push_ascii(name);
            sink.push_ascii("(");
            for (index, value) in entries.iter().enumerate() {
                if index > 0 {
                    sink.push_ascii(", ");
                }
                let Some(value) = value.optional_data() else {
                    return false;
                };
                if scale_family && matches!(value, StyleValueData::Percentage { .. }) {
                    // The C++ serializer prints these through String::number, whose shortest
                    // round-trip formatting is not ported; decline so C++ serializes them.
                    return false;
                }
                if !serialize_style_value(sink, value, mode) {
                    return false;
                }
            }
            sink.push_ascii(")");
            true
        }
        StyleValueData::FontSource {
            is_local,
            local_name,
            url,
            url_type,
            url_modifiers,
            has_format,
            format,
            tech,
        } => {
            use crate::css::css_enums::font_tech;
            // NOTE: The C++ serializer ignores the mode, forcing Normal for local names.
            if *is_local {
                let Some(local_name) = local_name.optional_data() else {
                    return false;
                };
                sink.push_ascii("local(");
                if !serialize_style_value(sink, local_name, SerializationMode::Normal) {
                    return false;
                }
                sink.push_ascii(")");
                return true;
            }
            sink.push_ascii(if *url_type == 0 { "url(" } else { "src(" });
            serialize_a_string_utf8(sink, url);
            for modifier in url_modifiers.as_slice() {
                sink.push_ascii(" ");
                serialize_request_url_modifier(sink, modifier);
            }
            sink.push_ascii(")");
            if *has_format {
                sink.push_ascii(" format(");
                with_fly_string_units(format, |units| serialize_an_identifier(sink, &units));
                sink.push_ascii(")");
            }
            if !tech.as_slice().is_empty() {
                sink.push_ascii(" tech(");
                for (index, &tech_value) in tech.as_slice().iter().enumerate() {
                    if index > 0 {
                        sink.push_ascii(", ");
                    }
                    sink.push_ascii(font_tech::NAMES[tech_value as usize]);
                }
                sink.push_ascii(")");
            }
            true
        }
        StyleValueData::Shorthand {
            shorthand_property,
            sub_properties,
            values,
        } => serialize_shorthand(
            sink,
            value,
            *shorthand_property,
            sub_properties.as_slice(),
            values,
            mode,
        ),
        StyleValueData::ColorFunction {
            color_base,
            channel_0,
            channel_1,
            channel_2,
            alpha,
            has_name,
            name,
            origin_color,
        } => {
            use crate::css::color_resolution::{
                EMPTY_INPUT, SerializationBehavior, descriptor_for_color_type, to_color,
            };
            if !color_base.has_color_type {
                return false;
            }
            let descriptor = descriptor_for_color_type(color_base.color_type);
            let channels = [channel_0, channel_1, channel_2];

            // https://drafts.csswg.org/css-color-5/#serial-relative-color
            if let Some(origin) = origin_color.optional_data() {
                if descriptor.behavior == SerializationBehavior::ColorFunction {
                    sink.push_ascii("color(from ");
                    if !serialize_style_value(sink, origin, mode) {
                        return false;
                    }
                    sink.push_ascii(" ");
                    sink.push_ascii(descriptor.name);
                    sink.push_ascii(" ");
                } else {
                    sink.push_ascii(descriptor.name);
                    sink.push_ascii("(from ");
                    if !serialize_style_value(sink, origin, mode) {
                        return false;
                    }
                    sink.push_ascii(" ");
                }
                for (index, channel) in channels.into_iter().enumerate() {
                    if index > 0 {
                        sink.push_ascii(" ");
                    }
                    let Some(channel) = channel.optional_data() else {
                        return false;
                    };
                    if !serialize_style_value(sink, channel, mode) {
                        return false;
                    }
                }
                if let Some(alpha) = alpha.optional_data() {
                    sink.push_ascii(" / ");
                    if !serialize_style_value(sink, alpha, mode) {
                        return false;
                    }
                }
                sink.push_ascii(")");
                return true;
            }

            if matches!(
                descriptor.behavior,
                SerializationBehavior::SrgbLegacy | SerializationBehavior::SrgbModern
            ) {
                if mode != SerializationMode::ResolvedValue && *has_name {
                    with_fly_string_units(name, |units| push_units_ascii_lowercased(sink, &units));
                    return true;
                }
                // sRGB-equivalent shortcut: serialize through the resolved color when it
                // resolves cleanly.
                if let Some(color) = to_color(value, &EMPTY_INPUT) {
                    color.serialize_a_srgb_value(sink);
                    return true;
                }
            }

            // https://drafts.csswg.org/css-color-4/#serializing-color-function-values
            if descriptor.behavior == SerializationBehavior::ColorFunction {
                return serialize_color_function_form(sink, descriptor.name, &channels, alpha, mode);
            }

            sink.push_ascii(descriptor.name);
            sink.push_ascii("(");
            for (index, channel) in channels.into_iter().enumerate() {
                if index > 0 {
                    sink.push_ascii(" ");
                }
                let Some(channel) = channel.optional_data() else {
                    return false;
                };
                let channel_descriptor = &descriptor.channels[index];
                let serialized = if channel_descriptor.is_hue {
                    serialize_hue_component(sink, channel, mode)
                } else {
                    serialize_color_component(
                        sink,
                        channel,
                        f64::from(channel_descriptor.percent_reference),
                        channel_descriptor.serialize_clamp_min,
                        channel_descriptor.serialize_clamp_max,
                        mode,
                    )
                };
                if !serialized {
                    return false;
                }
            }
            if let Some(alpha) = alpha.optional_data()
                && alpha_should_be_serialized(alpha)
            {
                sink.push_ascii(" / ");
                if !serialize_alpha_component(sink, alpha, mode) {
                    return false;
                }
            }
            sink.push_ascii(")");
            true
        }
        StyleValueData::ColorMix {
            color_interpolation_method,
            first_color,
            first_percentage,
            second_color,
            second_percentage,
            ..
        } => {
            use crate::css::css_enums::rectangular_color_space;
            sink.push_ascii("color-mix(");
            if let Some(method) = color_interpolation_method.optional_data() {
                let is_default_oklab = matches!(
                    method,
                    StyleValueData::ColorInterpolationMethod { is_polar: false, color_space, .. }
                        if *color_space == rectangular_color_space::OKLAB
                );
                if !is_default_oklab {
                    if !serialize_style_value(sink, method, mode) {
                        return false;
                    }
                    sink.push_ascii(", ");
                }
            }
            let first = first_percentage.optional_data();
            let second = second_percentage.optional_data();
            let literal_percentage = |value: Option<&StyleValueData>| match value {
                Some(StyleValueData::Percentage { value }) => Some(*value),
                _ => None,
            };
            let is_calc = |value: Option<&StyleValueData>| matches!(value, Some(StyleValueData::Calculated { .. }));

            let Some(first_color) = first_color.optional_data() else {
                return false;
            };
            if !serialize_style_value(sink, first_color, mode) {
                return false;
            }
            match (first, second) {
                (Some(first_value), Some(_)) => {
                    let both_fifty =
                        literal_percentage(first) == Some(50.0) && literal_percentage(second) == Some(50.0);
                    if !both_fifty {
                        sink.push_ascii(" ");
                        if !serialize_style_value(sink, first_value, mode) {
                            return false;
                        }
                    }
                }
                (Some(first_value), None) => {
                    if literal_percentage(first) != Some(50.0) {
                        sink.push_ascii(" ");
                        if !serialize_style_value(sink, first_value, mode) {
                            return false;
                        }
                    }
                }
                (None, Some(_)) => {
                    if literal_percentage(second) != Some(50.0)
                        && let Some(second_literal) = literal_percentage(second)
                    {
                        sink.push_ascii(" ");
                        serialize_a_number(sink, 100.0 - second_literal);
                        sink.push_ascii("%");
                    }
                }
                (None, None) => {}
            }
            sink.push_ascii(", ");
            let Some(second_color) = second_color.optional_data() else {
                return false;
            };
            if !serialize_style_value(sink, second_color, mode) {
                return false;
            }
            match (first, second) {
                (Some(_), Some(second_value)) => {
                    let sum_is_hundred = match (literal_percentage(first), literal_percentage(second)) {
                        (Some(first_literal), Some(second_literal)) => first_literal + second_literal == 100.0,
                        _ => false,
                    };
                    if !sum_is_hundred {
                        sink.push_ascii(" ");
                        if !serialize_style_value(sink, second_value, mode) {
                            return false;
                        }
                    }
                }
                (Some(_) | None, None) => {}
                (None, Some(second_value)) => {
                    if literal_percentage(second) != Some(50.0) && is_calc(second) {
                        sink.push_ascii(" ");
                        if !serialize_style_value(sink, second_value, mode) {
                            return false;
                        }
                    }
                }
            }
            sink.push_ascii(")");
            true
        }
        _ => false,
    }
}

/// Serializes one numeric calc leaf using the `CalcNumericValue::leaf_parts` encoding:
/// 0 = number (with the Number::Type in `unit`), 1 = angle, 2 = flex, 3 = frequency,
/// 4 = length, 5 = percentage, 6 = resolution, 7 = time. Mirrors the piece consumer in
/// CalculatedStyleValue.cpp.
fn serialize_numeric_leaf(sink: &mut TextSink, numeric_kind: u8, value: f64, unit: u8, mode: SerializationMode) {
    use crate::css::calc::{
        ANGLE_UNIT_CANONICAL_RATIOS, ANGLE_UNIT_NAMES, FLEX_UNIT_CANONICAL_RATIOS, FLEX_UNIT_NAMES,
        FREQUENCY_UNIT_CANONICAL_RATIOS, FREQUENCY_UNIT_NAMES, RESOLUTION_UNIT_CANONICAL_RATIOS, RESOLUTION_UNIT_NAMES,
        TIME_UNIT_CANONICAL_RATIOS, TIME_UNIT_NAMES,
    };
    use crate::css::style_compute::{LENGTH_UNIT_NAMES, absolute_length_to_px, px_length_unit};

    let resolved = mode == SerializationMode::ResolvedValue;
    match numeric_kind {
        0 => serialize_number_with_type(sink, value, unit),
        1 if resolved => serialize_a_dimension(sink, value * ANGLE_UNIT_CANONICAL_RATIOS[unit as usize], "deg"),
        1 => serialize_a_dimension(sink, value, ANGLE_UNIT_NAMES[unit as usize]),
        2 if resolved => serialize_a_dimension(sink, value * FLEX_UNIT_CANONICAL_RATIOS[unit as usize], "fr"),
        2 => serialize_a_dimension(sink, value, FLEX_UNIT_NAMES[unit as usize]),
        3 if resolved => serialize_a_dimension(sink, value * FREQUENCY_UNIT_CANONICAL_RATIOS[unit as usize], "hz"),
        3 => serialize_a_dimension(sink, value, FREQUENCY_UNIT_NAMES[unit as usize]),
        4 => {
            if resolved
                && unit != px_length_unit()
                && let Some(px) = absolute_length_to_px(value, unit)
            {
                let rounded = crate::css::css_pixels::CssPixels::nearest_value_for(px).to_double();
                serialize_a_dimension(sink, rounded, "px");
            } else {
                serialize_a_dimension(sink, value, LENGTH_UNIT_NAMES[unit as usize]);
            }
        }
        5 => {
            serialize_a_number(sink, value);
            sink.push_ascii("%");
        }
        6 if resolved => serialize_a_dimension(sink, value * RESOLUTION_UNIT_CANONICAL_RATIOS[unit as usize], "dppx"),
        6 => serialize_a_dimension(sink, value, RESOLUTION_UNIT_NAMES[unit as usize]),
        7 if resolved => serialize_a_dimension(sink, value * TIME_UNIT_CANONICAL_RATIOS[unit as usize], "s"),
        7 => serialize_a_dimension(sink, value, TIME_UNIT_NAMES[unit as usize]),
        _ => unreachable!("invalid numeric leaf kind"),
    }
}

/// Port of Web::CSS::Number::serialize; `number_type` is the Number::Type discriminant
/// (0 = number, 1 = integer with explicit sign, 2 = integer).
fn serialize_number_with_type(sink: &mut TextSink, value: f64, number_type: u8) {
    if number_type == 1 {
        format_double_with_precision_6(sink, value, true);
        return;
    }
    if value == f64::INFINITY {
        sink.push_ascii("infinity");
        return;
    }
    if value == f64::NEG_INFINITY {
        sink.push_ascii("-infinity");
        return;
    }
    if value.is_nan() {
        sink.push_ascii("NaN");
        return;
    }
    serialize_a_number(sink, value);
}

/// https://drafts.csswg.org/css-values-4/#serialize-a-math-function, by walking the piece
/// batch the calc serializer in calc.rs produces; mirrors the consumer loop in
/// CalculatedStyleValue.cpp. Returns false when an embedded style value's serialization has
/// not been ported yet.
fn serialize_calculated(sink: &mut TextSink, value: &StyleValueData, mode: SerializationMode) -> bool {
    use crate::css::css_enums::channel_keyword;

    let pieces = crate::css::calc::serialize_math_function_pieces(value, mode == SerializationMode::ResolvedValue);
    let mut previous_piece_appended = false;
    for piece in &pieces {
        let start_length = sink.len();
        match piece.kind {
            0 => sink.push_ascii(literal_piece_text(piece.bytes, piece.length)),
            1 => serialize_numeric_leaf(sink, piece.numeric_kind, piece.value, piece.unit_or_channel, mode),
            2 => {
                // SAFETY: Style value pieces point at data retained by the calculation tree,
                // which outlives this walk.
                let child = unsafe { &*piece.style_value.cast::<StyleValueData>() };
                if !serialize_style_value(sink, child, mode) {
                    return false;
                }
            }
            3 => sink.push_ascii(channel_keyword::NAMES[piece.unit_or_channel as usize]),
            4 => {
                if previous_piece_appended {
                    sink.push_ascii(literal_piece_text(piece.bytes, piece.length));
                }
            }
            _ => unreachable!("invalid calc serialization piece kind"),
        }
        previous_piece_appended = sink.len() > start_length;
    }
    true
}

/// Literal pieces are produced from static ASCII strings in calc.rs.
fn literal_piece_text(bytes: *const u8, length: usize) -> &'static str {
    // SAFETY: Literal pieces borrow 'static ASCII strings.
    unsafe { std::str::from_utf8_unchecked(std::slice::from_raw_parts(bytes, length)) }
}

/// Port of Web::CSS::serialize_a_positional_value_list: 2 or 4 values with equal-serialization
/// suffixes dropped. Returns false when a value's serialization has not been ported yet.
fn serialize_a_positional_value_list(sink: &mut TextSink, values: &[&StyleValueData], mode: SerializationMode) -> bool {
    let mut serialized = Vec::with_capacity(values.len());
    for value in values {
        let mut sub_sink = TextSink::new();
        if !serialize_style_value(&mut sub_sink, value, mode) {
            return false;
        }
        serialized.push(sub_sink);
    }
    let emit = |sink: &mut TextSink, parts: &[&TextSink]| {
        for (index, part) in parts.iter().enumerate() {
            if index > 0 {
                sink.push_ascii(" ");
            }
            sink.push_sink(part);
        }
    };
    match serialized.as_slice() {
        [first, second] => {
            if first.content_equals(second) {
                emit(sink, &[first]);
            } else {
                emit(sink, &[first, second]);
            }
        }
        [first, second, third, fourth] => {
            if first.content_equals(second) && first.content_equals(third) && first.content_equals(fourth) {
                emit(sink, &[first]);
            } else if first.content_equals(third) && second.content_equals(fourth) {
                emit(sink, &[first, second]);
            } else if second.content_equals(fourth) {
                emit(sink, &[first, second, third]);
            } else {
                emit(sink, &[first, second, third, fourth]);
            }
        }
        _ => unreachable!("positional value lists have two or four values"),
    }
    true
}

/// Appends a fly string's code units verbatim, with no escaping.
fn push_units_raw(sink: &mut TextSink, units: &StringUnits) {
    match units {
        StringUnits::Ascii(bytes) => {
            for &byte in *bytes {
                sink.push_code_unit(u16::from(byte));
            }
        }
        StringUnits::Utf16(utf16) => {
            for &unit in *utf16 {
                sink.push_code_unit(unit);
            }
        }
    }
}

/// Whether a sink's contents equal an ASCII string.
fn sink_is_ascii_str(sink: &TextSink, text: &str) -> bool {
    debug_assert!(text.is_ascii());
    let mut expected = TextSink::new();
    expected.push_ascii(text);
    sink.content_equals(&expected)
}

/// Port of StyleValue::has_auto: the auto keyword.
fn value_has_auto(value: &StyleValueData) -> bool {
    matches!(value, StyleValueData::Keyword { keyword: code } if *code == keyword::AUTO)
}

/// Port of ColorInterpolationMethodStyleValue::serialize's body, shared by the gradients.
fn serialize_color_interpolation_method(
    sink: &mut TextSink,
    is_polar: bool,
    color_space: u8,
    hue_interpolation_method: u8,
) {
    use crate::css::css_enums::{hue_interpolation_method as hue_enum, polar_color_space, rectangular_color_space};
    sink.push_ascii("in ");
    if is_polar {
        sink.push_ascii(polar_color_space::NAMES[color_space as usize]);
        if hue_interpolation_method != hue_enum::SHORTER {
            sink.push_ascii(" ");
            sink.push_ascii(hue_enum::NAMES[hue_interpolation_method as usize]);
            sink.push_ascii(" hue");
        }
    } else {
        sink.push_ascii(rectangular_color_space::NAMES[color_space as usize]);
    }
}

/// Whether a gradient's interpolation-method value is the default for its color syntax
/// (ColorSyntax::Legacy is 0), mirroring default_color_interpolation_method.
fn interpolation_method_is_default(method: &StyleValueData, color_syntax: u8) -> bool {
    use crate::css::css_enums::rectangular_color_space;
    let default_space = if color_syntax == 0 {
        rectangular_color_space::SRGB
    } else {
        rectangular_color_space::OKLAB
    };
    matches!(method, StyleValueData::ColorInterpolationMethod { is_polar: false, color_space, .. } if *color_space == default_space)
}

/// Serializes a gradient's interpolation method when it differs from the syntax default.
/// Returns whether anything was emitted, or None to decline.
fn serialize_gradient_interpolation_method(
    sink: &mut TextSink,
    method: &crate::css::style_value::RetainedStyleValueData,
    color_syntax: u8,
    mode: SerializationMode,
) -> Option<bool> {
    let Some(method) = method.optional_data() else {
        return Some(false);
    };
    if interpolation_method_is_default(method, color_syntax) {
        return Some(false);
    }
    if !serialize_style_value(sink, method, mode) {
        return None;
    }
    Some(true)
}

/// Port of Web::CSS::serialize_color_stop_list.
fn serialize_color_stop_list(sink: &mut TextSink, stops: &RetainedColorStopList, mode: SerializationMode) -> bool {
    for (index, stop) in stops.as_slice().iter().enumerate() {
        if index > 0 {
            sink.push_ascii(", ");
        }
        let [transition_hint, color, position, second_position] = stop.values();
        if let Some(transition_hint) = transition_hint.optional_data() {
            if !serialize_style_value(sink, transition_hint, mode) {
                return false;
            }
            sink.push_ascii(", ");
        }
        let Some(color) = color.optional_data() else {
            return false;
        };
        if !serialize_style_value(sink, color, mode) {
            return false;
        }
        if let Some(position) = position.optional_data() {
            sink.push_ascii(" ");
            if !serialize_style_value(sink, position, mode) {
                return false;
            }
        }
        if let Some(second_position) = second_position.optional_data() {
            sink.push_ascii(" ");
            if !serialize_style_value(sink, second_position, mode) {
                return false;
            }
        }
    }
    true
}

/// Port of EdgeStyleValue::is_center; None means a component's serialization declined.
fn edge_is_center(edge: &StyleValueData, mode: SerializationMode) -> Option<bool> {
    use crate::css::css_enums::position_edge;
    let StyleValueData::Edge { has_edge, edge, offset } = edge else {
        return Some(false);
    };
    if *has_edge && *edge == position_edge::CENTER {
        return Some(true);
    }
    let Some(offset) = offset.optional_data() else {
        return Some(false);
    };
    let mut offset_sink = TextSink::new();
    if !serialize_style_value(&mut offset_sink, offset, mode) {
        return None;
    }
    Some(sink_is_ascii_str(&offset_sink, "50%"))
}

/// Port of PositionStyleValue::is_center; None means a component's serialization declined.
fn position_is_center(position: &StyleValueData, mode: SerializationMode) -> Option<bool> {
    let StyleValueData::Position { edge_x, edge_y } = position else {
        return Some(false);
    };
    let (Some(edge_x), Some(edge_y)) = (edge_x.optional_data(), edge_y.optional_data()) else {
        return Some(false);
    };
    Some(edge_is_center(edge_x, mode)? && edge_is_center(edge_y, mode)?)
}

/// Appends a fly string's units ASCII-lowercased, matching to_ascii_lowercase on names.
fn push_units_ascii_lowercased(sink: &mut TextSink, units: &StringUnits) {
    let mut emit = |unit: u16| {
        let lowered = if (u16::from(b'A')..=u16::from(b'Z')).contains(&unit) {
            unit + 0x20
        } else {
            unit
        };
        sink.push_code_unit(lowered);
    };
    match units {
        StringUnits::Ascii(bytes) => bytes.iter().for_each(|&byte| emit(u16::from(byte))),
        StringUnits::Utf16(utf16) => utf16.iter().for_each(|&unit| emit(unit)),
    }
}

/// Port of the file-local alpha_should_be_serialized in ColorFunctionStyleValue.cpp.
fn alpha_should_be_serialized(alpha: &StyleValueData) -> bool {
    match alpha {
        StyleValueData::Number { value } => *value < 1.0,
        StyleValueData::Percentage { value } => value * 0.01 < 1.0,
        _ => true,
    }
}

/// Port of ColorStyleValue::serialize_color_component.
fn serialize_color_component(
    sink: &mut TextSink,
    value: &StyleValueData,
    one_hundred_percent: f64,
    clamp_min: Option<f64>,
    clamp_max: Option<f64>,
    mode: SerializationMode,
) -> bool {
    use crate::css::color_resolution::{EMPTY_INPUT, resolve_with_reference_value};
    if matches!(value, StyleValueData::Keyword { keyword: code } if *code == keyword::NONE) {
        sink.push_ascii("none");
        return true;
    }
    if matches!(value, StyleValueData::Calculated { .. }) && mode == SerializationMode::Normal {
        return serialize_style_value(sink, value, mode);
    }
    match resolve_with_reference_value(value, one_hundred_percent, &EMPTY_INPUT) {
        Some(mut resolved) => {
            if let Some(clamp_min) = clamp_min {
                resolved = resolved.max(clamp_min);
            }
            if let Some(clamp_max) = clamp_max {
                resolved = resolved.min(clamp_max);
            }
            serialize_a_number(sink, resolved);
            true
        }
        None => serialize_style_value(sink, value, mode),
    }
}

/// Port of ColorStyleValue::serialize_alpha_component.
fn serialize_alpha_component(sink: &mut TextSink, value: &StyleValueData, mode: SerializationMode) -> bool {
    use crate::css::color_resolution::{EMPTY_INPUT, resolve_alpha};
    if matches!(value, StyleValueData::Keyword { keyword: code } if *code == keyword::NONE) {
        sink.push_ascii("none");
        return true;
    }
    if matches!(value, StyleValueData::Calculated { .. }) && mode == SerializationMode::Normal {
        return serialize_style_value(sink, value, mode);
    }
    match resolve_alpha(value, &EMPTY_INPUT) {
        Some(resolved) => {
            serialize_a_number(sink, resolved);
            true
        }
        None => serialize_style_value(sink, value, mode),
    }
}

/// Port of ColorStyleValue::serialize_hue_component, which prints through AK's four-digit
/// precision formatting instead of serialize_a_number.
fn serialize_hue_component(sink: &mut TextSink, value: &StyleValueData, mode: SerializationMode) -> bool {
    use crate::css::color_resolution::{EMPTY_INPUT, resolve_hue};
    if matches!(value, StyleValueData::Keyword { keyword: code } if *code == keyword::NONE) {
        sink.push_ascii("none");
        return true;
    }
    if matches!(value, StyleValueData::Calculated { .. }) && mode == SerializationMode::Normal {
        return serialize_style_value(sink, value, mode);
    }
    match resolve_hue(value, &EMPTY_INPUT) {
        Some(resolved) => {
            format_double_with_precision(sink, resolved, 4, false);
            true
        }
        None => serialize_style_value(sink, value, mode),
    }
}

/// Port of the color() serialization form with its percentage-to-number conversions and
/// alpha requirements.
fn serialize_color_function_form(
    sink: &mut TextSink,
    function_name: &str,
    channels: &[&crate::css::style_value::RetainedStyleValueData; 3],
    alpha: &crate::css::style_value::RetainedStyleValueData,
    mode: SerializationMode,
) -> bool {
    // Converts percentages (and, in resolved mode, calc percentages and numbers) to plain
    // numbers; other values serialize as themselves.
    enum Converted<'a> {
        Number(f64),
        Original(&'a StyleValueData),
    }
    fn convert_percentage(value: &StyleValueData, mode: SerializationMode) -> Converted<'_> {
        if let StyleValueData::Percentage { value } = value {
            return Converted::Number(value / 100.0);
        }
        if mode == SerializationMode::ResolvedValue && matches!(value, StyleValueData::Calculated { .. }) {
            if let Some(resolved) = crate::css::calc::resolve_calculated_percentage_with_channels(value, None, None) {
                let mut resolved_number = resolved / 100.0;
                if !resolved_number.is_finite() {
                    resolved_number = 0.0;
                }
                return Converted::Number(resolved_number);
            }
            if let Some(resolved) = crate::css::calc::resolve_calculated_number_with_channels(value, None, None) {
                return Converted::Number(resolved);
            }
        }
        Converted::Original(value)
    }
    let serialize_converted = |sink: &mut TextSink, converted: &Converted| -> bool {
        match converted {
            Converted::Number(number) => {
                serialize_a_number(sink, *number);
                true
            }
            Converted::Original(value) => serialize_style_value(sink, value, mode),
        }
    };

    // An omitted alpha is treated as 1 and not serialized. Like the C++ serializer, the
    // number checks see through the conversion to plain number values.
    fn alpha_number(converted: &Converted) -> Option<f64> {
        match converted {
            Converted::Number(number) => Some(*number),
            Converted::Original(StyleValueData::Number { value }) => Some(*value),
            Converted::Original(_) => None,
        }
    }
    let original_alpha = alpha.optional_data();
    let converted_alpha = original_alpha.map(|alpha| convert_percentage(alpha, mode));
    let is_alpha_required = match &converted_alpha {
        None => false,
        Some(converted) => alpha_number(converted).is_none_or(|number| number < 1.0),
    };
    let converted_alpha = match converted_alpha {
        Some(converted) if alpha_number(&converted).is_some_and(|number| number < 0.0) => Some(Converted::Number(0.0)),
        other => other,
    };

    sink.push_ascii("color(");
    sink.push_ascii(function_name);
    sink.push_ascii(" ");
    for (index, channel) in channels.iter().enumerate() {
        if index > 0 {
            sink.push_ascii(" ");
        }
        let Some(channel) = channel.optional_data() else {
            return false;
        };
        if !serialize_converted(sink, &convert_percentage(channel, mode)) {
            return false;
        }
    }
    if is_alpha_required && let Some(converted_alpha) = &converted_alpha {
        sink.push_ascii(" / ");
        if !serialize_converted(sink, converted_alpha) {
            return false;
        }
    }
    sink.push_ascii(")");
    true
}

/// Serializes a sink's accumulated contents with the serialize-a-string rules.
fn serialize_a_string_sink(sink: &mut TextSink, contents: &TextSink) {
    if contents.is_ascii {
        serialize_a_string(sink, &StringUnits::Ascii(&contents.ascii));
    } else {
        serialize_a_string(sink, &StringUnits::Utf16(&contents.utf16));
    }
}

/// Port of GridTrackSizeList::serialize and the ExplicitGridTrack serializers.
fn serialize_grid_track_size_list(
    sink: &mut TextSink,
    is_subgrid: bool,
    entries: &[crate::css::style_value::RetainedGridTrackEntry],
    mode: SerializationMode,
) -> bool {
    use crate::css::style_value::GridTrackEntryKind;

    if is_subgrid {
        sink.push_ascii("subgrid");
        if entries.is_empty() {
            return true;
        }
        sink.push_ascii(" ");
    } else if entries.is_empty() {
        sink.push_ascii("none");
        return true;
    }
    for (index, entry) in entries.iter().enumerate() {
        if index > 0 {
            sink.push_ascii(" ");
        }
        match entry.kind {
            GridTrackEntryKind::LineNames => {
                sink.push_ascii("[");
                for (name_index, name) in entry.names.as_slice().iter().enumerate() {
                    if name_index > 0 {
                        sink.push_ascii(" ");
                    }
                    with_fly_string_units(name, |units| serialize_an_identifier(sink, &units));
                }
                sink.push_ascii("]");
            }
            GridTrackEntryKind::Size => {
                let Some(size) = entry.size_value.optional_data() else {
                    return false;
                };
                if !serialize_style_value(sink, size, mode) {
                    return false;
                }
            }
            GridTrackEntryKind::MinMax => {
                let (Some(min), Some(max)) = (entry.min_value.optional_data(), entry.max_value.optional_data()) else {
                    return false;
                };
                sink.push_ascii("minmax(");
                if !serialize_style_value(sink, min, mode) {
                    return false;
                }
                sink.push_ascii(", ");
                if !serialize_style_value(sink, max, mode) {
                    return false;
                }
                sink.push_ascii(")");
            }
            GridTrackEntryKind::Repeat => {
                sink.push_ascii("repeat(");
                // GridRepeatType: AutoFit is 0, AutoFill is 1, Fixed is 2.
                match entry.repeat_type {
                    0 => sink.push_ascii("auto-fit"),
                    1 => sink.push_ascii("auto-fill"),
                    _ => {
                        let Some(repeat_count) = entry.repeat_count.optional_data() else {
                            return false;
                        };
                        if !serialize_style_value(sink, repeat_count, mode) {
                            return false;
                        }
                    }
                }
                sink.push_ascii(", ");
                if !serialize_grid_track_size_list(sink, entry.repeat_is_subgrid, entry.repeat_entries(), mode) {
                    return false;
                }
                sink.push_ascii(")");
            }
        }
    }
    true
}

/// Port of StringBuilder::append_escaped_for_json over UTF-16 code units.
fn push_escaped_for_json(sink: &mut TextSink, units: &StringUnits) {
    let mut emit = |unit: u16| match unit {
        0x08 => sink.push_ascii("\\b"),
        0x0A => sink.push_ascii("\\n"),
        0x09 => sink.push_ascii("\\t"),
        0x22 => sink.push_ascii("\\\""),
        0x5C => sink.push_ascii("\\\\"),
        0x00..=0x1F => sink.push_ascii(&format!("\\u{unit:04x}")),
        _ => sink.push_code_unit(unit),
    };
    match units {
        StringUnits::Ascii(bytes) => bytes.iter().for_each(|&byte| emit(u16::from(byte))),
        StringUnits::Utf16(utf16) => utf16.iter().for_each(|&unit| emit(unit)),
    }
}

/// Port of Web::CSS::Display::to_string, including to_keyword's short-form precedence.
fn serialize_display(sink: &mut TextSink, raw: u32) {
    use crate::css::css_enums::{display_box, display_inside, display_internal, display_outside};
    use crate::css::display::{DISPLAY_TAG_BOX, DISPLAY_TAG_INTERNAL, FfiDisplay};

    let display = FfiDisplay::from_raw(raw);
    match display.tag {
        DISPLAY_TAG_BOX => sink.push_ascii(display_box::NAMES[display.box_value as usize]),
        DISPLAY_TAG_INTERNAL => sink.push_ascii(display_internal::NAMES[display.internal as usize]),
        _ => {
            // NOTE: Following the precedence rules of "most backwards-compatible, then
            //       shortest", serialization of equivalent display values uses the short form.
            let outside = display.outside;
            let inside = display.inside;
            let short: Option<&str> = if display.list_item {
                (outside == display_outside::BLOCK && inside == display_inside::FLOW).then_some("list-item")
            } else if outside == display_outside::BLOCK {
                match inside {
                    _ if inside == display_inside::FLOW => Some("block"),
                    _ if inside == display_inside::FLOW_ROOT => Some("flow-root"),
                    _ if inside == display_inside::FLEX => Some("flex"),
                    _ if inside == display_inside::_WEBKIT_BOX => Some("-webkit-box"),
                    _ if inside == display_inside::GRID => Some("grid"),
                    _ if inside == display_inside::TABLE => Some("table"),
                    _ => None,
                }
            } else if outside == display_outside::INLINE {
                match inside {
                    _ if inside == display_inside::FLOW => Some("inline"),
                    _ if inside == display_inside::FLOW_ROOT => Some("inline-block"),
                    _ if inside == display_inside::FLEX => Some("inline-flex"),
                    _ if inside == display_inside::_WEBKIT_BOX => Some("-webkit-inline-box"),
                    _ if inside == display_inside::GRID => Some("inline-grid"),
                    _ if inside == display_inside::RUBY => Some("ruby"),
                    _ if inside == display_inside::TABLE => Some("inline-table"),
                    _ if inside == display_inside::MATH => Some("math"),
                    _ => None,
                }
            } else if outside == display_outside::RUN_IN && inside == display_inside::FLOW {
                Some("run-in")
            } else {
                None
            };
            if let Some(short) = short {
                sink.push_ascii(short);
                return;
            }
            let mut first = true;
            let mut part = |sink: &mut TextSink, text: &str| {
                if !first {
                    sink.push_ascii(" ");
                }
                first = false;
                sink.push_ascii(text);
            };
            if !(outside == display_outside::BLOCK && inside == display_inside::FLOW_ROOT) {
                part(sink, display_outside::NAMES[outside as usize]);
            }
            if inside != display_inside::FLOW {
                part(sink, display_inside::NAMES[inside as usize]);
            }
            if display.list_item {
                part(sink, "list-item");
            }
        }
    }
}

/// Port of ShorthandStyleValue::serialize. Property cases whose C++ serializer needs the
/// parser (the coordinating-value-list shorthands) or unported machinery decline.
fn serialize_shorthand(
    sink: &mut TextSink,
    whole_value: &StyleValueData,
    shorthand_property: u16,
    sub_properties: &[u16],
    values: &crate::css::style_value::RetainedStyleValueDataList,
    mode: SerializationMode,
) -> bool {
    use crate::css::property_metadata::{POSITIONAL_VALUE_LIST_SHORTHANDS, property_id, property_initial_value};
    use crate::css::style_compute::{expand_shorthands_with, value_is_css_wide_keyword};

    // If all the longhands are the same CSS-wide keyword, just return that once.
    let mut built_in_keyword: Option<u16> = None;
    let mut all_same_keyword = true;
    expand_shorthands_with(
        shorthand_property,
        (whole_value as *const StyleValueData).cast(),
        false,
        &mut |_, data, _| {
            // SAFETY: The expansion hands back live Rust-owned style value data.
            let value = unsafe { &*data.cast::<StyleValueData>() };
            if !value_is_css_wide_keyword(value) {
                all_same_keyword = false;
                return;
            }
            let StyleValueData::Keyword { keyword: code } = value else {
                all_same_keyword = false;
                return;
            };
            match built_in_keyword {
                None => built_in_keyword = Some(*code),
                Some(existing) if existing != *code => all_same_keyword = false,
                _ => {}
            }
        },
    );
    if let Some(code) = built_in_keyword {
        if all_same_keyword {
            sink.push_ascii(keyword::NAMES[code as usize]);
        }
        return true;
    }

    let longhand = |id: u16| -> Option<&StyleValueData> {
        let index = sub_properties.iter().position(|&sub| sub == id)?;
        values.as_slice().get(index)?.optional_data()
    };
    let property_is_shorthand = |id: u16| !crate::css::property_metadata::longhands_for_shorthand(id).is_empty();
    let initial_source = |id: u16| -> Option<&'static str> {
        if property_is_shorthand(id) {
            return None;
        }
        Some(property_initial_value(id))
    };
    let sub_sink = |value: &StyleValueData| -> Option<TextSink> {
        let mut serialized = TextSink::new();
        serialize_style_value(&mut serialized, value, mode).then_some(serialized)
    };

    let default_serialize = |sink: &mut TextSink| -> bool {
        let entries = values.as_slice();
        let first_value = entries.first().and_then(|value| value.optional_data());
        let Some(first_value) = first_value else {
            return false;
        };
        if entries
            .iter()
            .all(|value| value.optional_data().is_some_and(|value| value == first_value))
        {
            return serialize_style_value(sink, first_value, mode);
        }
        let mut first = true;
        for (index, value) in entries.iter().enumerate() {
            let Some(value) = value.optional_data() else {
                return false;
            };
            let Some(initial_source) = initial_source(sub_properties[index]) else {
                return false;
            };
            let Some(value_sink) = sub_sink(value) else {
                return false;
            };
            let mut initial_sink = TextSink::new();
            initial_sink.push_ascii(initial_source);
            if value_sink.content_equals(&initial_sink)
                || value_sink.content_equals_ascii_case_insensitive(&initial_sink)
            {
                continue;
            }
            if first {
                first = false;
            } else {
                sink.push_ascii(" ");
            }
            sink.push_sink(&value_sink);
        }
        // NOTE: Like the C++ serializer, this checks the whole builder, not just this
        //       shorthand's own output.
        if sink.len() == 0 {
            return serialize_style_value(sink, first_value, mode);
        }
        true
    };

    if shorthand_property == property_id::ALL {
        // NOTE: 'all' can only be serialized in the case all sub-properties share the same
        //       CSS-wide keyword, which was handled above; serialize the empty string.
        return true;
    }
    if shorthand_property == property_id::BORDER_IMAGE {
        for (index, (id, separator)) in [
            (property_id::BORDER_IMAGE_SOURCE, ""),
            (property_id::BORDER_IMAGE_SLICE, " "),
            (property_id::BORDER_IMAGE_WIDTH, " / "),
            (property_id::BORDER_IMAGE_OUTSET, " / "),
            (property_id::BORDER_IMAGE_REPEAT, " "),
        ]
        .into_iter()
        .enumerate()
        {
            if index > 0 {
                sink.push_ascii(separator);
            }
            let Some(value) = longhand(id) else {
                return false;
            };
            if !serialize_style_value(sink, value, mode) {
                return false;
            }
        }
        return true;
    }
    if shorthand_property == property_id::BORDER_RADIUS {
        let corners = [
            longhand(property_id::BORDER_TOP_LEFT_RADIUS),
            longhand(property_id::BORDER_TOP_RIGHT_RADIUS),
            longhand(property_id::BORDER_BOTTOM_RIGHT_RADIUS),
            longhand(property_id::BORDER_BOTTOM_LEFT_RADIUS),
        ];
        let radius_sink = |corner: Option<&StyleValueData>, vertical: bool| -> Option<TextSink> {
            let corner = corner?;
            if let StyleValueData::BorderRadius {
                horizontal_radius,
                vertical_radius,
                ..
            } = corner
            {
                let radius = if vertical { vertical_radius } else { horizontal_radius };
                return sub_sink(radius.optional_data()?);
            }
            sub_sink(corner)
        };
        let serialize_radius = |sink: &mut TextSink, values: &[TextSink; 4]| {
            let [top_left, top_right, bottom_right, bottom_left] = values;
            let parts: &[&TextSink] = if top_left.content_equals(top_right)
                && top_left.content_equals(bottom_right)
                && top_left.content_equals(bottom_left)
            {
                &[top_left]
            } else if top_left.content_equals(bottom_right) && top_right.content_equals(bottom_left) {
                &[top_left, top_right]
            } else if top_right.content_equals(bottom_left) {
                &[top_left, top_right, bottom_right]
            } else {
                &[top_left, top_right, bottom_right, bottom_left]
            };
            for (index, part) in parts.iter().enumerate() {
                if index > 0 {
                    sink.push_ascii(" ");
                }
                sink.push_sink(part);
            }
        };
        let mut horizontal = Vec::with_capacity(4);
        let mut vertical = Vec::with_capacity(4);
        for corner in corners {
            horizontal.push(match radius_sink(corner, false) {
                Some(serialized) => serialized,
                None => return false,
            });
            vertical.push(match radius_sink(corner, true) {
                Some(serialized) => serialized,
                None => return false,
            });
        }
        let horizontal: [TextSink; 4] = horizontal.try_into().ok().unwrap();
        let vertical: [TextSink; 4] = vertical.try_into().ok().unwrap();
        let mut horizontal_sink = TextSink::new();
        serialize_radius(&mut horizontal_sink, &horizontal);
        let mut vertical_sink = TextSink::new();
        serialize_radius(&mut vertical_sink, &vertical);
        sink.push_sink(&horizontal_sink);
        if !horizontal_sink.content_equals(&vertical_sink) {
            sink.push_ascii(" / ");
            sink.push_sink(&vertical_sink);
        }
        return true;
    }
    if shorthand_property == property_id::COLUMNS {
        let (Some(width), Some(count), Some(height)) = (
            longhand(property_id::COLUMN_WIDTH).and_then(&sub_sink),
            longhand(property_id::COLUMN_COUNT).and_then(&sub_sink),
            longhand(property_id::COLUMN_HEIGHT).and_then(&sub_sink),
        ) else {
            return false;
        };
        if width.content_equals(&count) {
            sink.push_sink(&width);
        } else if sink_is_ascii_str(&width, "auto") {
            sink.push_sink(&count);
        } else if sink_is_ascii_str(&count, "auto") {
            sink.push_sink(&width);
        } else {
            sink.push_sink(&width);
            sink.push_ascii(" ");
            sink.push_sink(&count);
        }
        if !sink_is_ascii_str(&height, "auto") {
            sink.push_ascii(" / ");
            sink.push_sink(&height);
        }
        return true;
    }
    if shorthand_property == property_id::FLEX {
        for (index, id) in [
            property_id::FLEX_GROW,
            property_id::FLEX_SHRINK,
            property_id::FLEX_BASIS,
        ]
        .into_iter()
        .enumerate()
        {
            if index > 0 {
                sink.push_ascii(" ");
            }
            let Some(value) = longhand(id) else {
                return false;
            };
            if !serialize_style_value(sink, value, mode) {
                return false;
            }
        }
        return true;
    }
    if shorthand_property == property_id::CONTAINER {
        let (Some(name), Some(container_type)) = (
            longhand(property_id::CONTAINER_NAME),
            longhand(property_id::CONTAINER_TYPE),
        ) else {
            return false;
        };
        if !serialize_style_value(sink, name, mode) {
            return false;
        }
        let Some(container_type_initial) = initial_source(property_id::CONTAINER_TYPE) else {
            return false;
        };
        let Some(container_type_sink) = sub_sink(container_type) else {
            return false;
        };
        let mut container_type_initial_sink = TextSink::new();
        container_type_initial_sink.push_ascii(container_type_initial);
        if !container_type_sink.content_equals(&container_type_initial_sink) {
            sink.push_ascii(" / ");
            if !serialize_style_value(sink, container_type, mode) {
                return false;
            }
        }
        return true;
    }
    if shorthand_property == property_id::GRID_COLUMN || shorthand_property == property_id::GRID_ROW {
        let (start_id, end_id) = if shorthand_property == property_id::GRID_COLUMN {
            (property_id::GRID_COLUMN_START, property_id::GRID_COLUMN_END)
        } else {
            (property_id::GRID_ROW_START, property_id::GRID_ROW_END)
        };
        let (Some(start), Some(end)) = (longhand(start_id), longhand(end_id)) else {
            return false;
        };
        let end_is_auto_placement = matches!(end, StyleValueData::GridTrackPlacement { kind: 0, .. });
        if end_is_auto_placement || start == end {
            return serialize_style_value(sink, start, mode);
        }
        if !serialize_style_value(sink, start, mode) {
            return false;
        }
        sink.push_ascii(" / ");
        return serialize_style_value(sink, end, mode);
    }
    if shorthand_property == property_id::PLACE_CONTENT
        || shorthand_property == property_id::PLACE_ITEMS
        || shorthand_property == property_id::PLACE_SELF
    {
        let entries: Vec<&StyleValueData> = match values.as_slice().iter().map(|value| value.optional_data()).collect()
        {
            Some(entries) => entries,
            None => return false,
        };
        return serialize_a_positional_value_list(sink, &entries, mode);
    }
    if shorthand_property == property_id::TEXT_DECORATION {
        // The rule here seems to be, only print what's different from the default value,
        // but if they're all default, print the line.
        for id in [
            property_id::TEXT_DECORATION_LINE,
            property_id::TEXT_DECORATION_THICKNESS,
            property_id::TEXT_DECORATION_STYLE,
            property_id::TEXT_DECORATION_COLOR,
        ] {
            let (Some(value), Some(initial_source)) = (longhand(id), initial_source(id)) else {
                return false;
            };
            let Some(value_sink) = sub_sink(value) else {
                return false;
            };
            let mut initial_sink = TextSink::new();
            initial_sink.push_ascii(initial_source);
            if !value_sink.content_equals(&initial_sink) {
                if sink.len() != 0 {
                    sink.push_ascii(" ");
                }
                if !serialize_style_value(sink, value, mode) {
                    return false;
                }
            }
        }
        if sink.len() == 0 {
            let Some(line) = longhand(property_id::TEXT_DECORATION_LINE) else {
                return false;
            };
            return serialize_style_value(sink, line, mode);
        }
        return true;
    }
    if shorthand_property == property_id::WHITE_SPACE {
        let (Some(collapse), Some(wrap_mode), Some(trim)) = (
            longhand(property_id::WHITE_SPACE_COLLAPSE),
            longhand(property_id::TEXT_WRAP_MODE),
            longhand(property_id::WHITE_SPACE_TRIM),
        ) else {
            return false;
        };
        if matches!(trim, StyleValueData::Keyword { keyword: code } if *code == keyword::NONE)
            && let (StyleValueData::Keyword { keyword: collapse }, StyleValueData::Keyword { keyword: wrap_mode }) =
                (collapse, wrap_mode)
        {
            let short = match (*collapse, *wrap_mode) {
                (collapse, wrap) if collapse == keyword::COLLAPSE && wrap == keyword::WRAP => Some("normal"),
                (collapse, wrap) if collapse == keyword::PRESERVE && wrap == keyword::NOWRAP => Some("pre"),
                (collapse, wrap) if collapse == keyword::PRESERVE && wrap == keyword::WRAP => Some("pre-wrap"),
                (collapse, wrap) if collapse == keyword::PRESERVE_BREAKS && wrap == keyword::WRAP => Some("pre-line"),
                _ => None,
            };
            if let Some(short) = short {
                sink.push_ascii(short);
                return true;
            }
        }
        return default_serialize(sink);
    }
    // The remaining special-cased shorthands (animation, background and its position, font and
    // font-variant, the grid family, mask, the timeline pair and transition) still serialize in
    // C++: their rules need the parser or per-layer coordination that has not been ported.
    if [
        property_id::ANIMATION,
        property_id::BACKGROUND,
        property_id::BORDER,
        property_id::BACKGROUND_POSITION,
        property_id::FONT,
        property_id::FONT_VARIANT,
        property_id::GRID,
        property_id::GRID_AREA,
        property_id::GRID_TEMPLATE,
        property_id::MASK,
        property_id::SCROLL_TIMELINE,
        property_id::TRANSITION,
        property_id::VIEW_TIMELINE,
    ]
    .contains(&shorthand_property)
    {
        return false;
    }
    if POSITIONAL_VALUE_LIST_SHORTHANDS.contains(&shorthand_property) {
        let entries: Vec<&StyleValueData> = match values.as_slice().iter().map(|value| value.optional_data()).collect()
        {
            Some(entries) => entries,
            None => return false,
        };
        return serialize_a_positional_value_list(sink, &entries, mode);
    }
    default_serialize(sink)
}

/// A native `AK::Utf16String` ownership reference handed to C++ without copying.
#[repr(C)]
pub struct FfiSerializedText {
    pub raw: usize,
    pub has_value: bool,
}

impl FfiSerializedText {
    fn unported() -> Self {
        Self {
            raw: 0,
            has_value: false,
        }
    }
}

pub(crate) fn sink_into_ffi(sink: TextSink) -> FfiSerializedText {
    let string = if sink.is_ascii {
        // SAFETY: The ASCII representation is valid UTF-8.
        let text = unsafe { str::from_utf8_unchecked(&sink.ascii) };
        ak::Utf16String::from_utf8(text)
    } else {
        ak::Utf16String::from_utf16(&sink.utf16)
    };
    FfiSerializedText {
        raw: string.into_raw(),
        has_value: true,
    }
}

/// Serializes a style value, or returns the null serialization when the value's type has not
/// been ported yet so the C++ dispatcher falls back to the legacy serializer.
///
/// # Safety
/// `value` must point at live style value data.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_serialize(value: *const c_void, mode: u8) -> FfiSerializedText {
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::StyleValueSerializeEntry);
    crate::abort_on_panic(|| {
        let value = unsafe { &*value.cast::<StyleValueData>() };
        let mut sink = TextSink::new();
        if !serialize_style_value(&mut sink, value, SerializationMode::from_ffi(mode)) {
            return FfiSerializedText::unported();
        }
        sink_into_ffi(sink)
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn text(sink: TextSink) -> String {
        if sink.is_ascii {
            String::from_utf8(sink.ascii).unwrap()
        } else {
            String::from_utf16(&sink.utf16).unwrap()
        }
    }

    fn number_to_string(value: f64) -> String {
        let mut sink = TextSink::new();
        serialize_a_number(&mut sink, value);
        text(sink)
    }

    #[test]
    fn number_serialization_matches_ak_format() {
        assert_eq!(number_to_string(0.0), "0");
        assert_eq!(number_to_string(1.0), "1");
        assert_eq!(number_to_string(-1.0), "-1");
        assert_eq!(number_to_string(0.5), "0.5");
        assert_eq!(number_to_string(1.5), "1.5");
        assert_eq!(number_to_string(-0.25), "-0.25");
        assert_eq!(number_to_string(0.0000004), "0");
        assert_eq!(number_to_string(100.0), "100");
        assert_eq!(number_to_string(0.1), "0.1");
        assert_eq!(number_to_string(16.0 / 9.0), "1.777778");
        assert_eq!(number_to_string(1.0 / 3.0), "0.333333");
    }

    #[test]
    fn identifier_escaping() {
        let mut sink = TextSink::new();
        serialize_an_identifier(&mut sink, &StringUnits::Ascii(b"7abc"));
        assert_eq!(text(sink), "\\37 abc");

        let mut sink = TextSink::new();
        serialize_an_identifier(&mut sink, &StringUnits::Ascii(b"-"));
        assert_eq!(text(sink), "\\-");

        let mut sink = TextSink::new();
        serialize_an_identifier(&mut sink, &StringUnits::Ascii(b"hello world"));
        assert_eq!(text(sink), "hello\\ world");
    }

    #[test]
    fn string_escaping() {
        let mut sink = TextSink::new();
        serialize_a_string(&mut sink, &StringUnits::Ascii(b"a\"b\\c"));
        assert_eq!(text(sink), "\"a\\\"b\\\\c\"");
    }

    #[test]
    fn utf16_promotion() {
        let mut sink = TextSink::new();
        sink.push_ascii("abc");
        sink.push_code_point(0x1F600);
        assert!(!sink.is_ascii);
        assert_eq!(text(sink), "abc\u{1F600}");
    }
}
