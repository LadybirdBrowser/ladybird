/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ops::ControlFlow;
use std::collections::{HashMap, HashSet};
use std::rc::Rc;

use ak::{Utf16FlyString, Utf16String};
use libjs_runtime_macros::Trace;

use crate::bytecode::executable::StaticPropertyLookupCacheSite;
use crate::gc::class::{GcCell, define_cell};
use crate::gc::root::MarkedVec;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{call, call_function_object, length_of_array_like};
use crate::runtime::array::Array;
use crate::runtime::big_int_object::BigIntObject;
use crate::runtime::boolean_object::BooleanObject;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::native_function::raw_native;
use crate::runtime::number_object::NumberObject;
use crate::runtime::object::{
    IntegrityLevel, MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, PropertyKind, define_object_class,
};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, DEFAULT_ATTRIBUTES, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::raw_json_object::RawJSONObject;
use crate::runtime::realm::Realm;
use crate::runtime::shape::Shape;
use crate::runtime::string_conversions::parse_first_number_f64;
use crate::runtime::string_object::StringObject;
use crate::runtime::value::{append_number_to_string, same_value};
use crate::simdjson;
use crate::utf16::{Utf16StringBuilder, Utf16View};

/// A JSON Parse Record is a Record value used to describe the initial state of a value parsed from JSON text.
/// https://tc39.es/ecma262/#sec-json-parse-record
struct JSONParseRecord {
    // [[Key]]: the property name with which [[Value]] is associated.
    key: Utf16String,
    // NB: In place of the spec's [[ParseNode]] field, we capture the source text
    //     matched by the parse node up front. It is present only when [[Value]] is
    //     a primitive, since the reviver context only exposes "source" for those.
    source: Option<Utf16String>,
    // [[Elements]]: if [[Value]] is an Array, the records for its elements; else empty.
    elements: Vec<usize>,
    // [[Entries]]: if [[Value]] is a non-Array Object, the records for its entries; else empty.
    entries: Vec<usize>,
}

/// The JSON Parse Records of one text, by index, the root record first. Their [[Value]]s are kept in a marked list,
/// which keeps them alive while the reviver may detach them from the object graph.
pub struct JSONParseRecords<'vm> {
    // [[Value]]: the value produced by evaluation of [[ParseNode]].
    values: MarkedVec<'vm, Value>,
    records: Vec<JSONParseRecord>,
}

impl<'vm> JSONParseRecords<'vm> {
    pub const ROOT: usize = 0;

    pub fn new(vm: &'vm Vm) -> Self {
        let mut records = Self {
            values: MarkedVec::new(vm),
            records: Vec::new(),
        };
        records.create();
        records
    }

    fn create(&mut self) -> usize {
        self.values.push(Value::UNDEFINED);
        self.records.push(JSONParseRecord {
            key: Utf16String::default(),
            source: None,
            elements: Vec::new(),
            entries: Vec::new(),
        });
        self.records.len() - 1
    }

    fn value(&self, record: usize) -> Value {
        self.values.get(record).expect("the record exists")
    }

    fn set_value(&mut self, record: usize, value: Value) {
        self.values.set(record, value);
    }
}

#[repr(C)]
#[derive(Trace)]
pub struct JSONObject {
    base: Object,
}

define_object_class!(JSONObject, extends: [Object], methods: {
    initialize: JSONObject::initialize,
    ..ORDINARY_OBJECT_METHODS
});

const STRINGIFY_OBJECT_STACK_LINEAR_LOOKUP_LIMIT: usize = 32;

fn address_of<T>(cell: Gc<T>) -> usize {
    cell.as_ptr().addr()
}

struct StringifyObjectStack<'vm> {
    stack: MarkedVec<'vm, Gc<Object>>,
    lookup: HashSet<usize>,
}

impl<'vm> StringifyObjectStack<'vm> {
    fn new(vm: &'vm Vm) -> Self {
        Self {
            stack: MarkedVec::new(vm),
            lookup: HashSet::new(),
        }
    }

    fn contains(&self, object: Gc<Object>) -> bool {
        if self.lookup.is_empty() {
            return (0..self.stack.len()).any(|index| self.stack.get(index) == Some(object));
        }
        self.lookup.contains(&address_of(object))
    }

    fn push(&mut self, object: Gc<Object>) {
        if self.stack.len() == STRINGIFY_OBJECT_STACK_LINEAR_LOOKUP_LIMIT {
            for index in 0..self.stack.len() {
                let seen_object = self.stack.get(index).expect("the index is in bounds");
                self.lookup.insert(address_of(seen_object));
            }
        }
        if !self.lookup.is_empty() {
            self.lookup.insert(address_of(object));
        }
        self.stack.push(object);
    }

    fn pop(&mut self) {
        let object = self.stack.pop().expect("an object is being serialized");
        if self.lookup.is_empty() {
            return;
        }
        self.lookup.remove(&address_of(object));
        if self.stack.len() <= STRINGIFY_OBJECT_STACK_LINEAR_LOOKUP_LIMIT {
            self.lookup.clear();
        }
    }
}

struct StringifyCachedProperty {
    property: PropertyKey,
    offset: u32,
    serialized_key: Utf16String,
}

struct StringifyShapeCacheEntry {
    shape: usize,
    properties: Vec<StringifyCachedProperty>,
}

const STRINGIFY_SHAPE_CACHE_LINEAR_LOOKUP_LIMIT: usize = 4;
const STRINGIFY_SHAPE_CACHE_MAXIMUM_ENTRIES: usize = 64;

struct StringifyShapeCache<'vm> {
    shape_roots: MarkedVec<'vm, Gc<Shape>>,
    entries: Vec<Rc<StringifyShapeCacheEntry>>,
    lookup: HashMap<usize, usize>,
}

impl<'vm> StringifyShapeCache<'vm> {
    fn new(vm: &'vm Vm) -> Self {
        Self {
            shape_roots: MarkedVec::new(vm),
            entries: Vec::new(),
            lookup: HashMap::new(),
        }
    }

    fn get(&self, shape: Gc<Shape>) -> Option<Rc<StringifyShapeCacheEntry>> {
        if !self.lookup.is_empty() {
            return self
                .lookup
                .get(&address_of(shape))
                .map(|index| self.entries[*index].clone());
        }
        self.entries
            .iter()
            .find(|entry| entry.shape == address_of(shape))
            .cloned()
    }

    /// Adds the entry of `shape`, whose properties `properties_of_shape` computes, unless the cache is full.
    fn add(
        &mut self,
        shape: Gc<Shape>,
        properties_of_shape: impl FnOnce() -> Vec<StringifyCachedProperty>,
    ) -> Option<Rc<StringifyShapeCacheEntry>> {
        if self.entries.len() >= STRINGIFY_SHAPE_CACHE_MAXIMUM_ENTRIES {
            return None;
        }

        let entry = Rc::new(StringifyShapeCacheEntry {
            shape: address_of(shape),
            properties: properties_of_shape(),
        });

        if self.entries.len() == STRINGIFY_SHAPE_CACHE_LINEAR_LOOKUP_LIMIT {
            for (index, existing_entry) in self.entries.iter().enumerate() {
                self.lookup.insert(existing_entry.shape, index);
            }
        }
        if !self.lookup.is_empty() {
            self.lookup.insert(address_of(shape), self.entries.len());
        }

        self.shape_roots.push(shape);
        self.entries.push(entry.clone());
        Some(entry)
    }
}

struct StringifyState<'vm> {
    replacer_function: Option<Gc<FunctionObject>>,
    object_stack: StringifyObjectStack<'vm>,
    shape_cache: StringifyShapeCache<'vm>,
    indent_depth: usize,
    gap: Utf16String,
    property_list: Option<Rc<Vec<Utf16String>>>,
    builder: Utf16StringBuilder,
}

fn can_use_direct_property_access(object: &Object) -> bool {
    // OPTIMIZATION: Ordinary non-dictionary objects can snapshot their shape and read data
    //               properties directly by offset while that shape remains unchanged.
    object.eligible_for_own_property_enumeration_fast_path()
        && !object.has_intrinsic_accessors()
        && !object.has_parameter_map()
        && !object.is_platform_object()
        && !object.shape().is_dictionary()
        && object.indexed_real_size() == 0
}

