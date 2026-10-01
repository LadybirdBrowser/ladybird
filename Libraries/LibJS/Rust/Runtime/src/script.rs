/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::parser_error::ParserError;
use libjs_rust::ast::ProgramType;
use libjs_rust::compile::{CompiledScript, ParsedProgram, compile_script, parse};

/// A script compiled to bytecode, as in Libraries/LibJS/Script.h.
pub struct Script {
    pub(crate) compiled: CompiledScript,
}

impl Script {
    /// 16.1.5 ParseScript ( sourceText, realm, hostDefined ), https://tc39.es/ecma262/#sec-parse-script
    pub fn parse(source: &[u16]) -> Result<Self, Vec<ParserError>> {
        let parsed = parse(source, ProgramType::Script, 1);
        if parsed.has_errors() {
            return Err(ParserError::all_from_parsed_program(&parsed));
        }
        Ok(Self::compile_parsed_program(parsed, source.len()))
    }

    /// Compiles a script the caller parsed without errors from `source_length` code units.
    pub fn compile_parsed_program(parsed: ParsedProgram, source_length: usize) -> Self {
        assert!(parsed.program_type() == ProgramType::Script && !parsed.has_errors());
        Self {
            compiled: compile_script(parsed, source_length),
        }
    }
}
