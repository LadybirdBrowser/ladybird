/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The JSON text parser of JSON.parse without a reviver. It reads the code units of the text once and makes the values
//! of the parse nodes as it goes, since nothing but the result needs the parse nodes when there is no reviver to give
//! their source text to.

use core::cell::{Cell, RefCell};
use core::hash::{Hash, Hasher};

use ak::{Utf16FlyString, Utf16String};

use crate::gc::visitor::{Trace, Visitor};
use crate::gc::weak::GcWeak;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::array::Array;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::object::MAX_TRANSITIONS_BEFORE_CONVERTING_TO_DICTIONARY;
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::DEFAULT_ATTRIBUTES;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::shape::Shape;
use crate::utf16::{Utf16View, is_json_special_code_unit, json_special_code_unit_lanes};

/// The code units of a JSON text: ASCII bytes or UTF-16 code units.
trait CodeUnit: Copy + Eq + Into<u32> + 'static {
    fn make_string(units: &[Self]) -> Utf16String;
    fn make_primitive_string(vm: &Vm, units: &[Self]) -> Gc<PrimitiveString>;
    fn make_fly_string(units: &[Self]) -> Utf16FlyString;

    /// The position of the first unit at or after `position` that is a quotation mark, a reverse solidus or a
    /// control character, or the length of `units` if there is none.
    fn skip_ordinary_string_units(units: &[Self], position: usize) -> usize;
}

impl CodeUnit for u8 {
    fn make_string(units: &[u8]) -> Utf16String {
        Utf16String::from_ascii(units)
    }

    fn make_primitive_string(vm: &Vm, units: &[u8]) -> Gc<PrimitiveString> {
        PrimitiveString::create_from_ascii(vm, units)
    }

    fn make_fly_string(units: &[u8]) -> Utf16FlyString {
        Utf16FlyString::from_utf8(core::str::from_utf8(units).expect("the text is ASCII"))
    }

    fn skip_ordinary_string_units(units: &[u8], mut position: usize) -> usize {
        while let Some(chunk) = units.get(position..position + 8) {
            let word = u64::from_le_bytes(chunk.try_into().expect("the chunk has eight units"));
            let special_lanes = json_special_code_unit_lanes(word, 0x0101_0101_0101_0101);
            if special_lanes != 0 {
                return position + (special_lanes.trailing_zeros() / 8) as usize;
            }
            position += 8;
        }
        while position < units.len() && !is_json_special_code_unit(u32::from(units[position])) {
            position += 1;
        }
        position
    }
}

impl CodeUnit for u16 {
    fn make_string(units: &[u16]) -> Utf16String {
        Utf16String::from_utf16(units)
    }

    fn make_primitive_string(vm: &Vm, units: &[u16]) -> Gc<PrimitiveString> {
        PrimitiveString::create(vm, Utf16String::from_utf16(units))
    }

    fn make_fly_string(units: &[u16]) -> Utf16FlyString {
        Utf16FlyString::from_utf16(units)
    }

    fn skip_ordinary_string_units(units: &[u16], mut position: usize) -> usize {
        while let Some(chunk) = units.get(position..position + 4) {
            let word = chunk.iter().rev().fold(0, |word, unit| (word << 16) | u64::from(*unit));
            let special_lanes = json_special_code_unit_lanes(word, 0x0001_0001_0001_0001);
            if special_lanes != 0 {
                return position + (special_lanes.trailing_zeros() / 16) as usize;
            }
            position += 4;
        }
        while position < units.len() && !is_json_special_code_unit(u32::from(units[position])) {
            position += 1;
        }
        position
    }
}

/// What JSON.parse keeps between the texts it parses: the property keys and the shapes of the objects of the texts it
/// parsed last, which later texts usually share, and the values of the arrays and objects it has not finished making.
#[derive(Default)]
pub struct JsonParseCache {
    /// The elements of the arrays and the values of the properties of the objects that are being parsed, which the
    /// VM keeps alive until their array or object is made.
    values: RefCell<Vec<Value>>,
    tables: Cell<Option<Box<JsonParseTables>>>,
}

impl JsonParseCache {
    pub fn trace(&self, visitor: &mut Visitor) {
        self.values.borrow().trace(visitor);
    }
}

#[derive(Default)]
struct JsonParseTables {
    keys: KeyCache,
    shapes: ShapeCache,
    /// The keys of the properties of the objects that are being parsed.
    property_keys: Vec<PropertyKey>,
}