fn append_json_number(builder: &mut Utf16StringBuilder, value: Value) {
    if !value.is_int32() {
        append_number_to_string(builder, value.as_f64());
        return;
    }

    builder.append_ascii(&value.as_i32().to_string());
}

fn write_indent(builder: &mut Utf16StringBuilder, gap: &Utf16String, depth: usize) {
    builder.append_repeated(Utf16View::of_string(gap), depth);
}

fn json_malformed<T>(vm: &Vm) -> ThrowCompletionOr<T> {
    vm.throw_completion(ErrorKind::SyntaxError, ErrorType::JsonMalformed, &[])
}

/// Unescape a JSON string, properly handling \uXXXX escape sequences including lone surrogates.
/// simdjson validates UTF-8 strictly and rejects lone surrogates, but JSON allows them.
/// Returns None on malformed escape sequences.
fn unescape_json_string(raw: &[u8]) -> Option<Utf16String> {
    let mut builder: Vec<u16> = Vec::with_capacity(raw.len());

    let mut position = 0;
    let remaining = |position: usize| raw.len() - position;

    let consume_hex4 = |position: &mut usize| -> Option<u16> {
        if remaining(*position) < 4 {
            return None;
        }
        let mut value: u16 = 0;
        for _ in 0..4 {
            let character = raw[*position];
            *position += 1;
            value <<= 4;
            value |= u16::from(char::from(character).to_digit(16)? as u8);
        }
        Some(value)
    };

    while position < raw.len() {
        if raw[position] == b'\\' {
            position += 1;
            if position == raw.len() {
                return None;
            }
            let escaped = raw[position];
            position += 1;
            match escaped {
                b'"' => builder.push(u16::from(b'"')),
                b'\\' => builder.push(u16::from(b'\\')),
                b'/' => builder.push(u16::from(b'/')),
                b'b' => builder.push(0x08),
                b'f' => builder.push(0x0C),
                b'n' => builder.push(u16::from(b'\n')),
                b'r' => builder.push(u16::from(b'\r')),
                b't' => builder.push(u16::from(b'\t')),
                b'u' => builder.push(consume_hex4(&mut position)?),
                _ => return None,
            }
        } else {
            // Non-escaped character - copy UTF-8 code point to UTF-16
            let character = u32::from(raw[position]);
            position += 1;
            if character & 0x80 == 0 {
                // ASCII
                builder.push(character as u16);
            } else if character & 0xE0 == 0xC0 {
                // 2-byte UTF-8
                if position == raw.len() {
                    return None;
                }
                let character2 = u32::from(raw[position]);
                position += 1;
                let code_point = ((character & 0x1F) << 6) | (character2 & 0x3F);
                builder.push(code_point as u16);
            } else if character & 0xF0 == 0xE0 {
                // 3-byte UTF-8
                if remaining(position) < 2 {
                    return None;
                }
                let character2 = u32::from(raw[position]);
                let character3 = u32::from(raw[position + 1]);
                position += 2;
                let code_point = ((character & 0x0F) << 12) | ((character2 & 0x3F) << 6) | (character3 & 0x3F);
                builder.push(code_point as u16);
            } else if character & 0xF8 == 0xF0 {
                // 4-byte UTF-8 (needs surrogate pair)
                if remaining(position) < 3 {
                    return None;
                }
                let character2 = u32::from(raw[position]);
                let character3 = u32::from(raw[position + 1]);
                let character4 = u32::from(raw[position + 2]);
                position += 3;
                let code_point = ((character & 0x07) << 18)
                    | ((character2 & 0x3F) << 12)
                    | ((character3 & 0x3F) << 6)
                    | (character4 & 0x3F);
                let code_point = char::from_u32(code_point)?;
                let mut buffer = [0u16; 2];
                builder.extend_from_slice(code_point.encode_utf16(&mut buffer));
            } else {
                return None;
            }
        }
    }

    Some(Utf16String::from_utf16(&builder))
}

struct JSONTextBytes<'a> {
    text: Utf16View<'a>,
    /// The text as UTF-8 with each lone surrogate written as a \uXXXX escape, then SIMDJSON_PADDING zero bytes.
    utf8: Vec<u8>,
    length: usize,
    byte_to_code_unit_offsets: Vec<usize>,
}

impl JSONTextBytes<'_> {
    fn bytes(&self) -> &[u8] {
        &self.utf8[..self.length]
    }

    /// The text of a string or key without escapes, which `bytes` holds between two quotes, as C++ makes it with
    /// from_utf8_without_validation().
    fn unescaped_text<'a>(&self, bytes: &'a [u8]) -> &'a str {
        assert!(self.bytes().as_ptr_range().contains(&bytes.as_ptr()) || bytes.is_empty());
        debug_assert!(core::str::from_utf8(bytes).is_ok());
        // SAFETY: The text is UTF-8 by construction, and `bytes` runs between two of its ASCII quotes.
        unsafe { core::str::from_utf8_unchecked(bytes) }
    }

    fn byte_offset_to_code_unit_offset(&self, byte_offset: usize) -> usize {
        if self.text.has_ascii_storage() {
            return byte_offset;
        }
        assert!(byte_offset < self.byte_to_code_unit_offsets.len());
        self.byte_to_code_unit_offsets[byte_offset]
    }

    /// The offset of a token simdjson returned, which is a part of these bytes.
    fn offset_of(&self, token: &[u8]) -> usize {
        let offset = token.as_ptr().addr() - self.utf8.as_ptr().addr();
        assert!(offset + token.len() <= self.utf8.len());
        offset
    }
}

/// An entry of the cache of the property keys of a text, by their raw bytes.
struct JSONPropertyKeyCacheEntry {
    raw_key: Vec<u8>,
    key: Option<PropertyKey>,
}

struct JSONPropertyKeyCache {
    entries: Vec<JSONPropertyKeyCacheEntry>,
}

/// AK's pair_int_hash().
fn pair_int_hash(key1: u32, key2: u32) -> u32 {
    let mut key = (u64::from(key1) << 32) | u64::from(key2);
    key ^= key >> 33;
    key = key.wrapping_mul(0xff51_afd7_ed55_8ccd);
    key ^= key >> 33;
    key = key.wrapping_mul(0xc4ce_b9fe_1a85_ec53);
    key ^= key >> 33;
    key as u32
}

impl JSONPropertyKeyCache {
    const ENTRY_COUNT: usize = 128;

    fn new() -> Self {
        Self {
            entries: (0..Self::ENTRY_COUNT)
                .map(|_| JSONPropertyKeyCacheEntry {
                    raw_key: Vec::new(),
                    key: None,
                })
                .collect(),
        }
    }

    fn entry_for(&mut self, raw_key: &[u8]) -> &mut JSONPropertyKeyCacheEntry {
        let mut ends = 0;
        if let (Some(first), Some(last)) = (raw_key.first(), raw_key.last()) {
            ends = (u32::from(*first) << 8) | u32::from(*last);
        }
        &mut self.entries[pair_int_hash(raw_key.len() as u32, ends) as usize % Self::ENTRY_COUNT]
    }
}

struct JSONParseState<'text, 'records, 'vm> {
    text: &'text JSONTextBytes<'text>,
    property_keys: JSONPropertyKeyCache,
    records: Option<&'records mut JSONParseRecords<'vm>>,
}

