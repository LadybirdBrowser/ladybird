/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The parts of simdjson's On-Demand API that JSONObject.cpp drives, with simdjson 4's acceptance rules. Stage 1
//! indexes the structural characters of the whole text, and the iterators then read values token by token,
//! validating only what the caller consumes. JSON.parse and JSON.rawJSON are observable through those rules (which
//! inputs are malformed, and where a value's source text ends), so they are reproduced here, scalar by scalar, rather
//! than replaced with a stricter parser.

use core::cell::Cell;

/// The bytes simdjson requires after the text, which it may read past the last token.
pub const SIMDJSON_PADDING: usize = 64;

/// The error codes of the operations here. JSONObject only distinguishes NUMBER_ERROR from the others.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ErrorCode {
    TapeError,
    NumberError,
    UnclosedString,
    UnescapedChars,
    Empty,
    IncorrectType,
    TrailingContent,
    IncompleteArrayOrObject,
}

pub type SimdjsonResult<T> = Result<T, ErrorCode>;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum JsonType {
    Array,
    Object,
    Number,
    String,
    Boolean,
    Null,
    Unknown,
}

/// jsoncharutils::is_structural_or_whitespace: JSON whitespace and the operators, which end a scalar.
fn is_structural_or_whitespace(character: u8) -> bool {
    matches!(
        character,
        b'\t' | b'\n' | b'\r' | b' ' | b',' | b':' | b'[' | b']' | b'{' | b'}'
    )
}

/// atomparsing::str4ncmp: whether the first four bytes differ from those of the atom.
fn str4ncmp(json: &[u8], atom: &[u8]) -> bool {
    json[..4] != atom[..4]
}

/// Stage 1: the offsets of the structural characters and of the first byte of every scalar outside a string, as
/// simdjson's structural indexer finds them.
fn find_structural_indexes(buffer: &[u8], length: usize) -> SimdjsonResult<Vec<u32>> {
    // NB: Like simdjson, make room for a structural character at every byte up front, plus the padding entries.
    let mut structural_indexes = Vec::with_capacity(length + 3);
    let mut in_string = false;
    let mut is_escaped = false;
    let mut follows_nonquote_scalar = false;
    let mut unescaped_chars_error = false;

    let text = &buffer[..length];
    let mut index = 0;
    while index < length {
        let character = text[index];

        // A backslash escapes the byte after it, inside strings or not, unless it is escaped itself.
        let escaped = is_escaped;
        is_escaped = character == b'\\' && !escaped;
        let is_quote = character == b'"' && !escaped;

        // A string runs from its opening quote up to, but not including, its closing quote.
        if is_quote {
            in_string = !in_string;
        }
        let is_in_string_tail = in_string != is_quote;
        if in_string && character <= 0x1F {
            unescaped_chars_error = true;
        }

        let is_whitespace = matches!(character, b' ' | b'\t' | b'\n' | b'\r');
        let is_operator = matches!(character, b'{' | b'}' | b'[' | b']' | b',' | b':');
        let is_scalar = !is_whitespace && !is_operator;
        let is_potential_scalar_start = is_scalar && !follows_nonquote_scalar;
        if (is_operator || is_potential_scalar_start) && !is_in_string_tail {
            structural_indexes.push(index as u32);
        }
        follows_nonquote_scalar = is_scalar && !is_quote;
        index += 1;

        // NB: Inside a string, a byte other than a quote, a backslash or a control character changes nothing above:
        //     it is not structural, escapes nothing and is no error, and the closing quote decides what follows the
        //     string. So the loop skips such bytes without tracking that state for each of them.
        if in_string && !is_escaped {
            index += offset_of_first_string_special_byte(&text[index..]);
        }
    }

    if in_string {
        return Err(ErrorCode::UnclosedString);
    }
    if unescaped_chars_error {
        return Err(ErrorCode::UnescapedChars);
    }
    if structural_indexes.is_empty() {
        return Err(ErrorCode::Empty);
    }

    // We pad beyond.
    structural_indexes.push(length as u32);
    structural_indexes.push(length as u32);
    structural_indexes.push(0);
    Ok(structural_indexes)
}