/// Parses `text` as a JSON text, and returns the value of its parse node as ParseJSON does, or throws a SyntaxError
/// if it is not a valid JSON text as specified in ECMA-404.
pub fn parse_json_text(vm: &Vm, realm: Gc<Realm>, text: Utf16View<'_>) -> ThrowCompletionOr<Value> {
    let cache = vm.json_parse_cache();
    let mut tables = cache.tables.take().unwrap_or_default();
    let first_value = cache.values.borrow().len();
    let first_key = tables.property_keys.len();
    let result = match text {
        Utf16View::Ascii(bytes) => Parser::new(vm, realm, &mut tables, bytes).parse_text(),
        Utf16View::Utf16(code_units) => Parser::new(vm, realm, &mut tables, code_units).parse_text(),
    };
    // NB: A text that is not valid leaves the values and keys of what it did not finish.
    cache.values.borrow_mut().truncate(first_value);
    tables.property_keys.truncate(first_key);
    cache.tables.set(Some(tables));
    result
}

fn json_malformed<T>(vm: &Vm) -> ThrowCompletionOr<T> {
    vm.throw_completion(ErrorKind::SyntaxError, ErrorType::JsonMalformed, &[])
}

/// Whether `units` are the code units of `key`.
fn are_code_units_of<C: CodeUnit>(units: &[C], key: &PropertyKey) -> bool {
    match Utf16View::of_fly_string(key.as_string()) {
        Utf16View::Ascii(bytes) => {
            bytes.len() == units.len()
                && bytes
                    .iter()
                    .zip(units)
                    .all(|(byte, unit)| u32::from(*byte) == (*unit).into())
        }
        Utf16View::Utf16(code_units) => {
            code_units.len() == units.len()
                && code_units
                    .iter()
                    .zip(units)
                    .all(|(code_unit, unit)| u32::from(*code_unit) == (*unit).into())
        }
    }
}

/// Property keys by their code units, in a small table indexed by a hash of them.
struct KeyCache {
    entries: Box<[Option<PropertyKey>; KeyCache::ENTRY_COUNT]>,
}

impl Default for KeyCache {
    fn default() -> Self {
        Self {
            entries: Box::new([const { None }; KeyCache::ENTRY_COUNT]),
        }
    }
}

impl KeyCache {
    const ENTRY_COUNT: usize = 64;

    fn slot<C: CodeUnit>(units: &[C]) -> usize {
        let (Some(first), Some(last)) = (units.first(), units.last()) else {
            return 0;
        };
        let middle: u32 = units[units.len() / 2].into();
        let hash = (units.len() as u32)
            .wrapping_mul(0x9E37_79B9)
            .wrapping_add((*first).into())
            .wrapping_mul(0x85EB_CA6B)
            .wrapping_add(middle)
            .wrapping_mul(0xC2B2_AE35)
            .wrapping_add((*last).into());
        (hash ^ (hash >> 15)) as usize % Self::ENTRY_COUNT
    }

    fn key<C: CodeUnit>(&mut self, units: &[C]) -> PropertyKey {
        let entry = &mut self.entries[Self::slot(units)];
        if let Some(key) = entry
            && are_code_units_of(units, key)
        {
            return key.clone();
        }
        let key = PropertyKey::from(C::make_fly_string(units));
        // NB: Array indices are not kept, since they are numbers rather than strings.
        if key.is_string() {
            *entry = Some(key.clone());
        }
        key
    }
}

/// The shapes of objects by their keys, in a small table indexed by a hash of them. The shapes are weak, so that the
/// table never keeps a realm alive.
struct ShapeCache {
    entries: Box<[Option<ShapeCacheEntry>; ShapeCache::ENTRY_COUNT]>,
}

struct ShapeCacheEntry {
    keys: Box<[PropertyKey]>,
    shape: GcWeak<Shape>,
}

impl Default for ShapeCache {
    fn default() -> Self {
        Self {
            entries: Box::new([const { None }; ShapeCache::ENTRY_COUNT]),
        }
    }
}

impl ShapeCache {
    const ENTRY_COUNT: usize = 16;

    fn slot(keys: &[PropertyKey]) -> usize {
        let mut hasher = KeysHasher(keys.len() as u64);
        keys.hash(&mut hasher);
        hasher.finish() as usize % Self::ENTRY_COUNT
    }

