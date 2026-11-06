/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The frontend's work, which needs no VM: parsing and compiling programs, which an embedder may do on any thread, as
//! C++ LibJS/ScriptCompilation.h allows, and then turning them into Script and Source Text Module Records on the VM's
//! thread.
//!
//! A JSParsedProgram and a JSCompiledProgram hold nothing of the VM or its heap, so any thread may create, inspect,
//! compile or destroy them, and they may move between threads. Everything that takes a JSVM runs on the VM's thread.
//!
//! The functions of a script or module that do not run right away compile on their first call.
//!
//! Tokenizing source text for syntax highlighting, and finding the positions of a source where a debugger can stop,
//! need no VM either.

use core::ffi::c_void;

use crate::ast::ProgramType;
use crate::compile::{
    CompiledProgram, ParsedProgram, compile_module, compile_parsed_program_off_thread, compile_script, parse,
};
use crate::embedding::abi_types::{
    JSOwnedUtf16String, JSRealm, JSSourceCode, JSUtf16View, cell_from_abi, cell_into_abi, owned_utf16_string_from_abi,
    vm_from_abi,
};
use crate::embedding::script::{JSParserErrorSink, JSScript, append_to_parser_error_sink, host_defined_slot_from_abi};
use crate::embedding::source_code::{shared_source_code_from_abi, source_code_into_abi};
use crate::layout::cell::Gc;
use crate::layout::host_class::{JSModule, JSVM};
use crate::parser_error::ParserError;
use crate::runtime::module::Module;
use crate::runtime::source_text_module::SourceTextModule;
use crate::script::Script;

/// A program that the frontend parsed, with or without syntax errors, which no VM owns: LibJS's JS::ParsedProgram.
pub struct JSParsedProgram {
    _opaque: [u8; 0],
}

/// A program compiled to bytecode, which no VM owns yet: LibJS's JS::CompiledProgram.
pub struct JSCompiledProgram {
    _opaque: [u8; 0],
}

/// JS::ProgramType: whether source text is a classic script or a module.
pub type JSProgramType = u8;

pub const JS_PROGRAM_TYPE_SCRIPT: JSProgramType = 0;
pub const JS_PROGRAM_TYPE_MODULE: JSProgramType = 1;

const _: () = assert!(JS_PROGRAM_TYPE_SCRIPT == ProgramType::Script as u8);
const _: () = assert!(JS_PROGRAM_TYPE_MODULE == ProgramType::Module as u8);

pub fn program_type_from_abi(program_type: JSProgramType) -> ProgramType {
    match program_type {
        JS_PROGRAM_TYPE_SCRIPT => ProgramType::Script,
        JS_PROGRAM_TYPE_MODULE => ProgramType::Module,
        program_type => panic!("{program_type} is not a program type"),
    }
}

fn parsed_program_into_abi(parsed: ParsedProgram) -> *mut JSParsedProgram {
    Box::into_raw(Box::new(parsed)).cast()
}

/// # Safety
///
/// `parsed` must be a parsed program the ABI handed out and that is still alive for `'a`.
unsafe fn parsed_program_from_abi<'a>(parsed: *const JSParsedProgram) -> &'a ParsedProgram {
    assert!(!parsed.is_null(), "the embedder passes a parsed program");
    // SAFETY: The caller passes a live parsed program, which is a boxed ParsedProgram.
    unsafe { &*parsed.cast::<ParsedProgram>() }
}

/// # Safety
///
/// `parsed` must be a parsed program the ABI handed out, which the caller gives up.
unsafe fn take_parsed_program_from_abi(parsed: *mut JSParsedProgram) -> ParsedProgram {
    assert!(!parsed.is_null(), "the embedder passes a parsed program");
    // SAFETY: The caller gives up a boxed ParsedProgram.
    *unsafe { Box::from_raw(parsed.cast::<ParsedProgram>()) }
}