/// The offset of the first quote, backslash or control character in `text`, or its length if there is none. Eight
/// bytes are checked at a time: in each word, a byte's high bit is set in the mask if the byte is one of those, with
/// false positives only above the first true one, which the lowest set bit ignores.
fn offset_of_first_string_special_byte(text: &[u8]) -> usize {
    const ONES: u64 = u64::from_ne_bytes([0x01; 8]);
    const HIGH_BITS: u64 = u64::from_ne_bytes([0x80; 8]);
    let has_zero_byte = |word: u64| word.wrapping_sub(ONES) & !word & HIGH_BITS;

    let mut offset = 0;
    while offset + 8 <= text.len() {
        let word = u64::from_le_bytes(text[offset..offset + 8].try_into().expect("the chunk has eight bytes"));
        let quotes = has_zero_byte(word ^ u64::from_ne_bytes([b'"'; 8]));
        let backslashes = has_zero_byte(word ^ u64::from_ne_bytes([b'\\'; 8]));
        let control_characters = word.wrapping_sub(u64::from_ne_bytes([0x20; 8])) & !word & HIGH_BITS;
        let special_bytes = quotes | backslashes | control_characters;
        if special_bytes != 0 {
            return offset + (special_bytes.trailing_zeros() / 8) as usize;
        }
        offset += 8;
    }
    offset
        + text[offset..]
            .iter()
            .position(|&character| character == b'"' || character == b'\\' || character <= 0x1F)
            .unwrap_or(text.len() - offset)
}

/// The json_iterator of a document: the position of the next token and the depth of the value being read.
pub struct JsonIterator<'a> {
    buffer: &'a [u8],
    structural_indexes: Vec<u32>,
    n_structural_indexes: usize,
    position: Cell<usize>,
    depth: Cell<u32>,
    error: Cell<Option<ErrorCode>>,
}

impl JsonIterator<'_> {
    fn index_at(&self, position: usize) -> usize {
        self.structural_indexes.get(position).copied().unwrap_or(0) as usize
    }

    fn peek_at(&self, position: usize) -> usize {
        self.index_at(position)
    }

    fn character_at(&self, offset: usize) -> u8 {
        self.buffer.get(offset).copied().unwrap_or(0)
    }

    fn peek(&self) -> u8 {
        self.character_at(self.peek_at(self.position.get()))
    }

    fn peek_last(&self) -> u8 {
        self.character_at(self.peek_at(self.n_structural_indexes - 1))
    }

    fn return_current_and_advance(&self) -> usize {
        let position = self.position.get();
        self.position.set(position + 1);
        self.peek_at(position)
    }

    fn peek_length(&self, position: usize) -> usize {
        (self.index_at(position + 1) as u32).wrapping_sub(self.index_at(position) as u32) as usize
    }

    fn peek_root_length(&self, position: usize) -> usize {
        let to_next = (self.index_at(position + 1) as u32).wrapping_sub(self.index_at(position) as u32);
        let to_second_next = (self.index_at(position + 2) as u32).wrapping_sub(self.index_at(position) as u32);
        (if to_second_next > to_next {
            to_next
        } else {
            to_second_next
        }) as usize
    }

    fn end_position(&self) -> usize {
        self.n_structural_indexes
    }

    pub fn at_end(&self) -> bool {
        self.position.get() == self.end_position()
    }

    fn is_single_token(&self) -> bool {
        self.n_structural_indexes == 1
    }

    fn ascend_to(&self, parent_depth: u32) {
        self.depth.set(parent_depth);
    }

    fn descend_to(&self, child_depth: u32) {
        self.depth.set(child_depth);
    }

    fn report_error(&self, error: ErrorCode) -> ErrorCode {
        self.error.set(Some(error));
        error
    }

    fn abandon(&self) {
        self.position.set(self.end_position());
        self.depth.set(0);
    }

    fn skip_child(&self, parent_depth: u32) -> SimdjsonResult<()> {
        if self.depth.get() <= parent_depth {
            return Ok(());
        }
        match self.character_at(self.return_current_and_advance()) {
            // For the first open array/object in a value, we've already incremented depth, so keep it the same
            // We never stop at colon, but if we did, it wouldn't affect depth
            b'[' | b'{' | b':' | b',' => {}
            // ] or } means we just finished a value and need to jump out of the array/object
            b']' | b'}' => {
                self.depth.set(self.depth.get() - 1);
                if self.depth.get() <= parent_depth {
                    return Ok(());
                }
            }
            b'"' if self.peek() == b':' => {
                // We are at a key!!!
                // This might happen if you just started an object and you skip it immediately.
                self.return_current_and_advance();
            }
            // Anything else must be a scalar value
            _ => {
                // For the first scalar, we will have incremented depth already, so we decrement it here.
                self.depth.set(self.depth.get() - 1);
                if self.depth.get() <= parent_depth {
                    return Ok(());
                }
            }
        }

        // Now that we've considered the first value, we only increment/decrement for arrays/objects
        while self.position.get() < self.end_position() {
            match self.character_at(self.return_current_and_advance()) {
                b'[' | b'{' => self.depth.set(self.depth.get() + 1),
                b']' | b'}' => {
                    self.depth.set(self.depth.get() - 1);
                    if self.depth.get() <= parent_depth {
                        return Ok(());
                    }
                }
                _ => {}
            }
        }

        Err(self.report_error(ErrorCode::TapeError))
    }
}