    /// The shape of an object of `realm` with the properties `keys` in that order, as data properties with the default
    /// attributes, made by put transitions from the shape of new objects. Returns None for keys that an object
    /// cannot have in that shape: a key that repeats, an array index, or more keys than a shape takes.
    fn shape_for_keys(&mut self, vm: &Vm, realm: Gc<Realm>, keys: &[PropertyKey]) -> Option<Gc<Shape>> {
        if keys.len() > MAX_TRANSITIONS_BEFORE_CONVERTING_TO_DICTIONARY as usize {
            return None;
        }
        let slot = Self::slot(keys);
        if let Some(entry) = &self.entries[slot]
            && let Some(shape) = entry.shape.get()
            && shape.realm() == realm
            && *entry.keys == *keys
        {
            return Some(shape);
        }

        let mut shape = realm.new_object_shape();
        for key in keys {
            if key.is_number() || shape.lookup(key).is_some() {
                return None;
            }
            shape = shape.create_put_transition(vm, key, DEFAULT_ATTRIBUTES);
        }
        self.entries[slot] = Some(ShapeCacheEntry {
            keys: keys.into(),
            shape: GcWeak::new(vm.heap(), shape),
        });
        Some(shape)
    }
}

/// Mixes the bits of property keys, which are interned, for the slot of their shape.
struct KeysHasher(u64);

impl Hasher for KeysHasher {
    fn finish(&self) -> u64 {
        self.0 ^ (self.0 >> 29)
    }

    fn write(&mut self, bytes: &[u8]) {
        for byte in bytes {
            self.write_u64(u64::from(*byte));
        }
    }

    fn write_u64(&mut self, value: u64) {
        self.0 = (self.0.rotate_left(5) ^ value).wrapping_mul(0x517c_c1b7_2722_0a95);
    }

    fn write_usize(&mut self, value: usize) {
        self.write_u64(value as u64);
    }
}

struct Parser<'a, 'vm, C: CodeUnit> {
    vm: &'vm Vm,
    realm: Gc<Realm>,
    tables: &'a mut JsonParseTables,
    values: &'vm RefCell<Vec<Value>>,
    units: &'a [C],
    position: usize,
}

/// Where a string of the text is, with what tells how to make its value.
struct StringToken {
    start: usize,
    end: usize,
    has_escapes: bool,
}

impl<'a, 'vm, C: CodeUnit> Parser<'a, 'vm, C> {
    fn new(vm: &'vm Vm, realm: Gc<Realm>, tables: &'a mut JsonParseTables, units: &'a [C]) -> Self {
        Self {
            vm,
            realm,
            tables,
            values: &vm.json_parse_cache().values,
            units,
            position: 0,
        }
    }

    fn peek(&self) -> Option<u32> {
        self.units.get(self.position).map(|unit| (*unit).into())
    }

    fn skip_whitespace(&mut self) {
        // NB: JSON whitespace is only tab, line feed, carriage return and space.
        while let Some(unit) = self.peek()
            && matches!(unit, 0x09 | 0x0A | 0x0D | 0x20)
        {
            self.position += 1;
        }
    }

    fn consume(&mut self, expected: u8) -> ThrowCompletionOr<()> {
        if self.peek() != Some(u32::from(expected)) {
            return json_malformed(self.vm);
        }
        self.position += 1;
        Ok(())
    }

    fn consume_literal(&mut self, literal: &[u8]) -> ThrowCompletionOr<()> {
        for byte in literal {
            self.consume(*byte)?;
        }
        Ok(())
    }

    fn parse_text(mut self) -> ThrowCompletionOr<Value> {
        self.skip_whitespace();
        let value = self.parse_value()?;
        self.skip_whitespace();
        if self.position != self.units.len() {
            return json_malformed(self.vm);
        }
        Ok(value)
    }

    fn parse_value(&mut self) -> ThrowCompletionOr<Value> {
        // NB: Deeply nested texts would otherwise run out of stack.
        if self.vm.did_reach_stack_space_limit() {
            return self
                .vm
                .throw_completion(ErrorKind::InternalError, ErrorType::CallStackSizeExceeded, &[]);
        }

        match self.peek() {
            Some(0x7B) => self.parse_object(),
            Some(0x5B) => self.parse_array(),
            Some(0x22) => {
                let token = self.scan_string()?;
                if !token.has_escapes {
                    return Ok(Value::from_string(C::make_primitive_string(
                        self.vm,
                        &self.units[token.start..token.end],
                    )));
                }
                Ok(Value::from_string(PrimitiveString::create(
                    self.vm,
                    self.make_string(&token)?,
                )))
            }
            Some(0x74) => {
                self.consume_literal(b"true")?;
                Ok(Value::TRUE)
            }
            Some(0x66) => {
                self.consume_literal(b"false")?;
                Ok(Value::FALSE)
            }
            Some(0x6E) => {
                self.consume_literal(b"null")?;
                Ok(Value::NULL)
            }
            Some(0x2D | 0x30..=0x39) => self.parse_number(),
            _ => json_malformed(self.vm),
        }
    }

