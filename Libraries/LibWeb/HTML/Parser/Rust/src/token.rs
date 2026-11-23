/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::code_units::CodeUnits;
#[cfg(not(test))]
use ak::Utf16StringUnits;
use ak::Utf16View;
use std::cell::RefCell;

/// Source position in the input.
#[derive(Clone, Copy, Debug, Default)]
pub struct Position {
    pub line: u64,
    pub column: u64,
}

/// Where in the tokenizer's input something starts or ends, as an offset in code units, or nowhere at all.
///
/// The tokenizer only works out which line and column an offset is at when it is asked to.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct SourceOffset(usize);

impl SourceOffset {
    pub const NONE: Self = Self(usize::MAX);

    pub fn new(offset: usize) -> Self {
        debug_assert!(offset != usize::MAX);
        Self(offset)
    }

    pub fn get(self) -> Option<usize> {
        (self != Self::NONE).then_some(self.0)
    }
}

impl Default for SourceOffset {
    fn default() -> Self {
        Self::NONE
    }
}

/// A small number that stands for a known tag name, so that the tree builder can tell which of them a token has
/// without comparing names. Every other name has the id `UNKNOWN`.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct TagId(u16);

impl TagId {
    pub const UNKNOWN: Self = Self(0);

    pub const fn from_index(index: u16) -> Self {
        Self(index)
    }

    pub const fn index(self) -> usize {
        self.0 as usize
    }

    pub fn for_name(name: &str) -> Self {
        crate::known_names::tag_id_for_name(name.as_bytes())
    }

    pub fn for_code_units(name: Utf16View<'_>) -> Self {
        // NB: Every known tag name is ASCII, so a name with other code units in it is found to be none of them.
        match name {
            Utf16View::Latin1(name) => crate::known_names::tag_id_for_name(name),
            Utf16View::Utf16(_) => Self::UNKNOWN,
        }
    }
}

/// A set of known tag names.
pub struct TagSet([u64; crate::known_names::TAG_SET_WORD_COUNT]);

impl TagSet {
    pub const fn new(ids: &[TagId]) -> Self {
        let mut words = [0; crate::known_names::TAG_SET_WORD_COUNT];
        let mut i = 0;
        while i < ids.len() {
            let index = ids[i].0 as usize;
            assert!(index != 0, "a tag set can only have known tag names in it");
            words[index / 64] |= 1 << (index % 64);
            i += 1;
        }
        Self(words)
    }

    #[inline(always)]
    pub const fn contains(&self, id: TagId) -> bool {
        let index = id.0 as usize;
        (self.0[index / 64] >> (index % 64)) & 1 != 0
    }
}

/// A set of the known tag names it is given, worked out when the parser is compiled.
macro_rules! tag_set {
    ($($name:tt),+ $(,)?) => {
        const { &$crate::token::TagSet::new(&[$($crate::known_names::tag_id!($name)),+]) }
    };
}
pub(crate) use tag_set;

/// A single attribute on a start or end tag token.
///
#[derive(Clone, Debug, Default)]
pub struct Attribute {
    pub local_name: HtmlName,
    pub value: CodeUnits,
    pub name_start_offset: SourceOffset,
    pub name_end_offset: SourceOffset,
    pub value_start_offset: SourceOffset,
    pub value_end_offset: SourceOffset,
}

const MAX_SPARE_ATTRIBUTE_LISTS: usize = 4;
const MAX_SPARE_ATTRIBUTE_VALUES: usize = 64;
// Buffers grown past these sizes by an unusually large tag are freed rather than kept, so the spare
// set retains at most a few hundred KiB after the parser that filled it is gone.
const MAX_SPARE_ATTRIBUTE_LIST_CAPACITY: usize = 64;
const MAX_SPARE_ATTRIBUTE_VALUE_CAPACITY: usize = 4096;

thread_local! {
    // Dropped tag tokens leave their attribute lists and value buffers here for the next tag.
    static SPARE_ATTRIBUTE_LISTS: RefCell<Vec<Vec<Attribute>>> = const { RefCell::new(Vec::new()) };
    static SPARE_ATTRIBUTE_VALUES: RefCell<Vec<CodeUnits>> = const { RefCell::new(Vec::new()) };
}