/// parser::iterate(): runs stage 1 over the first `length` bytes of `buffer`, which has SIMDJSON_PADDING more bytes
/// after them, and returns the document to iterate.
pub fn iterate(buffer: &[u8], length: usize) -> SimdjsonResult<Document<'_>> {
    assert!(buffer.len() >= length + SIMDJSON_PADDING);
    // NB: simdjson skips a UTF-8 byte order mark at the start of the text.
    let (buffer, length) = if buffer[..length].starts_with(b"\xEF\xBB\xBF") {
        (&buffer[3..], length - 3)
    } else {
        (buffer, length)
    };
    let structural_indexes = find_structural_indexes(buffer, length)?;
    let n_structural_indexes = structural_indexes.len() - 3;
    Ok(Document {
        iter: JsonIterator {
            buffer,
            structural_indexes,
            n_structural_indexes,
            position: Cell::new(0),
            depth: Cell::new(1),
            error: Cell::new(None),
        },
    })
}

/// A raw_json_string: the offset of the first byte after a string's opening quote.
#[derive(Clone, Copy, Debug)]
pub struct RawJsonString {
    pub raw: usize,
}

/// value_iterator: a value at a token position and depth of a document.
#[derive(Clone, Copy)]
pub struct ValueIterator<'i, 'a> {
    json_iter: &'i JsonIterator<'a>,
    depth: u32,
    start_position: usize,
}

impl<'i, 'a> ValueIterator<'i, 'a> {
    fn is_at_start(&self) -> bool {
        self.json_iter.position.get() == self.start_position
    }

    fn peek_start(&self) -> usize {
        self.json_iter.peek_at(self.start_position)
    }

    fn peek_scalar(&self) -> usize {
        if !self.is_at_start() {
            return self.peek_start();
        }
        self.json_iter.peek_at(self.json_iter.position.get())
    }

    fn advance_scalar(&self) {
        if !self.is_at_start() {
            return;
        }
        self.json_iter.return_current_and_advance();
        self.json_iter.ascend_to(self.depth - 1);
    }

    fn character(&self, offset: usize) -> u8 {
        self.json_iter.character_at(offset)
    }