fn compiled_program_into_abi(compiled: CompiledProgram) -> *mut JSCompiledProgram {
    Box::into_raw(Box::new(compiled)).cast()
}

/// # Safety
///
/// `compiled` must be a compiled program the ABI handed out, which the caller gives up.
unsafe fn take_compiled_program_from_abi(compiled: *mut JSCompiledProgram) -> CompiledProgram {
    assert!(!compiled.is_null(), "the embedder passes a compiled program");
    // SAFETY: The caller gives up a boxed CompiledProgram.
    *unsafe { Box::from_raw(compiled.cast::<CompiledProgram>()) }
}

fn module_into_abi(module: Gc<SourceTextModule>) -> *mut JSModule {
    module.upcast::<Module>().as_ptr().cast()
}

/// # Safety
///
/// `module` must be a live Source Text Module Record.
pub unsafe fn source_text_module_from_abi(module: *mut JSModule) -> Gc<SourceTextModule> {
    let module = core::ptr::NonNull::new(module.cast::<Module>()).expect("the embedder passes a module");
    // SAFETY: The caller passes a live module.
    unsafe { Gc::from_non_null(module) }
        .downcast::<SourceTextModule>()
        .expect("the embedder passes a Source Text Module Record")
}

/// ParsedProgram::parse(source_text, type, line_number_offset): parses `source` as a classic script or a module, with
/// its lines counted from `line_number_offset`, except that a module's lines start at 1 or later. The program holds its
/// syntax errors, if it has any. The caller owns the program. Adopts the string, whose storage the program shares from
/// then on. Any thread may call this.
///
/// # Safety
///
/// `source` must be a raw AK::Utf16String whose reference the caller gives up.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_compile_parse(
    source: JSOwnedUtf16String,
    program_type: JSProgramType,
    line_number_offset: usize,
) -> *mut JSParsedProgram {
    // SAFETY: The caller transfers its reference.
    let source = unsafe { owned_utf16_string_from_abi(source) };
    parsed_program_into_abi(parse(&source, program_type_from_abi(program_type), line_number_offset))
}

/// ParsedProgram::has_errors(). Any thread may call this.
///
/// # Safety
///
/// `parsed` must be a live parsed program.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_compile_parsed_program_has_errors(parsed: *const JSParsedProgram) -> bool {
    // SAFETY: The caller passes a live parsed program.
    unsafe { parsed_program_from_abi(parsed) }.has_errors()
}

/// Destroys a parsed program that the caller owns. Null does nothing. Any thread may call this.
///
/// # Safety
///
/// `parsed` must be null or a live parsed program, which the caller gives up.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_compile_parsed_program_destroy(parsed: *mut JSParsedProgram) {
    if !parsed.is_null() {
        // SAFETY: The caller gives up a live parsed program.
        drop(unsafe { take_parsed_program_from_abi(parsed) });
    }
}

/// CompiledProgram::compile(ParsedProgram): compiles a program that parsed without errors, which this consumes. The
/// bytecode covers the top level and the functions it invokes right away; every other function compiles on its first
/// call. The caller owns the compiled program. Any thread may call this.
///
/// # Safety
///
/// `parsed` must be a live parsed program without errors, which the caller gives up.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_compile_parsed_program(parsed: *mut JSParsedProgram) -> *mut JSCompiledProgram {
    // SAFETY: The caller gives up a live parsed program.
    let parsed = unsafe { take_parsed_program_from_abi(parsed) };
    compiled_program_into_abi(compile_parsed_program_off_thread(parsed))
}

/// # Safety
///
/// `compiled` must be a compiled program the ABI handed out and that is still alive for `'a`.
pub unsafe fn compiled_program_from_abi<'a>(compiled: *const JSCompiledProgram) -> &'a CompiledProgram {
    assert!(!compiled.is_null(), "the embedder passes a compiled program");
    // SAFETY: The caller passes a live compiled program, which is a boxed CompiledProgram.
    unsafe { &*compiled.cast::<CompiledProgram>() }
}