impl<'vm> JSONParseState<'_, '_, 'vm> {
    fn records(&mut self) -> &mut JSONParseRecords<'vm> {
        self.records
            .as_deref_mut()
            .expect("there are records when a value has one")
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum TrackCodeUnitOffsets {
    No,
    Yes,
}

fn append_code_unit_offsets(
    offsets: &mut Vec<usize>,
    byte_count: usize,
    code_unit_count: usize,
    code_unit_offset: &mut usize,
) {
    for _ in 1..byte_count {
        offsets.push(*code_unit_offset);
    }
    offsets.push(*code_unit_offset + code_unit_count);
    *code_unit_offset += code_unit_count;
}

fn append_code_unit_offsets_of_utf8(offsets: &mut Vec<usize>, utf8: &[u8], code_unit_offset: &mut usize) {
    let mut index = 0;
    while index < utf8.len() {
        let byte_count = match utf8[index] {
            0xF0.. => 4,
            0xE0.. => 3,
            0xC0.. => 2,
            _ => 1,
        };
        append_code_unit_offsets(
            offsets,
            byte_count,
            if byte_count == 4 { 2 } else { 1 },
            code_unit_offset,
        );
        index += byte_count;
    }
}

fn json_text_bytes(text: Utf16View<'_>, track_code_unit_offsets: TrackCodeUnitOffsets) -> Option<JSONTextBytes<'_>> {
    let maximum_length = if text.has_ascii_storage() {
        text.length_in_code_units()
    } else {
        3 * text.length_in_code_units()
    };
    let mut utf8 = Vec::with_capacity(maximum_length + simdjson::SIMDJSON_PADDING);
    let mut offsets = Vec::new();

    match text {
        Utf16View::Ascii(bytes) => utf8.extend_from_slice(bytes),
        Utf16View::Utf16(code_units) => {
            let track_offsets = track_code_unit_offsets == TrackCodeUnitOffsets::Yes;
            let mut code_unit_offset = 0;
            if track_offsets {
                offsets.push(0);
            }

            let mut remaining = code_units;
            loop {
                let length_before = utf8.len();
                let valid_code_units = ak::append_utf16_as_utf8_up_to_unpaired_surrogate(&mut utf8, remaining);
                if track_offsets {
                    append_code_unit_offsets_of_utf8(&mut offsets, &utf8[length_before..], &mut code_unit_offset);
                }
                if valid_code_units == remaining.len() {
                    break;
                }

                let preceding_backslashes = utf8.iter().rev().take_while(|byte| **byte == b'\\').count();
                if preceding_backslashes % 2 != 0 {
                    return None;
                }
                let length_before = utf8.len();
                utf8.extend_from_slice(format!("\\u{:04X}", remaining[valid_code_units]).as_bytes());
                if track_offsets {
                    append_code_unit_offsets(&mut offsets, utf8.len() - length_before, 1, &mut code_unit_offset);
                }
                remaining = &remaining[valid_code_units + 1..];
            }
        }
    }

    let length = utf8.len();
    utf8.resize(length + simdjson::SIMDJSON_PADDING, 0);
    Some(JSONTextBytes {
        text,
        utf8,
        length,
        byte_to_code_unit_offsets: offsets,
    })
}

// The source text matched by a primitive parse node, used by JSON.parse revivers.
fn json_token_source(json_text: &JSONTextBytes<'_>, raw: &[u8]) -> Utf16String {
    let is_ascii_space = |byte: u8| matches!(byte, b' ' | b'\t' | b'\n' | 0x0B | 0x0C | b'\r');
    let mut start = 0;
    let mut end = raw.len();
    while start < end && is_ascii_space(raw[start]) {
        start += 1;
    }
    while end > start && is_ascii_space(raw[end - 1]) {
        end -= 1;
    }

    let token_offset = json_text.offset_of(raw);
    let token_start = token_offset + start;
    let token_end = token_offset + end;
    assert!(token_end <= json_text.bytes().len());

    let code_unit_start = json_text.byte_offset_to_code_unit_offset(token_start);
    let code_unit_end = json_text.byte_offset_to_code_unit_offset(token_end);
    json_text
        .text
        .substring_view(code_unit_start, code_unit_end - code_unit_start)
        .to_utf16_string()
}

/// The simdjson values JSONObject.cpp parses with templates: a document, or a value inside one.
trait SimdjsonValueOrDocument<'a> {
    fn get_double(&self) -> simdjson::SimdjsonResult<f64>;
    fn get_raw_json_string(&self) -> simdjson::SimdjsonResult<simdjson::RawJsonString>;
    fn get_array(&self) -> simdjson::SimdjsonResult<simdjson::Array<'_, 'a>>;
    fn get_object(&self) -> simdjson::SimdjsonResult<simdjson::Object<'_, 'a>>;
    fn is_document(&self) -> bool;
    fn at_end(&self) -> bool;
}

impl<'a> SimdjsonValueOrDocument<'a> for simdjson::ValueIterator<'_, 'a> {
    fn get_double(&self) -> simdjson::SimdjsonResult<f64> {
        simdjson::ValueIterator::get_double(self)
    }

    fn get_raw_json_string(&self) -> simdjson::SimdjsonResult<simdjson::RawJsonString> {
        simdjson::ValueIterator::get_raw_json_string(self)
    }

    fn get_array(&self) -> simdjson::SimdjsonResult<simdjson::Array<'_, 'a>> {
        simdjson::ValueIterator::get_array(self)
    }

    fn get_object(&self) -> simdjson::SimdjsonResult<simdjson::Object<'_, 'a>> {
        simdjson::ValueIterator::get_object(self)
    }

    fn is_document(&self) -> bool {
        false
    }

    fn at_end(&self) -> bool {
        unreachable!("only documents have an end")
    }
}

impl<'a> SimdjsonValueOrDocument<'a> for simdjson::Document<'a> {
    fn get_double(&self) -> simdjson::SimdjsonResult<f64> {
        simdjson::Document::get_double(self)
    }

    fn get_raw_json_string(&self) -> simdjson::SimdjsonResult<simdjson::RawJsonString> {
        simdjson::Document::get_raw_json_string(self)
    }

    fn get_array(&self) -> simdjson::SimdjsonResult<simdjson::Array<'_, 'a>> {
        simdjson::Document::get_array(self)
    }

    fn get_object(&self) -> simdjson::SimdjsonResult<simdjson::Object<'_, 'a>> {
        simdjson::Document::get_object(self)
    }

    fn is_document(&self) -> bool {
        true
    }

    fn at_end(&self) -> bool {
        simdjson::Document::at_end(self)
    }
}

fn ensure_simdjson_fully_parsed<'a, T: SimdjsonValueOrDocument<'a>>(vm: &Vm, value: &T) -> ThrowCompletionOr<()> {
    if value.is_document() && !value.at_end() {
        return json_malformed(vm);
    }
    Ok(())
}

fn parse_simdjson_number<'a, T: SimdjsonValueOrDocument<'a>>(
    vm: &Vm,
    value: &T,
    raw_sv: &[u8],
) -> ThrowCompletionOr<Value> {
    match value.get_double() {
        Ok(double_value) => {
            ensure_simdjson_fully_parsed(vm, value)?;
            return Ok(Value::from_f64(double_value));
        }
        Err(simdjson::ErrorCode::NumberError) => {}
        Err(_) => return json_malformed(vm),
    }

    let end_before_trailing_json_whitespace = raw_sv
        .iter()
        .rposition(|byte| !matches!(byte, b' ' | b'\t' | b'\n' | b'\r'))
        .map_or(0, |index| index + 1);
    let raw_sv = &raw_sv[..end_before_trailing_json_whitespace];

    // Validate JSON number format (parse_first_number is more lenient than spec)
    // - No leading zeros (except "0" or "0.xxx")
    // - No trailing decimal point (e.g., "1." is invalid)
    let mut i = 0;
    if i < raw_sv.len() && raw_sv[i] == b'-' {
        i += 1;
    }
    if i < raw_sv.len() && raw_sv[i] == b'0' && i + 1 < raw_sv.len() && raw_sv[i + 1].is_ascii_digit() {
        // Leading zero
        return json_malformed(vm);
    }
    while i < raw_sv.len() && raw_sv[i].is_ascii_digit() {
        i += 1;
    }
    if i < raw_sv.len() && raw_sv[i] == b'.' {
        i += 1;
        if i >= raw_sv.len() || !raw_sv[i].is_ascii_digit() {
            // Trailing decimal
            return json_malformed(vm);
        }
    }

    // Handle overflow to infinity (e.g., 1e309)
    // simdjson returns NUMBER_ERROR for numbers that overflow double
    // Use parse_first_number as fallback - it handles overflow correctly
    let raw_code_units: Vec<u16> = raw_sv.iter().map(|byte| u16::from(*byte)).collect();
    if let Some(result) = parse_first_number_f64(&raw_code_units)
        && result.characters_parsed == raw_sv.len()
    {
        return Ok(Value::from_f64(result.value));
    }

    json_malformed(vm)
}