    fn incorrect_type_error(&self) -> ErrorCode {
        ErrorCode::IncorrectType
    }

    pub fn type_(&self) -> JsonType {
        match self.character(self.peek_start()) {
            b'{' => JsonType::Object,
            b'[' => JsonType::Array,
            b'"' => JsonType::String,
            b'n' => JsonType::Null,
            b't' | b'f' => JsonType::Boolean,
            b'-' | b'0'..=b'9' => JsonType::Number,
            _ => JsonType::Unknown,
        }
    }

    /// The bytes from the value's first token to the next one, as value::raw_json_token() returns them.
    pub fn raw_json_token(&self) -> &'a [u8] {
        let start = self.peek_start();
        &self.json_iter.buffer[start..start + self.json_iter.peek_length(self.start_position)]
    }

    fn root_raw_json_token(&self) -> &'a [u8] {
        let start = self.peek_start();
        &self.json_iter.buffer[start..start + self.json_iter.peek_root_length(self.start_position)]
    }

    pub fn get_double(&self) -> SimdjsonResult<f64> {
        let result = parse_double(&self.json_iter.buffer[self.peek_scalar()..]);
        if result.is_ok() {
            self.advance_scalar();
        }
        result
    }

    pub fn get_bool(&self) -> SimdjsonResult<bool> {
        let json = &self.json_iter.buffer[self.peek_scalar()..];
        let not_true = str4ncmp(json, b"true");
        let not_false = str4ncmp(json, b"fals") || json[4] != b'e';
        let error = (not_true && not_false) || !is_structural_or_whitespace(json[if not_true { 5 } else { 4 }]);
        if error {
            return Err(self.incorrect_type_error());
        }
        self.advance_scalar();
        Ok(!not_true)
    }

    pub fn is_null(&self) -> SimdjsonResult<bool> {
        let json = &self.json_iter.buffer[self.peek_scalar()..];
        let is_null_string = !str4ncmp(json, b"null") && is_structural_or_whitespace(json[4]);
        // if we start with 'n', we must be a null
        if !is_null_string && json[0] == b'n' {
            return Err(self.incorrect_type_error());
        }
        if is_null_string {
            self.advance_scalar();
        }
        Ok(is_null_string)
    }

    pub fn get_raw_json_string(&self) -> SimdjsonResult<RawJsonString> {
        let json = self.peek_scalar();
        if self.character(json) != b'"' {
            return Err(self.incorrect_type_error());
        }
        self.advance_scalar();
        Ok(RawJsonString { raw: json + 1 })
    }

    fn start_container(&self, start_character: u8) -> SimdjsonResult<()> {
        let json = if !self.is_at_start() {
            self.peek_start()
        } else {
            self.json_iter.return_current_and_advance()
        };
        if self.character(json) != start_character {
            return Err(self.incorrect_type_error());
        }
        Ok(())
    }

    fn end_container(&self) {
        self.json_iter.ascend_to(self.depth - 1);
    }

    fn started_array(&self) {
        if self.json_iter.peek() == b']' {
            self.json_iter.return_current_and_advance();
            self.end_container();
            return;
        }
        self.json_iter.descend_to(self.depth + 1);
    }

    fn started_object(&self) {
        if self.json_iter.peek() == b'}' {
            self.json_iter.return_current_and_advance();
            self.end_container();
        }
    }

    pub fn get_array(&self) -> SimdjsonResult<Array<'i, 'a>> {
        self.start_container(b'[')?;
        self.started_array();
        Ok(Array { iter: *self })
    }

    pub fn get_object(&self) -> SimdjsonResult<Object<'i, 'a>> {
        self.start_container(b'{')?;
        self.started_object();
        Ok(Object { iter: *self })
    }

    fn is_open(&self) -> bool {
        self.json_iter.depth.get() >= self.depth
    }

    fn child(&self) -> ValueIterator<'i, 'a> {
        ValueIterator {
            json_iter: self.json_iter,
            depth: self.depth + 1,
            start_position: self.json_iter.position.get(),
        }
    }

    fn error(&self) -> Option<ErrorCode> {
        self.json_iter.error.get()
    }

    fn has_next_element(&self) -> SimdjsonResult<bool> {
        match self.character(self.json_iter.return_current_and_advance()) {
            b']' => {
                self.json_iter.ascend_to(self.depth - 1);
                Ok(false)
            }
            b',' => {
                self.json_iter.descend_to(self.depth + 1);
                Ok(true)
            }
            _ => Err(self.json_iter.report_error(ErrorCode::TapeError)),
        }
    }

    fn has_next_field(&self) -> SimdjsonResult<bool> {
        match self.character(self.json_iter.return_current_and_advance()) {
            b'}' => {
                self.end_container();
                Ok(false)
            }
            b',' => Ok(true),
            _ => Err(self.json_iter.report_error(ErrorCode::TapeError)),
        }
    }

    fn field_key(&self) -> SimdjsonResult<RawJsonString> {
        let key = self.json_iter.return_current_and_advance();
        if self.character(key) != b'"' {
            return Err(self.json_iter.report_error(ErrorCode::TapeError));
        }
        Ok(RawJsonString { raw: key + 1 })
    }

    fn field_value(&self) -> SimdjsonResult<()> {
        if self.character(self.json_iter.return_current_and_advance()) != b':' {
            return Err(self.json_iter.report_error(ErrorCode::TapeError));
        }
        self.json_iter.descend_to(self.depth + 1);
        Ok(())
    }
}