    fn parse_object(&mut self) -> ThrowCompletionOr<Value> {
        self.position += 1;
        let first_key = self.tables.property_keys.len();
        let first_value = self.values.borrow().len();
        self.skip_whitespace();
        if self.peek() == Some(0x7D) {
            self.position += 1;
            return Ok(Value::from_object(Object::create(
                self.vm,
                self.realm,
                Some(self.realm.object_prototype()),
            )));
        }
        loop {
            if self.peek() != Some(0x22) {
                return json_malformed(self.vm);
            }
            let token = self.scan_string()?;
            let key = if token.has_escapes {
                PropertyKey::from(&self.make_string(&token)?)
            } else {
                self.tables.keys.key(&self.units[token.start..token.end])
            };
            self.skip_whitespace();
            self.consume(b':')?;
            self.skip_whitespace();
            let value = self.parse_value()?;
            self.tables.property_keys.push(key);
            self.values.borrow_mut().push(value);
            self.skip_whitespace();
            match self.peek() {
                Some(0x2C) => {
                    self.position += 1;
                    self.skip_whitespace();
                }
                Some(0x7D) => {
                    self.position += 1;
                    break;
                }
                _ => return json_malformed(self.vm),
            }
        }

        let object = self.create_object(first_key, first_value);
        self.tables.property_keys.truncate(first_key);
        self.values.borrow_mut().truncate(first_value);
        Ok(Value::from_object(object))
    }

    /// Creates the object of the properties parsed last, whose keys and values start at `first_key` and
    /// `first_value`.
    fn create_object(&mut self, first_key: usize, first_value: usize) -> Gc<Object> {
        let tables = &mut *self.tables;
        let keys = &tables.property_keys[first_key..];
        if let Some(shape) = tables.shapes.shape_for_keys(self.vm, self.realm, keys) {
            let object = Object::create_with_premade_shape(self.vm, shape);
            for (offset, value) in self.values.borrow()[first_value..].iter().enumerate() {
                object.put_direct(offset as u32, *value);
            }
            return object;
        }

        // NB: Keys that repeat or are array indices, and too many keys, take the generic path.
        let object = Object::create(self.vm, self.realm, Some(self.realm.object_prototype()));
        for (index, key) in keys.iter().enumerate() {
            let value = self.values.borrow()[first_value + index];
            object.define_direct_property(self.vm, key, value, DEFAULT_ATTRIBUTES);
        }
        object
    }

    fn parse_array(&mut self) -> ThrowCompletionOr<Value> {
        self.position += 1;
        let first_value = self.values.borrow().len();
        self.skip_whitespace();
        if self.peek() == Some(0x5D) {
            self.position += 1;
            return Ok(Value::from_object(Array::create(self.vm, self.realm, 0, None).must()));
        }
        loop {
            let value = self.parse_value()?;
            self.values.borrow_mut().push(value);
            self.skip_whitespace();
            match self.peek() {
                Some(0x2C) => {
                    self.position += 1;
                    self.skip_whitespace();
                }
                Some(0x5D) => {
                    self.position += 1;
                    break;
                }
                _ => return json_malformed(self.vm),
            }
        }

        // NB: The elements become the packed indexed storage of the array in one step.
        let array = Array::create(self.vm, self.realm, 0, None).must();
        array.set_indexed_property_elements(&self.values.borrow()[first_value..]);
        self.values.borrow_mut().truncate(first_value);
        Ok(Value::from_object(array))
    }

    /// Finds the end of the string whose opening quote is at the current position, and moves past its closing quote.
    fn scan_string(&mut self) -> ThrowCompletionOr<StringToken> {
        let start = self.position + 1;
        let mut position = start;
        let mut has_escapes = false;
        loop {
            let Some(unit) = self.units.get(position).map(|unit| (*unit).into()) else {
                return json_malformed(self.vm);
            };
            match unit {
                0x22 => break,
                0x5C => {
                    has_escapes = true;
                    position += 2;
                }
                0x00..=0x1F => return json_malformed(self.vm),
                _ => position = C::skip_ordinary_string_units(self.units, position + 1),
            }
        }
        self.position = position + 1;
        Ok(StringToken {
            start,
            end: position,
            has_escapes,
        })
    }

