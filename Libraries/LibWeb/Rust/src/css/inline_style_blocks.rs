/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The declaration blocks of a document's style attributes, by their text, so that elements that
//! write the same text hold one block.

use crate::css::css_tokenizer::TokenizerInput;
use crate::css::declaration_block::{DeclarationBlock, SharedDeclarationBlock};
use crate::css::ffi_support::{FfiUtf16View, ascii_lowercase};
use crate::css::style_value::RetainedString;
use std::collections::HashMap;
use std::hash::{BuildHasher, Hasher};

struct InlineStyleEntry {
    /// One byte per unit when the text is ASCII, as most style attributes are.
    text: RetainedString,
    quirks: bool,
    block: SharedDeclarationBlock,
}

/// A document's style attribute blocks, by their text and the quirks mode they were parsed in.
pub struct InlineStyleBlocks {
    /// The entries by the hash of their text and quirks mode, one per hash.
    blocks: HashMap<u64, InlineStyleEntry>,
    /// The entries whose hash an entry of `blocks` already has, which a 64-bit hash makes rare.
    collisions: Vec<InlineStyleEntry>,
    /// How many entries the cache holds before it drops those only it still holds: twice as many
    /// as the last drop kept, so dropping costs a constant per entry inserted.
    sweep_threshold: usize,
    /// Keyed at random, so that a page cannot make many texts fall into one bucket.
    hasher: std::collections::hash_map::RandomState,
}

/// How many entries the cache holds before it first drops those only it still holds.
const MINIMUM_SWEEP_THRESHOLD: usize = 256;