/// An array being iterated, whose begin() starts at its first element.
#[derive(Clone, Copy)]
pub struct Array<'i, 'a> {
    iter: ValueIterator<'i, 'a>,
}

impl<'i, 'a> Array<'i, 'a> {
    /// Whether there is an element to visit: array_iterator's operator!=.
    pub fn is_open(&self) -> bool {
        self.iter.is_open()
    }

    /// The element at the current position: array_iterator's operator*.
    pub fn current(&self) -> SimdjsonResult<ValueIterator<'i, 'a>> {
        if let Some(error) = self.iter.error() {
            self.iter.json_iter.abandon();
            return Err(error);
        }
        Ok(self.iter.child())
    }

    /// Moves past the current element: array_iterator's operator++, which leaves any error for current() to return.
    pub fn advance(&self) {
        if self.iter.error().is_some() {
            return;
        }
        if self.iter.json_iter.skip_child(self.iter.depth).is_err() {
            return;
        }
        let _ = self.iter.has_next_element();
    }
}

/// A field of an object: its key and its value.
pub struct Field<'i, 'a> {
    key: RawJsonString,
    value: ValueIterator<'i, 'a>,
}

impl<'i, 'a> Field<'i, 'a> {
    /// The key's bytes, with escapes, as field::escaped_key() finds them: up to the last quote before the colon.
    pub fn escaped_key(&self) -> &'a [u8] {
        let json_iter = self.value.json_iter;
        let mut end_quote = json_iter.peek_at(json_iter.position.get() - 1);
        while json_iter.character_at(end_quote) != b'"' {
            end_quote -= 1;
        }
        &json_iter.buffer[self.key.raw..end_quote]
    }

    pub fn value(&self) -> ValueIterator<'i, 'a> {
        self.value
    }
}

/// An object being iterated, whose begin() starts at its first field.
#[derive(Clone, Copy)]
pub struct Object<'i, 'a> {
    iter: ValueIterator<'i, 'a>,
}

impl<'i, 'a> Object<'i, 'a> {
    /// Whether there is a field to visit: object_iterator's operator!=.
    pub fn is_open(&self) -> bool {
        self.iter.is_open()
    }