/// The attributes of a tag token, backed by a spare buffer when there is one and the tag has attributes.
#[derive(Debug)]
pub struct AttributeList(Vec<Attribute>);

impl AttributeList {
    pub fn new() -> Self {
        Self(Vec::new())
    }

    /// Adds an empty attribute, with a spare value buffer when a dropped tag token left one.
    pub fn start_new_attribute(&mut self) {
        // NB: Most tags have no attributes, so a list only takes a spare buffer once it has an attribute to hold.
        if self.0.capacity() == 0 {
            self.0 = SPARE_ATTRIBUTE_LISTS.with_borrow_mut(Vec::pop).unwrap_or_default();
        }
        self.0.push(Attribute {
            value: SPARE_ATTRIBUTE_VALUES.with_borrow_mut(Vec::pop).unwrap_or_default(),
            ..Default::default()
        });
    }
}

impl Default for AttributeList {
    fn default() -> Self {
        Self::new()
    }
}

impl Clone for AttributeList {
    fn clone(&self) -> Self {
        Self(self.0.clone())
    }
}

impl std::ops::Deref for AttributeList {
    type Target = Vec<Attribute>;

    fn deref(&self) -> &Vec<Attribute> {
        &self.0
    }
}

impl std::ops::DerefMut for AttributeList {
    fn deref_mut(&mut self) -> &mut Vec<Attribute> {
        &mut self.0
    }
}

impl IntoIterator for AttributeList {
    type Item = Attribute;
    type IntoIter = std::vec::IntoIter<Attribute>;

    fn into_iter(mut self) -> Self::IntoIter {
        std::mem::take(&mut self.0).into_iter()
    }
}

impl<'a> IntoIterator for &'a AttributeList {
    type Item = &'a Attribute;
    type IntoIter = std::slice::Iter<'a, Attribute>;

    fn into_iter(self) -> Self::IntoIter {
        self.0.iter()
    }
}

impl Drop for AttributeList {
    fn drop(&mut self) {
        if self.0.capacity() == 0 {
            return;
        }
        SPARE_ATTRIBUTE_VALUES.with_borrow_mut(|spare_values| {
            for attribute in self.0.drain(..) {
                if spare_values.len() == MAX_SPARE_ATTRIBUTE_VALUES {
                    break;
                }
                let mut value = attribute.value;
                value.clear();
                let capacity = value.capacity();
                if capacity == 0 || capacity > MAX_SPARE_ATTRIBUTE_VALUE_CAPACITY {
                    continue;
                }
                spare_values.push(value);
            }
        });
        if self.0.capacity() > MAX_SPARE_ATTRIBUTE_LIST_CAPACITY {
            return;
        }
        SPARE_ATTRIBUTE_LISTS.with_borrow_mut(|spare_lists| {
            if spare_lists.len() < MAX_SPARE_ATTRIBUTE_LISTS {
                spare_lists.push(std::mem::take(&mut self.0));
            }
        });
    }
}

#[cfg(not(test))]
#[derive(Clone, Default, Eq)]
pub struct HtmlName(ak::Utf16FlyString);

// Standalone Rust test binaries have no C++ fly-string table to own these names.
#[cfg(test)]
#[derive(Clone, Default, Eq)]
pub struct HtmlName(String);

#[cfg(not(test))]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct KnownName(usize);

#[cfg(test)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum KnownName {
    Raw(usize),
    Literal(&'static str),
}

impl KnownName {
    pub const fn from_raw(raw: usize) -> Self {
        #[cfg(not(test))]
        {
            Self(raw)
        }
        #[cfg(test)]
        {
            Self::Raw(raw)
        }
    }

    #[cfg(test)]
    pub const fn from_literal(name: &'static str) -> Self {
        Self::Literal(name)
    }

    /// The identity of the C++ `Utf16FlyString` that holds this name.
    #[cfg(not(test))]
    pub const fn raw_identity(self) -> usize {
        self.0
    }
}

impl HtmlName {
    pub fn from_code_units(name: Utf16View<'_>) -> Self {
        #[cfg(not(test))]
        {
            match name.as_ascii() {
                // SAFETY: ASCII is valid UTF-8.
                Some(name) => Self(ak::Utf16FlyString::from_utf8(unsafe { str::from_utf8_unchecked(name) })),
                None => Self(ak::Utf16FlyString::from_utf16(&name.to_utf16())),
            }
        }
        #[cfg(test)]
        {
            Self(String::from_utf16_lossy(&name.to_utf16()))
        }
    }

