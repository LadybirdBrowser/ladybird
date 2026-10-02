/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! AK's JsonValue, JsonParser and JSON serialization, as far as the test runner uses them: the results the harness
//! hands over as JSON text, and the results the runner prints as JSON. Strings are kept as the WTF-8 bytes AK's String
//! holds, since a JSON escape of a lone surrogate decodes to one.

use indexmap::IndexMap;

use crate::runtime::value::number_to_string;

pub enum JsonValue {
    Null,
    Bool(bool),
    I64(i64),
    U64(u64),
    Double(f64),
    String(Vec<u8>),
    Array(Vec<JsonValue>),
    Object(JsonObject),
}

impl From<bool> for JsonValue {
    fn from(value: bool) -> Self {
        Self::Bool(value)
    }
}

impl From<u32> for JsonValue {
    fn from(value: u32) -> Self {
        Self::U64(u64::from(value))
    }
}

impl From<f64> for JsonValue {
    fn from(value: f64) -> Self {
        Self::Double(value)
    }
}

impl From<&str> for JsonValue {
    fn from(value: &str) -> Self {
        Self::String(value.as_bytes().to_vec())
    }
}

impl From<JsonObject> for JsonValue {
    fn from(value: JsonObject) -> Self {
        Self::Object(value)
    }
}

impl JsonValue {
    pub fn as_object(&self) -> Option<&JsonObject> {
        match self {
            Self::Object(object) => Some(object),
            _ => None,
        }
    }

    /// JsonValue::get_integer<u64>(): the value of a number that is within the range of u64, truncated.
    fn get_u64(&self) -> Option<u64> {
        match *self {
            Self::I64(value) => u64::try_from(value).ok(),
            Self::U64(value) => Some(value),
            // NB: AK's is_within_range() compares the double against the limits converted to double.
            Self::Double(value) if value >= 0.0 && value <= u64::MAX as f64 => Some(value as u64),
            _ => None,
        }
    }

    pub fn serialized(&self) -> Vec<u8> {
        let mut output = Vec::new();
        self.serialize_into(&mut output);
        output
    }

    fn serialize_into(&self, output: &mut Vec<u8>) {
        match self {
            Self::Null => output.extend_from_slice(b"null"),
            Self::Bool(value) => output.extend_from_slice(if *value { b"true" } else { b"false" }),
            Self::I64(value) => output.extend_from_slice(value.to_string().as_bytes()),
            Self::U64(value) => output.extend_from_slice(value.to_string().as_bytes()),
            // JSON cannot represent infinities or NaN, so they serialize as null, as in JSON.stringify().
            Self::Double(value) if !value.is_finite() => output.extend_from_slice(b"null"),
            Self::Double(value) => output.extend_from_slice(format_double(*value).as_bytes()),
            Self::String(value) => {
                output.push(b'"');
                append_escaped_for_json(output, value);
                output.push(b'"');
            }
            Self::Array(values) => {
                output.push(b'[');
                for (index, value) in values.iter().enumerate() {
                    if index != 0 {
                        output.push(b',');
                    }
                    value.serialize_into(output);
                }
                output.push(b']');
            }
            Self::Object(object) => object.serialize_into(output),
        }
    }
}

/// StringBuilder::append_escaped_for_json(), byte by byte.
fn append_escaped_for_json(output: &mut Vec<u8>, bytes: &[u8]) {
    for &byte in bytes {
        match byte {
            b'\x08' => output.extend_from_slice(b"\\b"),
            b'\n' => output.extend_from_slice(b"\\n"),
            b'\t' => output.extend_from_slice(b"\\t"),
            b'"' => output.extend_from_slice(b"\\\""),
            b'\\' => output.extend_from_slice(b"\\\\"),
            0..=0x1f => output.extend_from_slice(format!("\\u{byte:04x}").as_bytes()),
            _ => output.push(byte),
        }
    }
}

