/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The parts of Libraries/LibJS/SourceCode.h the runtime has so far: the code a script, module or function was
//! compiled from, which functions keep to compile themselves lazily and to recover their [[SourceText]].

use std::cell::OnceCell;
use std::rc::Rc;

use ak::Utf16String;

use crate::utf16::Utf16View;

pub struct SourceCode {
    filename: Utf16String,
    code: Utf16String,
    // The code widened to UTF-16 code units, made on first use for code in the ASCII storage kind, as C++
    // SourceCode::utf16_data() caches it for an embedder that hands the code to a parser on another thread.
    utf16_code_units_of_ascii_code: OnceCell<Box<[u16]>>,
}

impl SourceCode {
    pub fn create(filename: Utf16String, code: Utf16String) -> Rc<SourceCode> {
        Rc::new(Self {
            filename,
            code,
            utf16_code_units_of_ascii_code: OnceCell::new(),
        })
    }

    pub fn filename(&self) -> &Utf16String {
        &self.filename
    }

    pub fn code(&self) -> &Utf16String {
        &self.code
    }

    pub fn length_in_code_units(&self) -> usize {
        Utf16View::of_string(&self.code).length_in_code_units()
    }

    /// The code as UTF-16 code units, which stay where they are for as long as the source code lives.
    pub fn utf16_code_units(&self) -> &[u16] {
        match Utf16View::of_string(&self.code) {
            Utf16View::Utf16(code_units) => code_units,
            ascii @ Utf16View::Ascii(_) => self
                .utf16_code_units_of_ascii_code
                .get_or_init(|| ascii.code_units().collect()),
        }
    }

    pub fn source_text_from_offsets(&self, start_offset: usize, length: usize) -> Utf16String {
        if length == 0 {
            return Utf16String::default();
        }

        assert!(start_offset <= usize::MAX - length);

        Utf16View::of_string(&self.code)
            .substring_view(start_offset, length)
            .to_utf16_string()
    }
}