    pub fn from_utf8(name: &str) -> Self {
        #[cfg(not(test))]
        {
            Self(ak::Utf16FlyString::from_utf8(name))
        }
        #[cfg(test)]
        {
            Self(name.to_string())
        }
    }

    /// Borrow an existing C++ `Utf16FlyString` identity.
    ///
    /// # Safety
    /// `raw` must identify a live `Utf16FlyString` for the duration of this call.
    pub unsafe fn from_borrowed_raw(raw: usize) -> Self {
        #[cfg(not(test))]
        {
            unsafe { ak::reference_utf16_string(raw) };
            Self(unsafe { ak::Utf16FlyString::from_raw_owned(raw) })
        }
        #[cfg(test)]
        {
            let _ = raw;
            unreachable!("C++ string identities are unavailable in standalone Rust tests")
        }
    }

    pub fn raw_identity(&self) -> usize {
        #[cfg(not(test))]
        {
            self.0.raw_identity()
        }
        #[cfg(test)]
        {
            0
        }
    }

    pub fn equals(&self, other: &str) -> bool {
        #[cfg(test)]
        return self.0 == other;

        #[cfg(not(test))]
        {
            if let Some(raw) = ak::utf16_short_string_raw(other) {
                return self.raw_identity() == raw;
            }
            match self.0.as_units() {
                Utf16StringUnits::Ascii(units) => units == other.as_bytes(),
                Utf16StringUnits::Latin1(units) => units.iter().map(|&unit| u16::from(unit)).eq(other.encode_utf16()),
                Utf16StringUnits::Utf16(units) => units.iter().copied().eq(other.encode_utf16()),
            }
        }
    }

    pub fn is_one_of(&self, names: &[KnownName]) -> bool {
        #[cfg(not(test))]
        {
            names.contains(&KnownName::from_raw(self.raw_identity()))
        }
        #[cfg(test)]
        {
            names.iter().any(|name| self == name)
        }
    }

    pub fn eq_ignore_ascii_case(&self, other: &Self) -> bool {
        #[cfg(test)]
        return self.0.eq_ignore_ascii_case(&other.0);

        #[cfg(not(test))]
        {
            match (self.0.as_units(), other.0.as_units()) {
                (
                    Utf16StringUnits::Ascii(left) | Utf16StringUnits::Latin1(left),
                    Utf16StringUnits::Ascii(right) | Utf16StringUnits::Latin1(right),
                ) => left.eq_ignore_ascii_case(right),
                (Utf16StringUnits::Utf16(left), Utf16StringUnits::Utf16(right)) => {
                    left.len() == right.len()
                        && left.iter().zip(right).all(|(left, right)| {
                            left == right
                                || (*left <= 0x7f
                                    && *right <= 0x7f
                                    && (*left as u8).eq_ignore_ascii_case(&(*right as u8)))
                        })
                }
                (Utf16StringUnits::Ascii(left) | Utf16StringUnits::Latin1(left), Utf16StringUnits::Utf16(right))
                | (Utf16StringUnits::Utf16(right), Utf16StringUnits::Ascii(left) | Utf16StringUnits::Latin1(left)) => {
                    left.len() == right.len()
                        && left.iter().zip(right).all(|(left, right)| {
                            u16::from(*left) == *right || (*right <= 0x7f && left.eq_ignore_ascii_case(&(*right as u8)))
                        })
                }
            }
        }
    }
}

impl PartialEq<KnownName> for HtmlName {
    fn eq(&self, other: &KnownName) -> bool {
        #[cfg(not(test))]
        {
            self.raw_identity() == other.0
        }
        #[cfg(test)]
        {
            match other {
                KnownName::Raw(raw) => self.raw_identity() == *raw,
                KnownName::Literal(name) => self.0 == *name,
            }
        }
    }
}

impl PartialEq<HtmlName> for KnownName {
    fn eq(&self, other: &HtmlName) -> bool {
        other == self
    }
}

impl PartialEq for HtmlName {
    fn eq(&self, other: &Self) -> bool {
        self.0 == other.0
    }
}