fn parse_simdjson_string<'a, T: SimdjsonValueOrDocument<'a>>(
    vm: &Vm,
    json_text: &JSONTextBytes<'_>,
    value: &T,
) -> ThrowCompletionOr<Value> {
    // Use get_raw_json_string() to get the raw JSON string content (without quotes, with escapes),
    // then unescape ourselves to properly handle lone surrogates like \uD800 which simdjson rejects.
    let Ok(raw_string) = value.get_raw_json_string() else {
        return json_malformed(vm);
    };
    let raw = &json_text.utf8[raw_string.raw..];
    let remaining = &json_text.bytes()[raw_string.raw..];

    // Find the length by looking for the closing quote (simdjson validated the structure)
    let quote_offset = remaining
        .iter()
        .position(|byte| *byte == b'"')
        .expect("simdjson validated the structure");
    let candidate = &remaining[..quote_offset];

    let Some(backslash_offset) = candidate.iter().position(|byte| *byte == b'\\') else {
        let candidate = json_text.unescaped_text(candidate);
        return Ok(Value::from_string(PrimitiveString::create(
            vm,
            Utf16String::from_utf8(candidate),
        )));
    };

    let mut length = backslash_offset;
    while raw[length] != b'"' {
        if raw[length] == b'\\' {
            // Skip escaped character
            length += 1;
        }
        length += 1;
    }
    let Some(unescaped) = unescape_json_string(&raw[..length]) else {
        return json_malformed(vm);
    };
    Ok(Value::from_string(PrimitiveString::create(vm, unescaped)))
}

fn json_property_key(
    vm: &Vm,
    state: &mut JSONParseState<'_, '_, '_>,
    raw_key: &[u8],
) -> ThrowCompletionOr<PropertyKey> {
    let entry = state.property_keys.entry_for(raw_key);
    if let Some(key) = &entry.key
        && entry.raw_key == raw_key
    {
        return Ok(key.clone());
    }

    let key = if raw_key.contains(&b'\\') {
        let Some(unescaped_key) = unescape_json_string(raw_key) else {
            return json_malformed(vm);
        };
        PropertyKey::from(&unescaped_key)
    } else {
        PropertyKey::from(Utf16FlyString::from_utf8(state.text.unescaped_text(raw_key)))
    };
    let entry = state.property_keys.entry_for(raw_key);
    entry.raw_key = raw_key.to_vec();
    entry.key = Some(key.clone());
    Ok(key)
}

fn parse_simdjson_array<'a, T: SimdjsonValueOrDocument<'a>>(
    vm: &Vm,
    state: &mut JSONParseState<'_, '_, '_>,
    value: &T,
    record: Option<usize>,
) -> ThrowCompletionOr<Value> {
    let realm = vm.current_realm().expect("JSON is parsed in a realm");

    let Ok(simdjson_array) = value.get_array() else {
        return json_malformed(vm);
    };

    let array = Array::create(vm, realm, 0, None).must();
    let mut index: u64 = 0;

    while simdjson_array.is_open() {
        let Ok(element_value) = simdjson_array.current() else {
            return json_malformed(vm);
        };
        let element_record = record.map(|_| state.records().create());
        let parsed = parse_simdjson_value(vm, state, element_value, element_record)?;
        array.define_direct_property(vm, &PropertyKey::from_number(index), parsed, DEFAULT_ATTRIBUTES);
        index += 1;
        if let (Some(record), Some(element_record)) = (record, element_record) {
            state.records().records[record].elements.push(element_record);
        }
        simdjson_array.advance();
    }

    ensure_simdjson_fully_parsed(vm, value)?;
    if let Some(record) = record {
        state.records().set_value(record, Value::from_object(array));
    }
    Ok(Value::from_object(array))
}

fn parse_simdjson_object<'a, T: SimdjsonValueOrDocument<'a>>(
    vm: &Vm,
    state: &mut JSONParseState<'_, '_, '_>,
    value: &T,
    record: Option<usize>,
) -> ThrowCompletionOr<Value> {
    let realm = vm.current_realm().expect("JSON is parsed in a realm");

    let Ok(simdjson_object) = value.get_object() else {
        return json_malformed(vm);
    };

    let object = Object::create(vm, realm, Some(realm.object_prototype()));

    while simdjson_object.is_open() {
        let Ok(field) = simdjson_object.current() else {
            return json_malformed(vm);
        };
        // Use escaped_key() to get the raw JSON key (with escapes), then unescape ourselves
        let raw_key = field.escaped_key();
        let key = json_property_key(vm, state, raw_key)?;
        let field_value = field.value();
        let entry_record = record.map(|_| state.records().create());
        let parsed = parse_simdjson_value(vm, state, field_value, entry_record)?;
        object.define_direct_property(vm, &key, parsed, DEFAULT_ATTRIBUTES);
        if let (Some(record), Some(entry_record)) = (record, entry_record) {
            let records = state.records();
            records.records[entry_record].key = key.to_utf16_string();
            // Duplicate keys keep the last value, matching object property semantics.
            let existing = records.records[record]
                .entries
                .iter()
                .position(|entry| records.records[*entry].key == records.records[entry_record].key);
            match existing {
                Some(existing) => records.records[record].entries[existing] = entry_record,
                None => records.records[record].entries.push(entry_record),
            }
        }
        simdjson_object.advance();
    }

    ensure_simdjson_fully_parsed(vm, value)?;
    if let Some(record) = record {
        state.records().set_value(record, Value::from_object(object));
    }
    Ok(Value::from_object(object))
}

fn set_primitive_record(state: &mut JSONParseState<'_, '_, '_>, record: Option<usize>, value: Value, token: &[u8]) {
    let Some(record) = record else {
        return;
    };
    let source = json_token_source(state.text, token);
    let records = state.records();
    records.set_value(record, value);
    records.records[record].source = Some(source);
}

fn parse_simdjson_value(
    vm: &Vm,
    state: &mut JSONParseState<'_, '_, '_>,
    value: simdjson::ValueIterator<'_, '_>,
    record: Option<usize>,
) -> ThrowCompletionOr<Value> {
    // NB: The C++ runtime has no limit here and runs out of stack for deeply nested texts.
    if vm.did_reach_stack_space_limit() {
        return vm.throw_completion(ErrorKind::InternalError, ErrorType::CallStackSizeExceeded, &[]);
    }

    // NB: raw_json_token() must be captured before the value is consumed by the get_* calls below.
    match value.type_() {
        simdjson::JsonType::Null => {
            let token = value.raw_json_token();
            if value.is_null() != Ok(true) {
                return json_malformed(vm);
            }
            set_primitive_record(state, record, Value::NULL, token);
            Ok(Value::NULL)
        }
        simdjson::JsonType::Boolean => {
            let token = value.raw_json_token();
            let Ok(boolean_value) = value.get_bool() else {
                return json_malformed(vm);
            };
            set_primitive_record(state, record, Value::from_bool(boolean_value), token);
            Ok(Value::from_bool(boolean_value))
        }
        simdjson::JsonType::Number => {
            let raw = value.raw_json_token();
            let parsed = parse_simdjson_number(vm, &value, raw)?;
            set_primitive_record(state, record, parsed, raw);
            Ok(parsed)
        }
        simdjson::JsonType::String => {
            let token = value.raw_json_token();
            let parsed = parse_simdjson_string(vm, state.text, &value)?;
            set_primitive_record(state, record, parsed, token);
            Ok(parsed)
        }
        simdjson::JsonType::Array => parse_simdjson_array(vm, state, &value, record),
        simdjson::JsonType::Object => parse_simdjson_object(vm, state, &value, record),
        simdjson::JsonType::Unknown => json_malformed(vm),
    }
}