    /// The field at the current position: object_iterator's operator*.
    pub fn current(&self) -> SimdjsonResult<Field<'i, 'a>> {
        if let Some(error) = self.iter.error() {
            self.iter.json_iter.abandon();
            return Err(error);
        }
        let result = self.iter.field_key().and_then(|key| {
            self.iter.field_value()?;
            Ok(Field {
                key,
                value: self.iter.child(),
            })
        });
        if result.is_err() {
            self.iter.json_iter.abandon();
        }
        result
    }

    /// Moves past the current field: object_iterator's operator++, which leaves any error for current() to return.
    pub fn advance(&self) {
        if self.iter.error().is_some() {
            return;
        }
        if self.iter.json_iter.skip_child(self.iter.depth).is_err() {
            return;
        }
        let _ = self.iter.has_next_field();
    }
}

/// A document: the root value of a text that passed stage 1.
pub struct Document<'a> {
    iter: JsonIterator<'a>,
}

impl<'a> Document<'a> {
    fn get_root_value_iterator(&self) -> ValueIterator<'_, 'a> {
        ValueIterator {
            json_iter: &self.iter,
            depth: 1,
            start_position: 0,
        }
    }

    pub fn type_(&self) -> JsonType {
        self.get_root_value_iterator().type_()
    }

    /// The bytes from the root token to the next one.
    pub fn raw_json_token(&self) -> &'a [u8] {
        self.get_root_value_iterator().root_raw_json_token()
    }

    pub fn at_end(&self) -> bool {
        self.iter.at_end()
    }

    /// value_iterator::copy_to_buffer() into a buffer of `capacity` bytes, padded with spaces, for the root scalars.
    fn copy_root_scalar(&self, capacity: usize) -> Option<Vec<u8>> {
        let token = self.raw_json_token();
        if capacity < token.len() || capacity == 0 {
            return None;
        }
        let mut buffer = vec![b' '; capacity + 1];
        buffer[..token.len()].copy_from_slice(token);
        buffer[capacity] = 0;
        Some(buffer)
    }

    pub fn is_null(&self) -> SimdjsonResult<bool> {
        let iter = self.get_root_value_iterator();
        let max_len = self.iter.peek_root_length(0);
        let json = &self.iter.buffer[iter.peek_scalar()..];
        let result = max_len >= 4 && !str4ncmp(json, b"null") && (max_len == 4 || is_structural_or_whitespace(json[4]));
        if result {
            // we have something that looks like a null.
            if !self.iter.is_single_token() {
                return Err(ErrorCode::TrailingContent);
            }
            iter.advance_scalar();
        } else if json[0] == b'n' {
            return Err(iter.incorrect_type_error());
        }
        Ok(result)
    }

    pub fn get_bool(&self) -> SimdjsonResult<bool> {
        let iter = self.get_root_value_iterator();
        let max_len = self.iter.peek_root_length(0);
        let json = &self.iter.buffer[iter.peek_scalar()..];
        // We have a boolean if we have either "true" or "false" and the next character is either
        // a structural character or whitespace. We also check that the length is correct:
        // "true" and "false" are 4 and 5 characters long, respectively.
        let value_true =
            max_len >= 4 && !str4ncmp(json, b"true") && (max_len == 4 || is_structural_or_whitespace(json[4]));
        let value_false =
            max_len >= 5 && !str4ncmp(json, b"false") && (max_len == 5 || is_structural_or_whitespace(json[5]));
        if !value_true && !value_false {
            return Err(iter.incorrect_type_error());
        }
        if !self.iter.is_single_token() {
            return Err(ErrorCode::TrailingContent);
        }
        iter.advance_scalar();
        Ok(value_true)
    }

    pub fn get_double(&self) -> SimdjsonResult<f64> {
        // Per https://www.exploringbinary.com/maximum-number-of-decimal-digits-in-binary-floating-point-numbers/,
        // 1074 is the maximum number of significant fractional digits. Add 8 more digits for the biggest
        // number: -0.<fraction>e-308.
        let Some(buffer) = self.copy_root_scalar(1074 + 8 + 1) else {
            return Err(ErrorCode::NumberError);
        };
        let result = parse_double(&buffer)?;
        if !self.iter.is_single_token() {
            return Err(ErrorCode::TrailingContent);
        }
        self.get_root_value_iterator().advance_scalar();
        Ok(result)
    }

    pub fn get_raw_json_string(&self) -> SimdjsonResult<RawJsonString> {
        let iter = self.get_root_value_iterator();
        let json = iter.peek_scalar();
        if iter.character(json) != b'"' {
            return Err(iter.incorrect_type_error());
        }
        if !self.iter.is_single_token() {
            return Err(ErrorCode::TrailingContent);
        }
        iter.advance_scalar();
        Ok(RawJsonString { raw: json + 1 })
    }

    pub fn get_array(&self) -> SimdjsonResult<Array<'_, 'a>> {
        let iter = self.get_root_value_iterator();
        iter.start_container(b'[')?;
        // The following lines do not fully protect against garbage content within the
        // array: e.g., `[1, 2] foo]`. Users concerned with garbage content should
        // also call `at_end()` on the document instance at the end of the processing to
        // ensure that the processing has finished at the end.
        if self.iter.peek_last() != b']' {
            self.iter.abandon();
            return Err(self.iter.report_error(ErrorCode::IncompleteArrayOrObject));
        }
        // NB: simdjson also checks that the document is balanced when the first padding byte is ']', which ours never
        //     is.
        iter.started_array();
        Ok(Array { iter })
    }

    pub fn get_object(&self) -> SimdjsonResult<Object<'_, 'a>> {
        let iter = self.get_root_value_iterator();
        iter.start_container(b'{')?;
        if self.iter.peek_last() != b'}' {
            self.iter.abandon();
            return Err(self.iter.report_error(ErrorCode::IncompleteArrayOrObject));
        }
        iter.started_object();
        Ok(Object { iter })
    }
}