fn units(text: TokenizerInput<'_>) -> impl Iterator<Item = u16> + '_ {
    let (ascii, utf16): (&[u8], &[u16]) = match text {
        TokenizerInput::Latin1(bytes) => (bytes, &[]),
        TokenizerInput::Utf16(units) => (&[], units),
    };
    ascii.iter().map(|&byte| u16::from(byte)).chain(utf16.iter().copied())
}

/// Whether a block parsed from `text` may be shared by every element of the document that writes
/// it. A value that resolves a URL, reads an attribute or is random per element is not, and an
/// escape could spell the name of such a function.
fn is_cacheable(text: TokenizerInput<'_>) -> bool {
    const FUNCTIONS: [&[u8]; 5] = [b"url(", b"image-set(", b"src(", b"attr(", b"random("];
    let lowered: Vec<u16> = units(text).map(ascii_lowercase).collect();
    if lowered.contains(&u16::from(b'\\')) {
        return false;
    }
    !FUNCTIONS.iter().any(|function| {
        lowered.windows(function.len()).any(|window| {
            window
                .iter()
                .zip(function.iter())
                .all(|(&unit, &byte)| unit == u16::from(byte))
        })
    })
}

impl InlineStyleBlocks {
    pub(crate) fn new() -> Self {
        Self {
            blocks: HashMap::new(),
            collisions: Vec::new(),
            sweep_threshold: MINIMUM_SWEEP_THRESHOLD,
            hasher: std::collections::hash_map::RandomState::new(),
        }
    }

    fn hash(&self, text: TokenizerInput<'_>, quirks: bool) -> u64 {
        let mut hasher = self.hasher.build_hasher();
        hasher.write_u8(u8::from(quirks));
        for unit in units(text) {
            hasher.write_u16(unit);
        }
        hasher.finish()
    }

    fn entry_count(&self) -> usize {
        self.blocks.len() + self.collisions.len()
    }

    pub(crate) fn find(&self, text: TokenizerInput<'_>, quirks: bool) -> Option<SharedDeclarationBlock> {
        let matches = |entry: &&InlineStyleEntry| entry.quirks == quirks && units(entry.text.units()).eq(units(text));
        let entry = self.blocks.get(&self.hash(text, quirks))?;
        if matches(&entry) {
            return Some(entry.block.clone());
        }
        self.collisions.iter().find(matches).map(|entry| entry.block.clone())
    }

    pub(crate) fn insert(&mut self, text: TokenizerInput<'_>, quirks: bool, block: &SharedDeclarationBlock) {
        if !is_cacheable(text) || self.find(text, quirks).is_some() {
            return;
        }
        if self.entry_count() >= self.sweep_threshold {
            self.sweep();
        }
        let entry = InlineStyleEntry {
            text: RetainedString::from_text(text),
            quirks,
            block: block.clone(),
        };
        match self.blocks.entry(self.hash(text, quirks)) {
            std::collections::hash_map::Entry::Vacant(slot) => {
                slot.insert(entry);
            }
            std::collections::hash_map::Entry::Occupied(_) => self.collisions.push(entry),
        }
    }

    /// Drops the entries whose block no element holds any more.
    fn sweep(&mut self) {
        self.blocks.retain(|_, entry| !entry.block.is_unique());
        self.collisions.retain(|entry| !entry.block.is_unique());
        self.sweep_threshold = (2 * self.entry_count()).max(MINIMUM_SWEEP_THRESHOLD);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_inline_style_blocks_create() -> *mut InlineStyleBlocks {
    Box::into_raw(Box::new(InlineStyleBlocks::new()))
}

/// # Safety
/// `blocks` must come from `rust_inline_style_blocks_create` and not be used again.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_inline_style_blocks_destroy(blocks: *mut InlineStyleBlocks) {
    assert!(!blocks.is_null());
    drop(unsafe { Box::from_raw(blocks) });
}

/// A new declaration block holding the block of `text`, or null when no element wrote it.
#[unsafe(no_mangle)]
pub extern "C" fn rust_inline_style_blocks_find(
    blocks: &InlineStyleBlocks,
    text: FfiUtf16View,
    quirks: bool,
) -> *mut DeclarationBlock {
    let Some(text) = text.units() else {
        return std::ptr::null_mut();
    };
    blocks.find(text, quirks).map_or(std::ptr::null_mut(), |block| {
        Box::into_raw(Box::new(DeclarationBlock::new(block)))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_inline_style_blocks_insert(
    blocks: &mut InlineStyleBlocks,
    text: FfiUtf16View,
    quirks: bool,
    block: &DeclarationBlock,
) {
    if let Some(text) = text.units() {
        blocks.insert(text, quirks, &block.data());
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::parser::syntax_parser::parse_shared_block;
    use crate::css::parser::value_parser::ParseContext;
    use crate::css::style::pipeline::allocation_count::count_live_bytes;

    fn parse(text: &str) -> SharedDeclarationBlock {
        let units: Vec<u16> = text.encode_utf16().collect();
        let context = ParseContext::default();
        let parse = parse_shared_block(
            TokenizerInput::Utf16(&units),
            vec![crate::css::parser::syntax_parser::RuleContext::Style],
            Some(&context),
            false,
        );
        let block = unsafe {
            Box::from_raw(crate::css::parser::syntax_parser::rust_css_syntax_parse_declaration_block(&parse))
        };
        block.data()
    }

    fn ascii(text: &str) -> TokenizerInput<'_> {
        TokenizerInput::Latin1(text.as_bytes())
    }

    #[test]
    fn equal_texts_share_one_block() {
        let mut blocks = InlineStyleBlocks::new();
        let text = "color: red; margin-left: 3.5px";
        let block = parse(text);
        blocks.insert(ascii(text), false, &block);
        let utf16: Vec<u16> = text.encode_utf16().collect();
        let hit = blocks.find(TokenizerInput::Utf16(&utf16), false).unwrap();
        assert!(SharedDeclarationBlock::ptr_eq(&hit, &block));
        assert!(blocks.find(ascii(text), true).is_none());
        assert!(blocks.find(ascii("color: red"), false).is_none());
    }

    #[test]
    fn texts_whose_values_differ_per_element_are_not_kept() {
        let mut blocks = InlineStyleBlocks::new();
        for text in [
            "background: URL(a.png)",
            "background-image: image-set(\"a.png\" 1x)",
            "width: attr(data-w px)",
            "width: random(1px, 2px)",
            "c\\olor: red",
        ] {
            blocks.insert(ascii(text), false, &parse(text));
            assert!(blocks.find(ascii(text), false).is_none(), "{text}");
        }
    }

    #[test]
    fn a_sweep_drops_the_blocks_only_the_cache_holds() {
        let mut blocks = InlineStyleBlocks::new();
        let held = parse("order: 0");
        blocks.insert(ascii("order: 0"), false, &held);
        for index in 1..=MINIMUM_SWEEP_THRESHOLD {
            let text = format!("order: {index}");
            blocks.insert(ascii(&text), false, &parse(&text));
        }
        assert_eq!(blocks.entry_count(), 2);
        assert!(blocks.find(ascii("order: 0"), false).is_some());
        assert_eq!(blocks.sweep_threshold, MINIMUM_SWEEP_THRESHOLD);
    }

    #[test]
    fn a_hit_costs_the_element_its_declaration_block_handle() {
        const COUNT: usize = 500;
        let mut blocks = InlineStyleBlocks::new();
        let text = "color: red; margin-left: 3.5px";
        blocks.insert(ascii(text), false, &parse(text));
        let mut handles = Vec::with_capacity(COUNT);
        let ((), bytes) = count_live_bytes(|| {
            for _ in 0..COUNT {
                handles.push(Box::new(DeclarationBlock::new(
                    blocks.find(ascii(text), false).unwrap(),
                )));
            }
        });
        let per_element = bytes as f64 / COUNT as f64;
        eprintln!("inline style, text another element repeats {per_element:.1} B (ceiling 24; 8 B, cached)");
        assert!(per_element <= 24.0);
    }

    #[test]
    fn own_text_costs_its_block_handle_and_entry() {
        const COUNT: usize = 500;
        // NB: Pooled values in pairs no other text repeats, so that each text parses a block of its own.
        let texts: Vec<String> = (0..COUNT)
            .map(|index| format!("z-index: {}; order: {}", index % 100, index / 100))
            .collect();
        let mut blocks = InlineStyleBlocks::new();
        for text in &texts {
            parse(text);
        }
        let mut handles = Vec::with_capacity(COUNT);
        let ((), bytes) = count_live_bytes(|| {
            for text in &texts {
                let block = parse(text);
                blocks.insert(ascii(text), false, &block);
                handles.push(Box::new(DeclarationBlock::new(block)));
            }
        });
        let per_element = bytes as f64 / COUNT as f64;
        eprintln!("inline style, own text, 2 declarations {per_element:.1} B (ceiling 176; 16 + 16 B per declaration)");
        assert!(per_element <= 176.0);
    }
}