/// CompiledProgram::clone(): another reference to the same compiled program, which the caller owns. Any thread may call
/// this.
///
/// # Safety
///
/// `compiled` must be a live compiled program.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_compile_compiled_program_clone(
    compiled: *const JSCompiledProgram,
) -> *mut JSCompiledProgram {
    // SAFETY: The caller passes a live compiled program.
    compiled_program_into_abi(unsafe { compiled_program_from_abi(compiled) }.clone())
}

/// CompiledProgram::memory_size(). Any thread may call this.
///
/// # Safety
///
/// `compiled` must be a live compiled program.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_compile_compiled_program_memory_size(compiled: *const JSCompiledProgram) -> usize {
    // SAFETY: The caller passes a live compiled program.
    unsafe { compiled_program_from_abi(compiled) }.memory_size()
}

/// CompiledProgram::set_retains_compiled_functions(). Any thread may call this.
///
/// # Safety
///
/// `compiled` must be a live compiled program.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_compile_compiled_program_set_retains_compiled_functions(
    compiled: *const JSCompiledProgram,
    retains_compiled_functions: bool,
) {
    // SAFETY: The caller passes a live compiled program.
    unsafe { compiled_program_from_abi(compiled) }.set_retains_compiled_functions(retains_compiled_functions);
}

/// Destroys a compiled program that the caller owns and that will not run, freeing what the frontend compiled for it.
/// Null does nothing. Any thread may call this.
///
/// # Safety
///
/// `compiled` must be null or a live compiled program, which the caller gives up.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_compile_compiled_program_destroy(compiled: *mut JSCompiledProgram) {
    if !compiled.is_null() {
        // SAFETY: The caller gives up a live compiled program.
        drop(unsafe { take_compiled_program_from_abi(compiled) });
    }
}

/// The parsed program, if it has no syntax errors. Otherwise appends them to `errors` and destroys the program.
///
/// # Safety
///
/// `errors` must be null or a valid sink.
unsafe fn parsed_program_without_errors(
    parsed: ParsedProgram,
    errors: *const JSParserErrorSink,
) -> Option<ParsedProgram> {
    if !parsed.has_errors() {
        return Some(parsed);
    }
    let parser_errors = ParserError::all_from_parsed_program(&parsed);
    drop(parsed);
    // SAFETY: The caller passes null or a valid sink.
    unsafe { append_to_parser_error_sink(errors, &parser_errors) };
    None
}

/// create_script(ParsedProgram, source_code, realm, filename, host_defined): the Script Record of a classic script
/// parsed from the code of `source_code`, which this compiles now and which reports the filename of the source code.
/// Its dynamic imports resolve against `filename`, and `host_defined` (null for none) is its [[HostDefined]]. Consumes
/// the program. Returns the script, which the caller keeps alive, or null after appending the program's syntax errors
/// to `errors` (which may be null). Only the VM's thread may call this.
///
/// # Safety
///
/// `vm`, `source_code` and `realm` must be live, `parsed` a live parsed script, which the caller gives up, `filename`
/// a valid view, `host_defined` null or a live cell, and `errors` null or a valid sink.
#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments, reason = "C++ create_script takes all of these")]
pub unsafe extern "C" fn js_compile_create_script_from_parsed_program(
    vm: *mut JSVM,
    parsed: *mut JSParsedProgram,
    source_code: *const JSSourceCode,
    realm: *mut JSRealm,
    filename: JSUtf16View,
    host_defined: *mut c_void,
    errors: *const JSParserErrorSink,
) -> *mut JSScript {
    // SAFETY: The caller passes a live VM, program, source code, realm and host-defined cell, and a valid view.
    let (vm, parsed, source_code, realm, filename, host_defined) = unsafe {
        (
            vm_from_abi(vm),
            take_parsed_program_from_abi(parsed),
            shared_source_code_from_abi(source_code),
            cell_from_abi(realm),
            filename.as_view().to_utf8(),
            host_defined_slot_from_abi(host_defined),
        )
    };
    // SAFETY: The caller passes null or a valid sink.
    let Some(parsed) = (unsafe { parsed_program_without_errors(parsed, errors) }) else {
        return core::ptr::null_mut();
    };
    cell_into_abi(Script::create(
        vm,
        realm,
        compile_script(parsed),
        source_code,
        &filename,
        host_defined,
    ))
}

