/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Named character reference matching using a DAFSA (Deterministic Acyclic Finite
//! State Automaton) with minimal perfect hashing. The DAFSA data is generated at
//! build time by build.rs from Entities.json.

include!(concat!(env!("OUT_DIR"), "/named_character_references.rs"));

fn ascii_alphabetic_to_index(c: u8) -> u8 {
    if c <= b'Z' { c - b'A' } else { c - b'a' + 26 }
}

#[derive(Clone, Copy)]
enum SearchState {
    Init,
    FirstToSecondLayer { mask: u64, offset: u16 },
    DafsaChildren { start_index: u16, len: u8 },
}

/// Incremental matcher for named character references using the DAFSA.
///
/// Feed characters one at a time via `try_consume_code_point()`. After each
/// rejected character (returns false), call `code_points()` to get the longest
/// match found so far, and `overconsumed_code_points()` to know how many
/// characters were consumed past the longest match.
pub struct NamedCharacterReferenceMatcher {
    search_state: SearchState,
    last_matched_unique_index: u16,
    pending_unique_index: u16,
    overconsumed_code_points: u8,
    ends_with_semicolon: bool,
}

impl NamedCharacterReferenceMatcher {
    pub fn new() -> Self {
        Self {
            search_state: SearchState::Init,
            last_matched_unique_index: 0,
            pending_unique_index: 0,
            overconsumed_code_points: 0,
            ends_with_semicolon: false,
        }
    }

    /// Feed one code point to the matcher. Returns true if the character was
    /// consumed (there may still be longer matches), false if no further
    /// matches are possible.
    pub fn try_consume_code_point(&mut self, c: u32) -> bool {
        if c > 0x7F {
            return false;
        }
        self.try_consume_ascii_char(c as u8)
    }

    fn try_consume_ascii_char(&mut self, c: u8) -> bool {
        match self.search_state {
            SearchState::Init => {
                if !c.is_ascii_alphabetic() {
                    return false;
                }
                let index = ascii_alphabetic_to_index(c) as usize;
                self.search_state = SearchState::FirstToSecondLayer {
                    mask: FIRST_TO_SECOND_LAYER[index].0,
                    offset: FIRST_TO_SECOND_LAYER[index].1,
                };
                self.pending_unique_index = FIRST_LAYER[index];
                self.overconsumed_code_points += 1;
                true
            }
            SearchState::FirstToSecondLayer { mask, offset } => {
                if !c.is_ascii_alphabetic() {
                    return false;
                }
                let bit_index = ascii_alphabetic_to_index(c);
                if ((1u64 << bit_index) & mask) == 0 {
                    return false;
                }

                // Count set bits below bit_index to find the node's position.
                let lower_mask = (1u64 << bit_index) - 1;
                let char_index = (mask & lower_mask).count_ones() as u16;
                let node = &SECOND_LAYER[(offset + char_index) as usize];

                self.pending_unique_index += node.number as u16;
                self.overconsumed_code_points += 1;
                if node.end_of_word {
                    self.pending_unique_index += 1;
                    self.last_matched_unique_index = self.pending_unique_index;
                    self.ends_with_semicolon = c == b';';
                    self.overconsumed_code_points = 0;
                }
                self.search_state = SearchState::DafsaChildren {
                    start_index: node.child_index,
                    len: node.children_len,
                };
                true
            }
            SearchState::DafsaChildren { start_index, len } => {
                for i in 0..len as u16 {
                    let node = &DAFSA_NODES[(start_index + i) as usize];
                    if node.character == c {
                        self.pending_unique_index += node.number as u16;
                        self.overconsumed_code_points += 1;
                        if node.end_of_word {
                            self.pending_unique_index += 1;
                            self.last_matched_unique_index = self.pending_unique_index;
                            self.ends_with_semicolon = c == b';';
                            self.overconsumed_code_points = 0;
                        }
                        self.search_state = SearchState::DafsaChildren {
                            start_index: node.child_index,
                            len: node.children_len,
                        };
                        return true;
                    }
                }
                false
            }
        }
    }

    /// Returns the codepoints for the longest match found, or None if no match.
    /// Returns (first_codepoint, second_codepoint). second is 0 if there is no second codepoint.
    pub fn code_points(&self) -> Option<(u32, u32)> {
        if self.last_matched_unique_index == 0 {
            return None;
        }
        let entry = &CODEPOINTS_LOOKUP[(self.last_matched_unique_index - 1) as usize];
        Some((entry.0, entry.1.value()))
    }

    /// Number of characters consumed past the longest match.
    pub fn overconsumed_code_points(&self) -> u8 {
        self.overconsumed_code_points
    }

    /// Whether the longest match ended with a semicolon.
    pub fn last_match_ends_with_semicolon(&self) -> bool {
        self.ends_with_semicolon
    }
}

impl Default for NamedCharacterReferenceMatcher {
    fn default() -> Self {
        Self::new()
    }
}