fn parse_simdjson_document(
    vm: &Vm,
    state: &mut JSONParseState<'_, '_, '_>,
    document: &simdjson::Document<'_>,
    record: Option<usize>,
) -> ThrowCompletionOr<Value> {
    let json_type = document.type_();

    // NB: raw_json_token() must be captured before the value is consumed by the get_* calls below.
    let raw_token = document.raw_json_token();

    match json_type {
        simdjson::JsonType::Null => {
            if document.is_null().is_err() {
                return json_malformed(vm);
            }
            if !document.at_end() {
                return json_malformed(vm);
            }
            set_primitive_record(state, record, Value::NULL, document.raw_json_token());
            Ok(Value::NULL)
        }
        simdjson::JsonType::Boolean => {
            let Ok(boolean_value) = document.get_bool() else {
                return json_malformed(vm);
            };
            let boolean_literal: &[u8] = if boolean_value { b"true" } else { b"false" };
            if !raw_token.starts_with(boolean_literal) {
                return json_malformed(vm);
            }
            if !document.at_end() {
                return json_malformed(vm);
            }
            set_primitive_record(state, record, Value::from_bool(boolean_value), raw_token);
            Ok(Value::from_bool(boolean_value))
        }
        simdjson::JsonType::Number => {
            if state.text.offset_of(raw_token) + raw_token.len() != state.text.bytes().len() {
                return json_malformed(vm);
            }
            let parsed = parse_simdjson_number(vm, document, raw_token)?;
            set_primitive_record(state, record, parsed, raw_token);
            Ok(parsed)
        }
        simdjson::JsonType::String => {
            let string_token = document.raw_json_token();
            let parsed = parse_simdjson_string(vm, state.text, document)?;
            set_primitive_record(state, record, parsed, string_token);
            Ok(parsed)
        }
        simdjson::JsonType::Array => parse_simdjson_array(vm, state, document, record),
        simdjson::JsonType::Object => parse_simdjson_object(vm, state, document, record),
        simdjson::JsonType::Unknown => json_malformed(vm),
    }
}

impl JSONObject {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<JSONObject> {
        realm.create_object(
            vm,
            JSONObject {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.object_prototype(),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;
        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &names.stringify,
            raw_native!(JSONObject::stringify),
            3,
            attr,
            None,
        );
        object.define_native_function(vm, realm, &names.parse, raw_native!(JSONObject::parse), 2, attr, None);
        object.define_native_function(
            vm,
            realm,
            &names.rawJSON,
            raw_native!(JSONObject::raw_json),
            1,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.isRawJSON,
            raw_native!(JSONObject::is_raw_json),
            1,
            attr,
            None,
        );

        // 25.5.3 JSON [ @@toStringTag ], https://tc39.es/ecma262/#sec-json-@@tostringtag
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_fly_string(
                vm,
                &Utf16FlyString::from_utf8("JSON"),
            )),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 25.5.2 JSON.stringify ( value [ , replacer [ , space ] ] ), https://tc39.es/ecma262/#sec-json.stringify
    /// The base implementation of stringify, which is exposed for the test runners to communicate with the scripts they
    /// run.
    pub fn stringify_impl(
        vm: &Vm,
        value: Value,
        replacer: Value,
        space: Value,
    ) -> ThrowCompletionOr<Option<Utf16String>> {
        let realm = vm.current_realm().expect("JSON is stringified in a realm");

        let mut state = StringifyState {
            replacer_function: None,
            object_stack: StringifyObjectStack::new(vm),
            shape_cache: StringifyShapeCache::new(vm),
            indent_depth: 0,
            gap: Utf16String::default(),
            property_list: None,
            builder: Utf16StringBuilder::new(),
        };

        if replacer.is_object() {
            if replacer.as_object().is_function() {
                state.replacer_function = Some(replacer.as_function());
            } else {
                let is_array = replacer.is_array(vm)?;
                if is_array {
                    let replacer_object = replacer.as_object();
                    let replacer_length = length_of_array_like(vm, &replacer_object)?;
                    let mut list: Vec<Utf16String> = Vec::new();
                    for i in 0..replacer_length {
                        let replacer_value = replacer_object.get(vm, &PropertyKey::from_number(i))?;
                        let mut item = None;
                        if replacer_value.is_string() {
                            item = Some(replacer_value.as_string().utf16_string());
                        } else if replacer_value.is_number() {
                            item = Some(replacer_value.to_utf16_string(vm).must());
                        } else if replacer_value.is_object() {
                            let value_object = replacer_value.as_object();
                            if value_object.is::<StringObject>() || value_object.is::<NumberObject>() {
                                item = Some(replacer_value.to_utf16_string(vm)?);
                            }
                        }
                        if let Some(item) = item
                            && !list.contains(&item)
                        {
                            list.push(item);
                        }
                    }
                    state.property_list = Some(Rc::new(list));
                }
            }
        }

        let mut space = space;
        let mut space_string = None;
        if space.is_string() {
            space_string = Some(space.as_string().utf16_string());
        } else if space.is_object() {
            let space_object = space.as_object();
            if space_object.is::<NumberObject>() {
                space = space.to_number(vm)?;
            } else if space_object.is::<StringObject>() {
                space_string = Some(space.to_utf16_string(vm)?);
            }
        }

        if space.is_number() {
            // NB: AK's min() converts the integer to an int, which saturates for infinities and large numbers.
            let space_mv = 10.min(space.to_integer_or_infinity(vm).must() as i32);
            state.gap = if space_mv < 1 {
                Utf16String::default()
            } else {
                Utf16String::from_utf8(&" ".repeat(space_mv as usize))
            };
        } else if let Some(space_string) = space_string {
            let space_string_view = Utf16View::of_string(&space_string);
            if space_string_view.length_in_code_units() <= 10 {
                state.gap = space_string;
            } else {
                state.gap = space_string_view.substring_view(0, 10).to_utf16_string();
            }
        } else {
            state.gap = Utf16String::default();
        }

        let wrapper = Object::create(vm, realm, Some(realm.object_prototype()));
        let empty_key = PropertyKey::from(Utf16FlyString::default());
        wrapper.create_data_property_or_throw(vm, &empty_key, value).must();

        let wrote_value = Self::serialize_json_property(vm, &mut state, &empty_key, wrapper)?;
        if !wrote_value {
            return Ok(None);
        }

        Ok(Some(state.builder.to_utf16_string()))
    }

    // 25.5.2 JSON.stringify ( value [ , replacer [ , space ] ] ), https://tc39.es/ecma262/#sec-json.stringify
    fn stringify(vm: &Vm) -> ThrowCompletionOr<Value> {
        if vm.argument_count() == 0 {
            return Ok(Value::UNDEFINED);
        }

        let value = vm.argument(0);
        let replacer = vm.argument(1);
        let space = vm.argument(2);

        let Some(string) = Self::stringify_impl(vm, value, replacer, space)? else {
            return Ok(Value::UNDEFINED);
        };

        Ok(Value::from_string(PrimitiveString::create(vm, string)))
    }

    // 25.5.2.1 SerializeJSONProperty ( state, key, holder ), https://tc39.es/ecma262/#sec-serializejsonproperty
    // 1.4.1 SerializeJSONProperty ( state, key, holder ), https://tc39.es/proposal-json-parse-with-source/#sec-serializejsonproperty
    // Returns true if a value was serialized, false if the value was undefined (should be omitted).
    fn serialize_json_property(
        vm: &Vm,
        state: &mut StringifyState<'_>,
        key: &PropertyKey,
        holder: Gc<Object>,
    ) -> ThrowCompletionOr<bool> {
        // 1. Let value be ? Get(holder, key).
        let value = holder.get(vm, key)?;

        Self::serialize_json_value(vm, state, key, holder, value)
    }