impl PartialEq<str> for HtmlName {
    fn eq(&self, other: &str) -> bool {
        self.equals(other)
    }
}

impl PartialEq<&str> for HtmlName {
    fn eq(&self, other: &&str) -> bool {
        self.equals(other)
    }
}

impl PartialEq<&HtmlName> for HtmlName {
    fn eq(&self, other: &&HtmlName) -> bool {
        self == *other
    }
}

impl std::fmt::Debug for HtmlName {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        #[cfg(not(test))]
        {
            formatter.debug_tuple("HtmlName").field(&self.0.raw_identity()).finish()
        }
        #[cfg(test)]
        {
            formatter.debug_tuple("HtmlName").field(&self.0).finish()
        }
    }
}

/// Data specific to DOCTYPE tokens.
#[derive(Clone, Debug, Default)]
pub struct DoctypeData {
    pub name: CodeUnits,
    pub public_identifier: CodeUnits,
    pub system_identifier: CodeUnits,
    pub missing_name: bool,
    pub missing_public_identifier: bool,
    pub missing_system_identifier: bool,
    pub force_quirks: bool,
}

/// The type of an HTML token.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum TokenType {
    #[default]
    Invalid = 0,
    Doctype = 1,
    StartTag = 2,
    EndTag = 3,
    Comment = 4,
    Character = 5,
    EndOfFile = 6,
}

/// Type-specific data for an HTML token.
///
#[derive(Clone, Debug, Default)]
pub enum TokenPayload {
    #[default]
    None,
    Tag {
        tag_name: HtmlName,
        tag_id: TagId,
        self_closing: bool,
        // AD-HOC: See AD-HOC comment on Element.m_had_duplicate_attribute_during_tokenization about why this is tracked.
        had_duplicate_attribute: bool,
        attributes: AttributeList,
    },
    Comment(CodeUnits),
    Doctype(Box<DoctypeData>),
}

/// An HTML token produced by the tokenizer.
#[derive(Clone, Debug, Default)]
pub struct Token {
    pub token_type: TokenType,
    pub code_point: u32,
    pub payload: TokenPayload,
    pub start_offset: SourceOffset,
    pub end_offset: SourceOffset,
}

impl Token {
    pub fn new_character(code_point: u32) -> Self {
        Token {
            token_type: TokenType::Character,
            code_point,
            ..Default::default()
        }
    }

    pub fn new_eof() -> Self {
        Token {
            token_type: TokenType::EndOfFile,
            ..Default::default()
        }
    }

    #[inline(always)]
    pub fn tag_name(&self) -> &HtmlName {
        match &self.payload {
            TokenPayload::Tag { tag_name, .. } => tag_name,
            _ => panic!("tag_name called on non-tag token"),
        }
    }

    #[inline(always)]
    pub fn tag_id(&self) -> TagId {
        match &self.payload {
            TokenPayload::Tag { tag_id, .. } => *tag_id,
            _ => panic!("tag_id called on non-tag token"),
        }
    }

    pub fn set_tag_name(&mut self, name: Utf16View<'_>) {
        match &mut self.payload {
            TokenPayload::Tag { tag_name, tag_id, .. } => {
                *tag_name = HtmlName::from_code_units(name);
                *tag_id = TagId::for_code_units(name);
            }
            _ => panic!("set_tag_name called on non-tag token"),
        }
    }

    #[inline(always)]
    pub fn set_self_closing(&mut self, value: bool) {
        match &mut self.payload {
            TokenPayload::Tag { self_closing, .. } => *self_closing = value,
            _ => panic!("set_self_closing called on non-tag token"),
        }
    }

    #[inline(always)]
    pub fn is_self_closing(&self) -> bool {
        match &self.payload {
            TokenPayload::Tag { self_closing, .. } => *self_closing,
            _ => false,
        }
    }

    #[inline(always)]
    pub fn had_duplicate_attribute(&self) -> bool {
        match &self.payload {
            TokenPayload::Tag {
                had_duplicate_attribute,
                ..
            } => *had_duplicate_attribute,
            _ => false,
        }
    }