/// create_script(CompiledProgram, source_code, realm, filename, host_defined): the Script Record of a classic script
/// compiled, on any thread, from the code of `source_code`, which reports the filename of the source code. Its dynamic
/// imports resolve against `filename`, and `host_defined` (null for none) is its [[HostDefined]]. Consumes the program.
/// Returns the script, which the caller keeps alive. Only the VM's thread may call this.
///
/// # Safety
///
/// `vm`, `source_code` and `realm` must be live, `compiled` a live compiled script, which the caller gives up,
/// `filename` a valid view, and `host_defined` null or a live cell.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_compile_create_script_from_compiled_program(
    vm: *mut JSVM,
    compiled: *mut JSCompiledProgram,
    source_code: *const JSSourceCode,
    realm: *mut JSRealm,
    filename: JSUtf16View,
    host_defined: *mut c_void,
) -> *mut JSScript {
    // SAFETY: The caller passes a live VM, program, source code, realm and host-defined cell, and a valid view.
    let (vm, compiled, source_code, realm, filename, host_defined) = unsafe {
        (
            vm_from_abi(vm),
            take_compiled_program_from_abi(compiled),
            shared_source_code_from_abi(source_code),
            cell_from_abi(realm),
            filename.as_view().to_utf8(),
            host_defined_slot_from_abi(host_defined),
        )
    };
    cell_into_abi(Script::create(
        vm,
        realm,
        compiled.into_script(),
        source_code,
        &filename,
        host_defined,
    ))
}

/// create_module(ParsedProgram, source_code, realm, filename, host_defined): like
/// js_compile_create_script_from_parsed_program(), but for a module, whose imports resolve against `filename`. Returns
/// the Source Text Module Record, or null after appending the program's syntax errors to `errors`. Only the VM's
/// thread may call this.
///
/// # Safety
///
/// As for js_compile_create_script_from_parsed_program(), with a parsed module.
#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments, reason = "C++ create_module takes all of these")]
pub unsafe extern "C" fn js_compile_create_module_from_parsed_program(
    vm: *mut JSVM,
    parsed: *mut JSParsedProgram,
    source_code: *const JSSourceCode,
    realm: *mut JSRealm,
    filename: JSUtf16View,
    host_defined: *mut c_void,
    errors: *const JSParserErrorSink,
) -> *mut JSModule {
    // SAFETY: The caller passes a live VM, program, source code, realm and host-defined cell, and a valid view.
    let (vm, parsed, source_code, realm, filename, host_defined) = unsafe {
        (
            vm_from_abi(vm),
            take_parsed_program_from_abi(parsed),
            shared_source_code_from_abi(source_code),
            cell_from_abi(realm),
            filename.as_view().to_utf8(),
            host_defined_slot_from_abi(host_defined),
        )
    };
    // SAFETY: The caller passes null or a valid sink.
    let Some(parsed) = (unsafe { parsed_program_without_errors(parsed, errors) }) else {
        return core::ptr::null_mut();
    };
    module_into_abi(SourceTextModule::create(
        vm,
        realm,
        &filename,
        compile_module(parsed),
        source_code,
        host_defined,
    ))
}