/// A double as AK formats it with "{}": the shortest digits that round-trip, which is what Number::toString picks too.
pub fn format_double(value: f64) -> String {
    if value.is_nan() {
        return "nan".to_string();
    }
    if value.is_infinite() {
        return if value < 0.0 { "-inf" } else { "inf" }.to_string();
    }
    if value == 0.0 && value.is_sign_negative() {
        return "-0".to_string();
    }
    number_to_string(value)
}

/// A double as AK formats it with a precision and no width, FormatBuilder::put_f64_with_precision() in its default
/// display mode, for the values that are not shown in scientific notation.
pub fn format_double_with_precision(value: f64, precision: usize) -> String {
    let mut value = value;
    let is_negative = value < 0.0;
    if is_negative {
        value = -value;
    }

    let mut integer_value = value as u64;
    value -= (value as i64) as f64;

    let mut fraction_digits = Vec::new();

    if precision > 0 {
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
                value -= ((value as u64) - (value as u64) % 10) as f64;
            }

            fraction_digits.push(b'0' + ((value as u32) % 10) as u8);
        }
    }

    // Round up if the following decimal is 5 or higher
    if ((value * 10.0) as u64) % 10 >= 5 && round_up_digits(&mut fraction_digits) {
        integer_value += 1;
    }

    while fraction_digits.last() == Some(&b'0') {
        fraction_digits.pop();
    }

    let mut output = String::new();
    if is_negative {
        output.push('-');
    }
    output.push_str(&integer_value.to_string());
    if !fraction_digits.is_empty() {
        output.push('.');
        output.extend(fraction_digits.iter().copied().map(char::from));
    }
    output
}

fn round_up_digits(digits: &mut [u8]) -> bool {
    for digit in digits.iter_mut().rev() {
        if *digit != b'9' {
            *digit += 1;
            return false;
        }
        *digit = b'0';
    }
    true
}

/// A JSON object that keeps its members in insertion order, like AK's JsonObject.
#[derive(Default)]
pub struct JsonObject {
    members: IndexMap<Vec<u8>, JsonValue>,
}

impl JsonObject {
    /// Replaces the value of an existing member in place, or appends a new one.
    pub fn set(&mut self, key: impl Into<Vec<u8>>, value: impl Into<JsonValue>) {
        self.members.insert(key.into(), value.into());
    }

    pub fn has(&self, key: &str) -> bool {
        self.members.contains_key(key.as_bytes())
    }

    pub fn get_string(&self, key: &str) -> Option<&[u8]> {
        match self.members.get(key.as_bytes()) {
            Some(JsonValue::String(value)) => Some(value),
            _ => None,
        }
    }

    pub fn get_u64(&self, key: &str) -> Option<u64> {
        self.members.get(key.as_bytes()).and_then(JsonValue::get_u64)
    }

    pub fn members(&self) -> impl Iterator<Item = (&[u8], &JsonValue)> {
        self.members.iter().map(|(key, value)| (key.as_slice(), value))
    }

    fn serialize_into(&self, output: &mut Vec<u8>) {
        output.push(b'{');
        for (index, (key, value)) in self.members.iter().enumerate() {
            if index != 0 {
                output.push(b',');
            }
            output.push(b'"');
            append_escaped_for_json(output, key);
            output.push(b'"');
            output.push(b':');
            value.serialize_into(output);
        }
        output.push(b'}');
    }
}

/// AK's JsonParser, which fails with the message it would fail with.
pub struct JsonParser<'input> {
    input: &'input [u8],
    index: usize,
    current_nesting_depth: usize,
}

const MAX_NESTING_DEPTH: usize = 512;

fn is_space(byte: u8) -> bool {
    matches!(byte, b'\t' | b'\n' | b'\r' | b' ')
}