    fn make_string(&self, token: &StringToken) -> ThrowCompletionOr<Utf16String> {
        let units = &self.units[token.start..token.end];
        if !token.has_escapes {
            return Ok(C::make_string(units));
        }

        let mut code_units: Vec<u16> = Vec::with_capacity(units.len());
        let mut index = 0;
        while index < units.len() {
            let unit: u32 = units[index].into();
            index += 1;
            if unit != 0x5C {
                code_units.push(unit as u16);
                continue;
            }
            let Some(escaped) = units.get(index).map(|unit| (*unit).into()) else {
                return json_malformed(self.vm);
            };
            index += 1;
            let code_unit = match escaped {
                0x22 => 0x22,
                0x5C => 0x5C,
                0x2F => 0x2F,
                0x62 => 0x08,
                0x66 => 0x0C,
                0x6E => 0x0A,
                0x72 => 0x0D,
                0x74 => 0x09,
                0x75 => {
                    let Some(hex_digits) = units.get(index..index + 4) else {
                        return json_malformed(self.vm);
                    };
                    index += 4;
                    let mut code_unit: u32 = 0;
                    for digit in hex_digits {
                        let Some(value) = char::from_u32((*digit).into()).and_then(|digit| digit.to_digit(16)) else {
                            return json_malformed(self.vm);
                        };
                        code_unit = (code_unit << 4) | value;
                    }
                    code_unit
                }
                _ => return json_malformed(self.vm),
            };
            code_units.push(code_unit as u16);
        }
        Ok(Utf16String::from_utf16(&code_units))
    }

    fn parse_number(&mut self) -> ThrowCompletionOr<Value> {
        let start = self.position;
        let is_digit = |unit: Option<u32>| unit.is_some_and(|unit| (0x30..=0x39).contains(&unit));

        let is_negative = self.peek() == Some(0x2D);
        if is_negative {
            self.position += 1;
        }

        // Integer part: 0, or a digit other than 0 followed by digits.
        let integer_start = self.position;
        match self.peek() {
            Some(0x30) => self.position += 1,
            Some(0x31..=0x39) => {
                while is_digit(self.peek()) {
                    self.position += 1;
                }
            }
            _ => return json_malformed(self.vm),
        }
        let integer_end = self.position;

        let mut is_integer = true;
        if self.peek() == Some(0x2E) {
            is_integer = false;
            self.position += 1;
            if !is_digit(self.peek()) {
                return json_malformed(self.vm);
            }
            while is_digit(self.peek()) {
                self.position += 1;
            }
        }
        if matches!(self.peek(), Some(0x45 | 0x65)) {
            is_integer = false;
            self.position += 1;
            if matches!(self.peek(), Some(0x2B | 0x2D)) {
                self.position += 1;
            }
            if !is_digit(self.peek()) {
                return json_malformed(self.vm);
            }
            while is_digit(self.peek()) {
                self.position += 1;
            }
        }

        // OPTIMIZATION: An integer of up to nine digits is exactly its value, which needs no float parsing.
        if is_integer && integer_end - integer_start <= 9 {
            let mut value: i32 = 0;
            for unit in &self.units[integer_start..integer_end] {
                value = value * 10 + ((*unit).into() - 0x30) as i32;
            }
            if is_negative {
                if value == 0 {
                    return Ok(Value::from_f64(-0.0));
                }
                value = -value;
            }
            return Ok(Value::from_i32(value));
        }

        // NB: The units of a number are ASCII digits and signs, so each one is a byte of its literal.
        let units = &self.units[start..self.position];
        let mut buffer = [0u8; 64];
        let mut long_literal = Vec::new();
        let literal = match buffer.get_mut(..units.len()) {
            Some(bytes) => bytes,
            None => {
                long_literal.resize(units.len(), 0);
                &mut long_literal[..]
            }
        };
        for (byte, unit) in literal.iter_mut().zip(units) {
            *byte = (*unit).into() as u8;
        }
        let value: f64 = core::str::from_utf8(literal)
            .ok()
            .and_then(|literal| literal.parse().ok())
            .expect("a JSON number is a valid float literal");
        Ok(Value::from_f64(value))
    }
}