/// create_module(CompiledProgram, source_code, realm, filename, host_defined): like
/// js_compile_create_script_from_compiled_program(), but for a module, whose imports resolve against `filename`.
/// Returns the Source Text Module Record. Only the VM's thread may call this.
///
/// # Safety
///
/// As for js_compile_create_script_from_compiled_program(), with a compiled module.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_compile_create_module_from_compiled_program(
    vm: *mut JSVM,
    compiled: *mut JSCompiledProgram,
    source_code: *const JSSourceCode,
    realm: *mut JSRealm,
    filename: JSUtf16View,
    host_defined: *mut c_void,
) -> *mut JSModule {
    // SAFETY: The caller passes a live VM, program, source code, realm and host-defined cell, and a valid view.
    let (vm, compiled, source_code, realm, filename, host_defined) = unsafe {
        (
            vm_from_abi(vm),
            take_compiled_program_from_abi(compiled),
            shared_source_code_from_abi(source_code),
            cell_from_abi(realm),
            filename.as_view().to_utf8(),
            host_defined_slot_from_abi(host_defined),
        )
    };
    module_into_abi(SourceTextModule::create(
        vm,
        realm,
        &filename,
        compiled.into_module(),
        source_code,
        host_defined,
    ))
}

/// top_level_source_code(Script const&): the source code that the script's top-level code was compiled from, which the
/// script keeps alive, and a caller that keeps it longer retains. Only the VM's thread may call this.
///
/// # Safety
///
/// `script` must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_compile_top_level_source_code_of_script(script: *mut JSScript) -> *const JSSourceCode {
    // SAFETY: The caller passes a live script.
    let script = unsafe { cell_from_abi(script) };
    script
        .cached_executable()
        .source_code
        .as_ref()
        .map_or(core::ptr::null(), source_code_into_abi)
}

/// top_level_source_code(SourceTextModule const&): like js_compile_top_level_source_code_of_script(), but null for a
/// module with top-level await, whose body is compiled as an async function instead. Only the VM's thread may call
/// this.
///
/// # Safety
///
/// `module` must be a live Source Text Module Record.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_compile_top_level_source_code_of_module(module: *mut JSModule) -> *const JSSourceCode {
    // SAFETY: The caller passes a live Source Text Module Record.
    let module = unsafe { source_text_module_from_abi(module) };
    module
        .cached_executable()
        .and_then(|executable| executable.source_code.as_ref().map(source_code_into_abi))
        .unwrap_or(core::ptr::null())
}

/// A token of a source text, with the trivia (whitespace and comments) before it, which JS::SyntaxHighlighter reads.
/// token_type and category are the values of JS::TokenType and JS::TokenCategory, and the offsets and lengths count
/// UTF-16 code units.
#[repr(C)]
pub struct JSToken {
    pub token_type: u8,
    pub category: u8,
    pub offset: u32,
    pub length: u32,
    pub trivia_offset: u32,
    pub trivia_length: u32,
}

const _: () = assert!(size_of::<JSToken>() == 20 && core::mem::offset_of!(JSToken, offset) == 4);

/// Where the runtime hands the tokens of a source text, one at a time, borrowed for the call.
#[repr(C)]
pub struct JSTokenSink {
    pub context: *mut c_void,
    pub append: Option<unsafe extern "C" fn(context: *mut c_void, token: *const JSToken)>,
}

// JS::TokenCategory.
pub const JS_TOKEN_CATEGORY_INVALID: u8 = 0;
pub const JS_TOKEN_CATEGORY_TRIVIA: u8 = 1;
pub const JS_TOKEN_CATEGORY_NUMBER: u8 = 2;
pub const JS_TOKEN_CATEGORY_STRING: u8 = 3;
pub const JS_TOKEN_CATEGORY_PUNCTUATION: u8 = 4;
pub const JS_TOKEN_CATEGORY_OPERATOR: u8 = 5;
pub const JS_TOKEN_CATEGORY_KEYWORD: u8 = 6;
pub const JS_TOKEN_CATEGORY_CONTROL_KEYWORD: u8 = 7;
pub const JS_TOKEN_CATEGORY_IDENTIFIER: u8 = 8;