impl<'input> JsonParser<'input> {
    pub fn parse(input: &'input [u8]) -> Result<JsonValue, &'static str> {
        let mut parser = Self {
            input,
            index: 0,
            current_nesting_depth: 0,
        };
        parser.parse_json()
    }

    /// GenericLexer::peek(), which is 0 past the end of the input.
    fn peek(&self, offset: usize) -> u8 {
        self.input.get(self.index + offset).copied().unwrap_or(0)
    }

    fn is_eof(&self) -> bool {
        self.index >= self.input.len()
    }

    fn consume_specific(&mut self, expected: &[u8]) -> bool {
        if self.input[self.index.min(self.input.len())..].starts_with(expected) {
            self.index += expected.len();
            return true;
        }
        false
    }

    fn ignore_spaces(&mut self) {
        while !self.is_eof() && is_space(self.peek(0)) {
            self.index += 1;
        }
    }

    fn consume_and_unescape_string(&mut self) -> Result<Vec<u8>, &'static str> {
        if !self.consume_specific(b"\"") {
            return Err("JsonParser: Expected '\"'");
        }
        let mut final_sb = Vec::new();

        loop {
            let mut literal_characters = 0;
            loop {
                let ch = self.peek(literal_characters);
                // Note: We get a 0 byte when we hit EOF
                if ch == 0 {
                    return Err("JsonParser: EOF while parsing String");
                }
                if ch < 0x20 {
                    return Err("JsonParser: ASCII control sequence encountered");
                }
                if ch == b'"' || ch == b'\\' {
                    break;
                }
                literal_characters += 1;
            }
            final_sb.extend_from_slice(&self.input[self.index..self.index + literal_characters]);
            self.index += literal_characters;

            if self.peek(0) == b'"' {
                self.index += 1;
                break;
            }

            // '\'
            self.index += 1;

            let escaped = self.peek(0);
            match escaped {
                0 => return Err("JsonParser: EOF while parsing String"),
                b'"' | b'\\' | b'/' => final_sb.push(escaped),
                b'b' => final_sb.push(b'\x08'),
                b'f' => final_sb.push(b'\x0c'),
                b'n' => final_sb.push(b'\n'),
                b'r' => final_sb.push(b'\r'),
                b't' => final_sb.push(b'\t'),
                b'u' => {
                    self.index += 1;
                    let Some(code_point) = self.decode_single_or_paired_surrogate() else {
                        return Err("JsonParser: Error while parsing Unicode escape");
                    };
                    append_code_point_as_wtf8(&mut final_sb, code_point);
                    continue;
                }
                _ => return Err("JsonParser: Invalid escaped character"),
            }
            self.index += 1;
        }

        Ok(final_sb)
    }

    fn decode_one_surrogate(&mut self) -> Option<u16> {
        let mut surrogate: u16 = 0;
        for _ in 0..4 {
            let digit = char::from(self.peek(0)).to_digit(16)?;
            self.index += 1;
            surrogate = (surrogate << 4) | digit as u16;
        }
        Some(surrogate)
    }

    /// GenericLexer::decode_single_or_paired_surrogate(), which leaves a high surrogate that is not followed by the
    /// escape of a low one on its own.
    fn decode_single_or_paired_surrogate(&mut self) -> Option<u32> {
        let high_surrogate = self.decode_one_surrogate()?;
        if !(0xD800..=0xDBFF).contains(&high_surrogate) {
            return Some(u32::from(high_surrogate));
        }
        if !self.consume_specific(b"\\u") {
            return Some(u32::from(high_surrogate));
        }
        if self.peek(0) == b'{' {
            self.index -= 2;
            return Some(u32::from(high_surrogate));
        }
        let low_surrogate = self.decode_one_surrogate()?;
        if (0xDC00..=0xDFFF).contains(&low_surrogate) {
            return Some(((u32::from(high_surrogate) - 0xD800) << 10) + (u32::from(low_surrogate) - 0xDC00) + 0x10000);
        }
        self.index -= 6;
        Some(u32::from(high_surrogate))
    }