    fn serialize_json_value(
        vm: &Vm,
        state: &mut StringifyState<'_>,
        key: &PropertyKey,
        holder: Gc<Object>,
        mut value: Value,
    ) -> ThrowCompletionOr<bool> {
        // OPTIMIZATION: These primitive values do not perform a toJSON lookup. Without a replacer,
        //               they can be emitted before entering the observable part of the algorithm.
        if state.replacer_function.is_none() {
            if value.is_null() {
                state.builder.append_ascii("null");
                return Ok(true);
            }
            if value.is_boolean() {
                state
                    .builder
                    .append_ascii(if value.as_bool() { "true" } else { "false" });
                return Ok(true);
            }
            if value.is_string() {
                state
                    .builder
                    .append_quoted_escaped_for_json(value.as_string().utf16_string_view());
                return Ok(true);
            }
            if value.is_finite_number() {
                append_json_number(&mut state.builder, value);
                return Ok(true);
            }
        }

        // 2. If Type(value) is Object or BigInt, then
        if value.is_object() || value.is_bigint() {
            // a. Let toJSON be ? GetV(value, "toJSON").
            let to_json = value.get_with_cache(
                vm,
                &vm.names.toJSON,
                vm.static_property_lookup_cache(StaticPropertyLookupCacheSite::JSONSerializeToJSON),
            )?;

            // b. If IsCallable(toJSON) is true, then
            if to_json.is_function() {
                // i. Set value to ? Call(toJSON, value, « key »).
                let key_string = Value::from_string(PrimitiveString::create(vm, key.to_utf16_string()));
                value = call(vm, to_json, value, &[key_string])?;
            }
        }

        // 3. If state.[[ReplacerFunction]] is not undefined, then
        if let Some(replacer_function) = state.replacer_function {
            // a. Set value to ? Call(state.[[ReplacerFunction]], holder, « key, value »).
            let key_string = Value::from_string(PrimitiveString::create(vm, key.to_utf16_string()));
            value = call_function_object(vm, replacer_function, Value::from_object(holder), &[key_string, value])?;
        }

        // 4. If Type(value) is Object, then
        if value.is_object() {
            let value_object = value.as_object();

            // a. If value has an [[IsRawJSON]] internal slot, then
            if value_object.is::<RawJSONObject>() {
                // i. Return ! Get(value, "rawJSON").
                let raw_json = value_object.get(vm, &vm.names.rawJSON).must();
                state.builder.append(raw_json.as_string().utf16_string_view());
                return Ok(true);
            }
            // b. If value has a [[NumberData]] internal slot, then
            if value_object.is::<NumberObject>() {
                // i. Set value to ? ToNumber(value).
                value = value.to_number(vm)?;
            }
            // c. Else if value has a [[StringData]] internal slot, then
            else if value_object.is::<StringObject>() {
                // i. Set value to ? ToString(value).
                value = Value::from_string(PrimitiveString::create(vm, value.to_utf16_string(vm)?));
            }
            // d. Else if value has a [[BooleanData]] internal slot, then
            else if let Some(boolean) = value_object.downcast::<BooleanObject>() {
                // i. Set value to value.[[BooleanData]].
                value = Value::from_bool(boolean.boolean());
            }
            // e. Else if value has a [[BigIntData]] internal slot, then
            else if let Some(bigint) = value_object.downcast::<BigIntObject>() {
                // i. Set value to value.[[BigIntData]].
                value = Value::from_bigint(bigint.bigint());
            }
        }

        // 5. If value is null, return "null".
        if value.is_null() {
            state.builder.append_ascii("null");
            return Ok(true);
        }

        // 6. If value is true, return "true".
        // 7. If value is false, return "false".
        if value.is_boolean() {
            state
                .builder
                .append_ascii(if value.as_bool() { "true" } else { "false" });
            return Ok(true);
        }

        // 8. If Type(value) is String, return QuoteJSONString(value).
        if value.is_string() {
            state
                .builder
                .append_quoted_escaped_for_json(value.as_string().utf16_string_view());
            return Ok(true);
        }

        // 9. If Type(value) is Number, then
        if value.is_number() {
            // a. If value is finite, return ! ToString(value).
            if value.is_finite_number() {
                append_json_number(&mut state.builder, value);
                return Ok(true);
            }

            // b. Return "null".
            state.builder.append_ascii("null");
            return Ok(true);
        }

        // 10. If Type(value) is BigInt, throw a TypeError exception.
        if value.is_bigint() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::JsonBigInt, &[]);
        }

        // 11. If Type(value) is Object and IsCallable(value) is false, then
        if value.is_object() && !value.is_function() {
            let value_object = value.as_object();

            if value_object.is::<Array>() {
                Self::serialize_json_array(vm, state, value_object)?;
                return Ok(true);
            }

            // a. Let isArray be ? IsArray(value).
            let is_array = value_object.is_proxy_object() && value.is_array(vm)?;

            // b. If isArray is true, return ? SerializeJSONArray(state, value).
            if is_array {
                Self::serialize_json_array(vm, state, value_object)?;
                return Ok(true);
            }

            // c. Return ? SerializeJSONObject(state, value).
            Self::serialize_json_object(vm, state, value_object)?;
            return Ok(true);
        }