    #[inline(always)]
    pub fn start_new_attribute(&mut self) {
        match &mut self.payload {
            TokenPayload::Tag { attributes, .. } => attributes.start_new_attribute(),
            _ => panic!("start_new_attribute called on non-tag token"),
        }
    }

    #[inline(always)]
    pub fn attributes_mut(&mut self) -> &mut Vec<Attribute> {
        match &mut self.payload {
            TokenPayload::Tag { attributes, .. } => &mut attributes.0,
            _ => panic!("attributes_mut called on non-tag token"),
        }
    }

    pub fn normalize_attributes(&mut self) {
        let TokenPayload::Tag {
            attributes,
            had_duplicate_attribute,
            ..
        } = &mut self.payload
        else {
            return;
        };

        let mut i = 0;
        while i < attributes.len() {
            let is_duplicate = (0..i).any(|seen_index| attributes[seen_index].local_name == attributes[i].local_name);
            if is_duplicate {
                *had_duplicate_attribute = true;
                attributes.remove(i);
            } else {
                i += 1;
            }
        }
    }

    #[inline(always)]
    pub fn set_comment_data(&mut self, data: CodeUnits) {
        match &mut self.payload {
            TokenPayload::Comment(s) => *s = data,
            _ => panic!("set_comment_data called on non-comment token"),
        }
    }

    #[inline(always)]
    pub fn doctype_data_mut(&mut self) -> &mut DoctypeData {
        match &mut self.payload {
            TokenPayload::Doctype(dd) => dd,
            _ => panic!("doctype_data_mut called on non-doctype token"),
        }
    }
}

#[cfg(test)]
mod spare_buffer_tests {
    use super::*;

    fn clear_spare_buffers() {
        SPARE_ATTRIBUTE_LISTS.with_borrow_mut(Vec::clear);
        SPARE_ATTRIBUTE_VALUES.with_borrow_mut(Vec::clear);
    }

    fn spare_value_capacities() -> Vec<usize> {
        SPARE_ATTRIBUTE_VALUES.with_borrow(|values| values.iter().map(CodeUnits::capacity).collect())
    }

    fn spare_list_capacities() -> Vec<usize> {
        SPARE_ATTRIBUTE_LISTS.with_borrow(|lists| lists.iter().map(Vec::capacity).collect())
    }

    fn attribute_with_value_capacity(capacity: usize) -> Attribute {
        Attribute {
            value: CodeUnits::Latin1(Vec::with_capacity(capacity)),
            ..Default::default()
        }
    }

    #[test]
    fn dropped_lists_keep_small_buffers_for_the_next_tag() {
        clear_spare_buffers();
        let mut list = AttributeList::new();
        list.push(attribute_with_value_capacity(16));
        list.push(attribute_with_value_capacity(MAX_SPARE_ATTRIBUTE_VALUE_CAPACITY));
        drop(list);
        assert_eq!(spare_value_capacities().len(), 2);
        assert_eq!(spare_list_capacities().len(), 1);
        let mut list = AttributeList::new();
        assert_eq!(list.capacity(), 0);
        assert_eq!(spare_list_capacities().len(), 1);
        list.start_new_attribute();
        assert!(list.capacity() >= 2);
        assert!(list[0].value.capacity() >= 16);
        assert!(spare_list_capacities().is_empty());
    }

    #[test]
    fn oversized_value_buffers_are_freed() {
        clear_spare_buffers();
        let mut list = AttributeList::new();
        list.push(attribute_with_value_capacity(MAX_SPARE_ATTRIBUTE_VALUE_CAPACITY + 1));
        list.push(attribute_with_value_capacity(1 << 20));
        drop(list);
        assert!(spare_value_capacities().is_empty());
    }

    #[test]
    fn oversized_lists_are_freed() {
        clear_spare_buffers();
        let mut list = AttributeList::new();
        list.reserve(MAX_SPARE_ATTRIBUTE_LIST_CAPACITY + 1);
        list.push(attribute_with_value_capacity(16));
        drop(list);
        assert!(spare_list_capacities().is_empty());
        assert_eq!(spare_value_capacities().len(), 1);
    }

    #[test]
    fn empty_value_buffers_are_not_retained() {
        clear_spare_buffers();
        let mut list = AttributeList::new();
        list.push(Attribute::default());
        drop(list);
        assert!(spare_value_capacities().is_empty());
    }
}