    fn parse_object(&mut self) -> Result<JsonValue, &'static str> {
        if self.current_nesting_depth >= MAX_NESTING_DEPTH {
            return Err("JsonParser: Exceeded maximum nesting depth");
        }
        self.current_nesting_depth += 1;
        let result = self.parse_object_members();
        self.current_nesting_depth -= 1;
        result
    }

    fn parse_object_members(&mut self) -> Result<JsonValue, &'static str> {
        let mut object = JsonObject::default();
        if !self.consume_specific(b"{") {
            return Err("JsonParser: Expected '{'");
        }
        loop {
            self.ignore_spaces();
            if self.peek(0) == b'}' {
                break;
            }
            self.ignore_spaces();
            let name = self.consume_and_unescape_string()?;
            self.ignore_spaces();
            if !self.consume_specific(b":") {
                return Err("JsonParser: Expected ':'");
            }
            self.ignore_spaces();
            let value = self.parse_helper()?;
            object.set(name, value);
            self.ignore_spaces();
            if self.peek(0) == b'}' {
                break;
            }
            if !self.consume_specific(b",") {
                return Err("JsonParser: Expected ','");
            }
            self.ignore_spaces();
            if self.peek(0) == b'}' {
                return Err("JsonParser: Unexpected '}'");
            }
        }
        if !self.consume_specific(b"}") {
            return Err("JsonParser: Expected '}'");
        }
        Ok(JsonValue::Object(object))
    }

    fn parse_array(&mut self) -> Result<JsonValue, &'static str> {
        if self.current_nesting_depth >= MAX_NESTING_DEPTH {
            return Err("JsonParser: Exceeded maximum nesting depth");
        }
        self.current_nesting_depth += 1;
        let result = self.parse_array_elements();
        self.current_nesting_depth -= 1;
        result
    }

    fn parse_array_elements(&mut self) -> Result<JsonValue, &'static str> {
        let mut array = Vec::new();
        if !self.consume_specific(b"[") {
            return Err("JsonParser: Expected '['");
        }
        loop {
            self.ignore_spaces();
            if self.peek(0) == b']' {
                break;
            }
            array.push(self.parse_helper()?);
            self.ignore_spaces();
            if self.peek(0) == b']' {
                break;
            }
            if !self.consume_specific(b",") {
                return Err("JsonParser: Expected ','");
            }
            self.ignore_spaces();
            if self.peek(0) == b']' {
                return Err("JsonParser: Unexpected ']'");
            }
        }
        self.ignore_spaces();
        if !self.consume_specific(b"]") {
            return Err("JsonParser: Expected ']'");
        }
        Ok(JsonValue::Array(array))
    }

    /// parse_first_number<double>(): the longest prefix from `start_index` that reads as a double.
    fn fallback_to_double_parse(&mut self, start_index: usize) -> Result<JsonValue, &'static str> {
        let mut end = start_index;
        let digits_from = |input: &[u8], mut end: usize| {
            while input.get(end).is_some_and(u8::is_ascii_digit) {
                end += 1;
            }
            end
        };
        if self.input.get(end) == Some(&b'-') {
            end += 1;
        }
        end = digits_from(self.input, end);
        if self.input.get(end) == Some(&b'.') && self.input.get(end + 1).is_some_and(u8::is_ascii_digit) {
            end = digits_from(self.input, end + 1);
        }
        if matches!(self.input.get(end), Some(b'e' | b'E')) {
            let mut exponent_end = end + 1;
            if matches!(self.input.get(exponent_end), Some(b'+' | b'-')) {
                exponent_end += 1;
            }
            if self.input.get(exponent_end).is_some_and(u8::is_ascii_digit) {
                end = digits_from(self.input, exponent_end);
            }
        }
        let text =
            core::str::from_utf8(&self.input[start_index..end]).map_err(|_| "JsonParser: Invalid floating point")?;
        let value: f64 = text.parse().map_err(|_| "JsonParser: Invalid floating point")?;
        self.index = end;
        Ok(JsonValue::Double(value))
    }

    fn parse_number(&mut self) -> Result<JsonValue, &'static str> {
        let mut number_buffer = String::new();

        let start_index = self.index;

        let mut negative = false;
        if self.peek(0) == b'-' {
            number_buffer.push('-');
            self.index += 1;
            negative = true;

            if !self.peek(0).is_ascii_digit() {
                return Err("JsonParser: Unexpected '-' without further digits");
            }
        }

        if self.peek(0) == b'0' && self.peek(1).is_ascii_digit() {
            return Err("JsonParser: Cannot have leading zeros");
        }

        let mut all_zero = true;
        loop {
            let ch = self.peek(0);
            if ch == b'.' {
                if !self.peek(1).is_ascii_digit() {
                    return Err("JsonParser: Must have digits after decimal point");
                }
                return self.fallback_to_double_parse(start_index);
            }
            if ch == b'e' || ch == b'E' {
                let next = self.peek(1);
                if !next.is_ascii_digit() && ((next != b'+' && next != b'-') || !self.peek(2).is_ascii_digit()) {
                    return Err("JsonParser: Must have digits after exponent with an optional sign inbetween");
                }
                return self.fallback_to_double_parse(start_index);
            }
            if ch.is_ascii_digit() {
                if ch != b'0' {
                    all_zero = false;
                }
                number_buffer.push(char::from(ch));
                self.index += 1;
                continue;
            }
            break;
        }

        // Negative zero is always a double
        if negative && all_zero {
            return Ok(JsonValue::Double(-0.0));
        }

        if let Ok(number) = number_buffer.parse::<u64>() {
            return Ok(JsonValue::U64(number));
        }
        if let Ok(number) = number_buffer.parse::<i64>() {
            return Ok(JsonValue::I64(number));
        }

        // It's possible the unsigned value is bigger than u64 max
        self.fallback_to_double_parse(start_index)
    }

    fn parse_helper(&mut self) -> Result<JsonValue, &'static str> {
        self.ignore_spaces();
        match self.peek(0) {
            b'{' => self.parse_object(),
            b'[' => self.parse_array(),
            b'"' => self.consume_and_unescape_string().map(JsonValue::String),
            b'-' | b'0'..=b'9' => self.parse_number(),
            b'f' if self.consume_specific(b"false") => Ok(JsonValue::Bool(false)),
            b'f' => Err("JsonParser: Expected 'false'"),
            b't' if self.consume_specific(b"true") => Ok(JsonValue::Bool(true)),
            b't' => Err("JsonParser: Expected 'true'"),
            b'n' if self.consume_specific(b"null") => Ok(JsonValue::Null),
            b'n' => Err("JsonParser: Expected 'null'"),
            _ => Err("JsonParser: Unexpected character"),
        }
    }

    fn parse_json(&mut self) -> Result<JsonValue, &'static str> {
        let result = self.parse_helper()?;
        self.ignore_spaces();
        if !self.is_eof() {
            return Err("JsonParser: Didn't consume all input");
        }
        Ok(result)
    }
}

/// StringBuilder::append_code_point(), which encodes a lone surrogate as the three bytes WTF-8 has for it.
fn append_code_point_as_wtf8(output: &mut Vec<u8>, code_point: u32) {
    if code_point <= 0x7f {
        output.push(code_point as u8);
    } else if code_point <= 0x07ff {
        output.push((((code_point >> 6) & 0x1f) | 0xc0) as u8);
        output.push(((code_point & 0x3f) | 0x80) as u8);
    } else if code_point <= 0xffff {
        output.push((((code_point >> 12) & 0x0f) | 0xe0) as u8);
        output.push((((code_point >> 6) & 0x3f) | 0x80) as u8);
        output.push(((code_point & 0x3f) | 0x80) as u8);
    } else {
        output.push((((code_point >> 18) & 0x07) | 0xf0) as u8);
        output.push((((code_point >> 12) & 0x3f) | 0x80) as u8);
        output.push((((code_point >> 6) & 0x3f) | 0x80) as u8);
        output.push(((code_point & 0x3f) | 0x80) as u8);
    }
}