        // 12. Return undefined.
        Ok(false)
    }

    fn serialize_json_object_property(
        vm: &Vm,
        state: &mut StringifyState<'_>,
        object: Gc<Object>,
        key: &PropertyKey,
        first: &mut bool,
    ) -> ThrowCompletionOr<()> {
        if key.is_symbol() {
            return Ok(());
        }

        // Mark position before writing anything for this property
        let mark = state.builder.len();

        // Write separator (comma and possibly newline/indent)
        if !*first {
            state.builder.append_ascii(",");
            if !Utf16View::of_string(&state.gap).is_empty() {
                state.builder.append_ascii("\n");
                write_indent(&mut state.builder, &state.gap, state.indent_depth);
            }
        } else if !Utf16View::of_string(&state.gap).is_empty() {
            state.builder.append_ascii("\n");
            write_indent(&mut state.builder, &state.gap, state.indent_depth);
        }

        // Write key and colon
        state
            .builder
            .append_quoted_escaped_for_json(Utf16View::of_string(&key.to_utf16_string()));
        state.builder.append_ascii(":");
        if !Utf16View::of_string(&state.gap).is_empty() {
            state.builder.append_ascii(" ");
        }

        // Serialize value
        let wrote_value = Self::serialize_json_property(vm, state, key, object)?;

        if wrote_value {
            *first = false;
        } else {
            // Rollback - value was undefined, remove everything we wrote for this property
            state.builder.truncate(mark);
        }
        Ok(())
    }

    fn serialize_json_own_properties(
        vm: &Vm,
        state: &mut StringifyState<'_>,
        object: Gc<Object>,
        first: &mut bool,
    ) -> ThrowCompletionOr<()> {
        let property_list = MarkedVec::with_capacity(vm, object.own_properties_count());
        object.for_each_own_property_with_enumerability(vm, |property_key, enumerable| {
            if enumerable {
                property_list.push(property_key.clone());
            }
            Ok(())
        })?;
        for index in 0..property_list.len() {
            let property = property_list.get(index).expect("the index is in bounds");
            Self::serialize_json_object_property(vm, state, object, &property, first)?;
        }
        Ok(())
    }

    // 25.5.2.4 SerializeJSONObject ( state, value ), https://tc39.es/ecma262/#sec-serializejsonobject
    fn serialize_json_object(vm: &Vm, state: &mut StringifyState<'_>, object: Gc<Object>) -> ThrowCompletionOr<()> {
        if vm.did_reach_stack_space_limit() {
            return vm.throw_completion(ErrorKind::InternalError, ErrorType::CallStackSizeExceeded, &[]);
        }

        if state.object_stack.contains(object) {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::JsonCircular, &[]);
        }

        state.object_stack.push(object);
        state.indent_depth += 1;

        state.builder.append_ascii("{");
        let position_after_open_brace = state.builder.len();
        let mut first = true;

        if let Some(property_list) = state.property_list.clone() {
            for property in property_list.iter() {
                Self::serialize_json_object_property(vm, state, object, &PropertyKey::from(property), &mut first)?;
            }
        } else if state.replacer_function.is_none()
            && Utf16View::of_string(&state.gap).is_empty()
            && can_use_direct_property_access(&object)
        {
            let initial_shape = object.shape();
            let mut shape_cache = state.shape_cache.get(initial_shape);
            if shape_cache.is_none() {
                shape_cache = state.shape_cache.add(initial_shape, || {
                    let mut properties = Vec::with_capacity(initial_shape.property_count() as usize);
                    initial_shape.for_each_property_in_insertion_order(|property_key, metadata| {
                        if property_key.is_symbol() || !metadata.attributes.is_enumerable() {
                            return ControlFlow::Continue(());
                        }
                        assert!(property_key.is_string());

                        let mut key_builder = Utf16StringBuilder::new();
                        key_builder.append_quoted_escaped_for_json(Utf16View::of_fly_string(property_key.as_string()));
                        key_builder.append_ascii(":");

                        properties.push(StringifyCachedProperty {
                            property: property_key.clone(),
                            offset: metadata.offset,
                            serialized_key: key_builder.to_utf16_string(),
                        });
                        ControlFlow::Continue(())
                    });
                    properties
                });
            }

            match shape_cache {
                None => Self::serialize_json_own_properties(vm, state, object, &mut first)?,
                Some(shape_cache) => {
                    for property in &shape_cache.properties {
                        if object.shape() == initial_shape && !object.get_direct(property.offset).is_accessor() {
                            let value = object.get_direct(property.offset);

                            let mark = state.builder.len();
                            if !first {
                                state.builder.append_ascii(",");
                            }
                            state.builder.append(Utf16View::of_string(&property.serialized_key));

                            let wrote_value = Self::serialize_json_value(vm, state, &property.property, object, value)?;
                            if wrote_value {
                                first = false;
                            } else {
                                state.builder.truncate(mark);
                            }
                        } else {
                            Self::serialize_json_object_property(vm, state, object, &property.property, &mut first)?;
                        }
                    }
                }
            }
        } else {
            Self::serialize_json_own_properties(vm, state, object, &mut first)?;
        }

        // Close the object
        state.indent_depth -= 1;
        if state.builder.len() > position_after_open_brace && !Utf16View::of_string(&state.gap).is_empty() {
            state.builder.append_ascii("\n");
            write_indent(&mut state.builder, &state.gap, state.indent_depth);
        }
        state.builder.append_ascii("}");

        state.object_stack.pop();
        Ok(())
    }

    // 25.5.2.5 SerializeJSONArray ( state, value ), https://tc39.es/ecma262/#sec-serializejsonarray
    fn serialize_json_array(vm: &Vm, state: &mut StringifyState<'_>, object: Gc<Object>) -> ThrowCompletionOr<()> {
        if vm.did_reach_stack_space_limit() {
            return vm.throw_completion(ErrorKind::InternalError, ErrorType::CallStackSizeExceeded, &[]);
        }

        if state.object_stack.contains(object) {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::JsonCircular, &[]);
        }

        state.object_stack.push(object);
        state.indent_depth += 1;

        // OPTIMIZATION: Packed arrays have no holes or indexed accessors, so their length and
        //               elements can be read directly while they remain packed.
        let simple_array = object
            .downcast::<Array>()
            .filter(|array| array.is_simple_packed_array());

        let length = match simple_array {
            Some(array) => u64::from(array.indexed_array_like_size()),
            None => length_of_array_like(vm, &object)?,
        };

        state.builder.append_ascii("[");

        for i in 0..length {
            // Write separator
            let has_gap = !Utf16View::of_string(&state.gap).is_empty();
            if i > 0 {
                state.builder.append_ascii(",");
                if has_gap {
                    state.builder.append_ascii("\n");
                    write_indent(&mut state.builder, &state.gap, state.indent_depth);
                }
            } else if has_gap {
                state.builder.append_ascii("\n");
                write_indent(&mut state.builder, &state.gap, state.indent_depth);
            }

            // Serialize value (undefined becomes null for arrays)
            let key = PropertyKey::from_number(i);
            let wrote_value = match simple_array {
                Some(array) if array.is_simple_packed_array() && i < u64::from(array.indexed_array_like_size()) => {
                    let value = array
                        .indexed_get(i as u32)
                        .expect("a packed array has its elements")
                        .value;
                    Self::serialize_json_value(vm, state, &key, object, value)?
                }
                _ => Self::serialize_json_property(vm, state, &key, object)?,
            };
            if !wrote_value {
                state.builder.append_ascii("null");
            }
        }

        // Close the array
        state.indent_depth -= 1;
        if length > 0 && !Utf16View::of_string(&state.gap).is_empty() {
            state.builder.append_ascii("\n");
            write_indent(&mut state.builder, &state.gap, state.indent_depth);
        }
        state.builder.append_ascii("]");

        state.object_stack.pop();
        Ok(())
    }

    // 25.5.1 JSON.parse ( text [ , reviver ] ), https://tc39.es/ecma262/#sec-json.parse
    fn parse(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("JSON is parsed in a realm");

        let text = vm.argument(0);
        let reviver = vm.argument(1);

        // 1. Let jsonString be ? ToString(text).
        let json_string = text.to_utf16_string(vm)?;

        // 2. Let parseResult be ? ParseJSON(jsonString).
        // 3. Let unfiltered be parseResult.[[Value]].
        // NB: We build the JSON Parse Record snapshot inline during parsing, but only
        //     when a reviver is present and may observe the matched source text.
        let mut root_record = reviver.is_function().then(|| JSONParseRecords::new(vm));
        let unfiltered = Self::parse_json(vm, Utf16View::of_string(&json_string), root_record.as_mut())?;

        // 4. If IsCallable(reviver) is false, return unfiltered.
        let Some(root_record) = root_record else {
            return Ok(unfiltered);
        };

        // 5. Let root be OrdinaryObjectCreate(%Object.prototype%).
        let root = Object::create(vm, realm, Some(realm.object_prototype()));

        // 6. Let rootName be the empty String.
        let root_name = PropertyKey::from(Utf16FlyString::default());

        // 7. Perform ! CreateDataPropertyOrThrow(root, rootName, unfiltered).
        root.create_data_property_or_throw(vm, &root_name, unfiltered).must();

        // 8. Let snapshot be CreateJSONParseRecord(parseResult.[[ParseNode]], rootName, unfiltered).
        // NB: The records keep every value they reference alive themselves.

        // 9. Return ? InternalizeJSONProperty(root, rootName, reviver, snapshot).
        Self::internalize_json_property(
            vm,
            root,
            &root_name,
            reviver.as_function(),
            &root_record,
            Some(JSONParseRecords::ROOT),
        )
    }

    // 25.5.1.1 ParseJSON ( text ), https://tc39.es/ecma262/#sec-ParseJSON
    pub fn parse_json(
        vm: &Vm,
        text: Utf16View<'_>,
        root_record: Option<&mut JSONParseRecords<'_>>,
    ) -> ThrowCompletionOr<Value> {
        // 1. If StringToCodePoints(text) is not a valid JSON text as specified in ECMA-404, throw a SyntaxError exception.
        // NB: Per ECMA-404, the BOM is not valid JSON whitespace. simdjson silently skips it, so we must reject it explicitly.
        if text.length_in_code_units() >= 1 && text.code_unit_at(0) == 0xFEFF {
            return json_malformed(vm);
        }

        let track_code_unit_offsets = if root_record.is_some() {
            TrackCodeUnitOffsets::Yes
        } else {
            TrackCodeUnitOffsets::No
        };
        let Some(text_bytes) = json_text_bytes(text, track_code_unit_offsets) else {
            return json_malformed(vm);
        };

        let Ok(document) = simdjson::iterate(&text_bytes.utf8, text_bytes.length) else {
            return json_malformed(vm);
        };

        let record = root_record.is_some().then_some(JSONParseRecords::ROOT);
        let mut state = JSONParseState {
            text: &text_bytes,
            property_keys: JSONPropertyKeyCache::new(),
            records: root_record,
        };

        // 2. Let scriptString be the string-concatenation of "(", text, and ");".
        // 3. Let script be ParseText(scriptString, Script).
        // 4. NOTE: The early error rules defined in 13.2.5.1 have special handling for the above invocation of ParseText.
        // 5. Assert: script is a Parse Node.
        // 6. Let result be ! Evaluation of script.
        let result = parse_simdjson_document(vm, &mut state, &document, record)?;

        // 7. NOTE: The PropertyDefinitionEvaluation semantics defined in 13.2.5.5 have special handling for the above evaluation.
        // 8. Assert: result is either a String, a Number, a Boolean, an Object that is defined by either an ArrayLiteral or an ObjectLiteral, or null.

        // 9. Return result.
        Ok(result)
    }

    // 25.5.1.1 InternalizeJSONProperty ( holder, name, reviver, parseRecord ), https://tc39.es/ecma262/#sec-internalizejsonproperty
    fn internalize_json_property(
        vm: &Vm,
        holder: Gc<Object>,
        name: &PropertyKey,
        reviver: Gc<FunctionObject>,
        records: &JSONParseRecords<'_>,
        parse_record: Option<usize>,
    ) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("JSON is parsed in a realm");

        // 1. Let value be ? Get(holder, name).
        let value = holder.get(vm, name)?;

        // 2. Let context be OrdinaryObjectCreate(%Object.prototype%).
        let context = Object::create(vm, realm, Some(realm.object_prototype()));

        // 3. If parseRecord is a JSON Parse Record and SameValue(parseRecord.[[Value]], value) is true, then
        // NB: An empty List of records is represented as None here.
        let mut element_records: Option<&[usize]> = None;
        let mut entry_records: Option<&[usize]> = None;
        if let Some(parse_record) = parse_record
            && same_value(records.value(parse_record), value)
        {
            let parse_record = &records.records[parse_record];

            // a. If value is not an Object, then
            if !value.is_object() {
                // i. Let parseNode be parseRecord.[[ParseNode]].
                // ii. Assert: parseNode is neither an ArrayLiteral Parse Node nor an ObjectLiteral Parse Node.
                // iii. Let sourceText be the source text matched by parseNode.
                // iv. Perform ! CreateDataPropertyOrThrow(context, "source", CodePointsToString(sourceText)).
                // NB: We captured sourceText while parsing rather than from a retained parse node.
                let source = parse_record
                    .source
                    .clone()
                    .expect("a primitive's record has its source");
                context
                    .create_data_property_or_throw(
                        vm,
                        &vm.names.source,
                        Value::from_string(PrimitiveString::create(vm, source)),
                    )
                    .must();
            }
            // b. Let elementRecords be parseRecord.[[Elements]].
            element_records = Some(&parse_record.elements);
            // c. Let entryRecords be parseRecord.[[Entries]].
            entry_records = Some(&parse_record.entries);
        }
        // 4. Else,
        //     a. Let elementRecords be a new empty List.
        //     b. Let entryRecords be a new empty List.

        // 5. If value is an Object, then
        if value.is_object() {
            // a. Let isArray be ? IsArray(value).
            let is_array = value.is_array(vm)?;

            let value_object = value.as_object();
            let process_property = |key: &PropertyKey, child_record: Option<usize>| -> ThrowCompletionOr<()> {
                // i/ii/iii. Let newElement be ? InternalizeJSONProperty(value, propertyKey, reviver, elementRecord/entryRecord).
                let new_element =
                    Self::internalize_json_property(vm, value_object, key, reviver, records, child_record)?;
                // If newElement is undefined, perform ? value.[[Delete]](propertyKey).
                if new_element.is_undefined() {
                    value_object.internal_delete(vm, key)?;
                }
                // Else, perform ? CreateDataProperty(value, propertyKey, newElement).
                else {
                    value_object.create_data_property(vm, key, new_element, None, None)?;
                }
                Ok(())
            };

            // b. If isArray is true, then
            if is_array {
                // i. Let elementRecordsLength be the number of elements in elementRecords.
                let element_records_length = element_records.map_or(0, <[usize]>::len);
                // ii. Let length be ? LengthOfArrayLike(value).
                let length = length_of_array_like(vm, &value_object)?;
                // iii-iv. Repeat, while index < length,
                for index in 0..length {
                    // 2. If index < elementRecordsLength, let elementRecord be elementRecords[index]; else let elementRecord be empty.
                    let element_record = element_records
                        .filter(|_| index < element_records_length as u64)
                        .map(|element_records| element_records[index as usize]);
                    process_property(&PropertyKey::from_number(index), element_record)?;
                }
            }
            // c. Else,
            else {
                // i. Let keys be ? EnumerableOwnProperties(value, key).
                let keys = value_object.enumerable_own_property_names(vm, PropertyKind::Key)?;
                // ii. For each String propertyKey of keys, do
                for index in 0..keys.len() {
                    let property_key = keys.get(index).expect("the index is in bounds");
                    let key = property_key.as_string().utf16_string();
                    // 1. If there exists an element entry of entryRecords such that entry.[[Key]] is propertyKey,
                    //    let entryRecord be entry; else let entryRecord be empty.
                    let entry_record = entry_records.and_then(|entry_records| {
                        entry_records
                            .iter()
                            .find(|entry| records.records[**entry].key == key)
                            .copied()
                    });
                    process_property(&PropertyKey::from(&key), entry_record)?;
                }
            }
        }

        // 6. Return ? Call(reviver, holder, « name, value, context »).
        let name_string = Value::from_string(PrimitiveString::create(vm, name.to_utf16_string()));
        call_function_object(
            vm,
            reviver,
            Value::from_object(holder),
            &[name_string, value, Value::from_object(context)],
        )
    }

    // 25.5.3 JSON.rawJSON ( text ), https://tc39.es/ecma262/#sec-json.rawjson
    fn raw_json(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("JSON.rawJSON runs in a realm");

        // 1. Let jsonString be ? ToString(text).
        let json_string = vm.argument(0).to_utf16_string(vm)?;
        let json_string_view = Utf16View::of_string(&json_string);

        // 2. If jsonString is the empty String, throw a SyntaxError exception.
        if json_string_view.is_empty() {
            return json_malformed(vm);
        }

        // 3. If the first code unit of jsonString is not either an ASCII lowercase letter code unit (0x0061 through
        //    0x007A, inclusive), an ASCII digit code unit (0x0030 through 0x0039, inclusive), 0x0022 (QUOTATION MARK), or
        //    0x002D (HYPHEN-MINUS), throw a SyntaxError exception.
        let first_code_unit = u8::try_from(json_string_view.code_unit_at(0));
        if !matches!(first_code_unit, Ok(b'a'..=b'z' | b'0'..=b'9' | b'"' | b'-')) {
            let error_type = if matches!(first_code_unit, Ok(b'{' | b'[')) {
                ErrorType::JsonRawJSONNonPrimitive
            } else {
                ErrorType::JsonMalformed
            };
            return vm.throw_completion(ErrorKind::SyntaxError, error_type, &[]);
        }

        // 4. If the last code unit of jsonString is not either an ASCII lowercase letter code unit (0x0061 through 0x007A,
        //    inclusive), an ASCII digit code unit (0x0030 through 0x0039, inclusive), or 0x0022 (QUOTATION MARK), throw a
        //    SyntaxError exception.
        let last_code_unit = u8::try_from(json_string_view.code_unit_at(json_string_view.length_in_code_units() - 1));
        if !matches!(last_code_unit, Ok(b'a'..=b'z' | b'0'..=b'9' | b'"')) {
            return json_malformed(vm);
        }

        // 5. Let parseResult be ? ParseJSON(jsonString).
        let parse_result = Self::parse_json(vm, json_string_view, None)?;

        // 6. Assert: parseResult.[[Value]] is either a String, a Number, a Boolean, or null.
        assert!(
            parse_result.is_string() || parse_result.is_number() || parse_result.is_boolean() || parse_result.is_null()
        );

        // 7. Let internalSlotsList be « [[IsRawJSON]] ».
        // 8. Let obj be OrdinaryObjectCreate(null, internalSlotsList).
        let object = RawJSONObject::create(vm, realm, None);

        // 9. Perform ! CreateDataPropertyOrThrow(obj, "rawJSON", jsonString).
        object
            .create_data_property_or_throw(
                vm,
                &vm.names.rawJSON,
                Value::from_string(PrimitiveString::create(vm, json_string)),
            )
            .must();

        // 10. Perform ! SetIntegrityLevel(obj, frozen).
        object.set_integrity_level(vm, IntegrityLevel::Frozen).must();

        // 11. Return obj.
        Ok(Value::from_object(object))
    }

    // 1.1 JSON.isRawJSON ( O ), https://tc39.es/proposal-json-parse-with-source/#sec-json.israwjson
    #[allow(clippy::unnecessary_wraps, reason = "native functions can throw")]
    fn is_raw_json(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. If Type(O) is Object and O has an [[IsRawJSON]] internal slot, return true.
        // 2. Return false.
        let argument = vm.argument(0);
        Ok(Value::from_bool(
            argument.is_object() && argument.as_object().is::<RawJSONObject>(),
        ))
    }
}