const _: () = {
    use crate::token::TokenCategory;
    assert!(JS_TOKEN_CATEGORY_INVALID == TokenCategory::Invalid as u8);
    assert!(JS_TOKEN_CATEGORY_TRIVIA == TokenCategory::Trivia as u8);
    assert!(JS_TOKEN_CATEGORY_NUMBER == TokenCategory::Number as u8);
    assert!(JS_TOKEN_CATEGORY_STRING == TokenCategory::String as u8);
    assert!(JS_TOKEN_CATEGORY_PUNCTUATION == TokenCategory::Punctuation as u8);
    assert!(JS_TOKEN_CATEGORY_OPERATOR == TokenCategory::Operator as u8);
    assert!(JS_TOKEN_CATEGORY_KEYWORD == TokenCategory::Keyword as u8);
    assert!(JS_TOKEN_CATEGORY_CONTROL_KEYWORD == TokenCategory::ControlKeyword as u8);
    assert!(JS_TOKEN_CATEGORY_IDENTIFIER == TokenCategory::Identifier as u8);
};

/// Tokenizes `source` without parsing it, as JS::SyntaxHighlighter does, and hands every token to `tokens` on the
/// calling thread. The last token is the end-of-file token, whose trivia is whatever follows the token before it.
/// Borrows the view. Any thread may call this.
///
/// # Safety
///
/// The view must be valid, and `tokens` must point to a sink with an append function.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_compile_tokenize(source: JSUtf16View, tokens: *const JSTokenSink) {
    // SAFETY: The caller passes a valid view and sink.
    let (source, tokens) = unsafe { (source.as_view(), &*tokens) };
    let append = tokens.append.expect("a token sink has an append function");
    for token in crate::tokenize::tokenize(source) {
        let token = JSToken {
            token_type: token.token_type as u8,
            category: token.category as u8,
            offset: token.offset,
            length: token.length,
            trivia_offset: token.trivia_offset,
            trivia_length: token.trivia_length,
        };
        // SAFETY: The embedder's sink takes tokens with the context it came with, and borrows each for the call.
        unsafe { append(tokens.context, &raw const token) };
    }
}

/// JS::Position: a line and a column, both counted from 1.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct JSPosition {
    pub line: u32,
    pub column: u32,
}

/// Where the runtime hands a run of positions, borrowed for the call.
#[repr(C)]
pub struct JSPositionSink {
    pub context: *mut c_void,
    pub append: Option<unsafe extern "C" fn(context: *mut c_void, positions: *const JSPosition, count: usize)>,
}

/// breakpoint_positions_for_source(source_code, type, line_number_offset): every position in `source` where a
/// breakpoint can be set, those inside functions that have not been compiled yet included, sorted and without repeats.
/// Lines count from `line_number_offset` as js_compile_parse() counts them, and a source with syntax errors has none.
/// This compiles a private copy of the source, which needs no VM. Hands the positions to `positions` in one call on
/// the calling thread, if there are any. Borrows the view. Any thread may call this.
///
/// # Safety
///
/// The view must be valid, and `positions` must point to a sink with an append function.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_compile_breakpoint_positions_for_source(
    source: JSUtf16View,
    program_type: JSProgramType,
    line_number_offset: usize,
    positions: *const JSPositionSink,
) {
    // SAFETY: The caller passes a valid view and sink.
    let (source, sink) = unsafe { (source.as_view().to_utf16_string(), &*positions) };
    let append = sink.append.expect("a position sink has an append function");
    let positions: Vec<JSPosition> = crate::breakpoint_positions::breakpoint_positions_for_source(
        &source,
        program_type_from_abi(program_type),
        line_number_offset,
    )
    .into_iter()
    .map(|position| JSPosition {
        line: position.line,
        column: position.column,
    })
    .collect();
    if positions.is_empty() {
        return;
    }
    // SAFETY: The embedder's sink takes the positions with the context it came with, and borrows them for the call.
    unsafe { append(sink.context, positions.as_ptr(), positions.len()) };
}