/// numberparsing::parse_double(): a JSON number followed by whitespace or an operator, as the closest double. Numbers
/// that are infinite as doubles are errors.
fn parse_double(src: &[u8]) -> SimdjsonResult<f64> {
    let at = |index: usize| src.get(index).copied().unwrap_or(0);

    // Check for minus sign
    let negative = at(0) == b'-';
    let start = usize::from(negative);

    // Parse the integer part.
    let mut p = start;
    let leading_zero = at(p) == b'0';
    while at(p).is_ascii_digit() {
        p += 1;
    }
    // no integer digits, or 0123 (zero must be solo)
    if p == start {
        return Err(ErrorCode::IncorrectType);
    }
    if leading_zero && p != start + 1 {
        return Err(ErrorCode::NumberError);
    }

    // Parse the decimal part.
    if at(p) == b'.' {
        p += 1;
        // no decimal digits
        if !at(p).is_ascii_digit() {
            return Err(ErrorCode::NumberError);
        }
        while at(p).is_ascii_digit() {
            p += 1;
        }
    }

    // Parse the exponent
    if at(p) == b'e' || at(p) == b'E' {
        p += 1;
        if at(p) == b'-' || at(p) == b'+' {
            p += 1;
        }
        let start_exponent_digits = p;
        while at(p).is_ascii_digit() {
            p += 1;
        }
        // no exp digits, or 20+ exp digits
        if p == start_exponent_digits || p - start_exponent_digits > 19 {
            return Err(ErrorCode::NumberError);
        }
    }

    if !is_structural_or_whitespace(at(p)) {
        return Err(ErrorCode::NumberError);
    }

    // simdjson rounds to nearest, as Rust's parser does, and refuses infinite values.
    let text = core::str::from_utf8(&src[..p]).expect("a JSON number is ASCII");
    let value: f64 = text.parse().expect("a JSON number is a Rust float literal");
    if value.is_infinite() {
        return Err(ErrorCode::NumberError);
    }
    Ok(value)
}
