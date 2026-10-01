/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The parts of Libraries/LibJS/SourceCode.h the runtime has so far: the code a script, module or function was
//! compiled from, which functions keep to compile themselves lazily and to recover their [[SourceText]].

use std::rc::Rc;

use ak::Utf16String;

use crate::utf16::Utf16View;

pub struct SourceCode {
    filename: Utf16String,
    code: Utf16String,
}

impl SourceCode {
    pub fn create(filename: Utf16String, code: Utf16String) -> Rc<SourceCode> {
        Rc::new(Self { filename, code })
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
