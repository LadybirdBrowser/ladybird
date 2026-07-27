/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The small subset of libclang's C API needed by the layout extractor.

use std::ffi::{CStr, CString, c_char, c_int, c_longlong, c_uint, c_ulonglong, c_void};

const CURSOR_VAR_DECL: c_int = 9;
const CURSOR_NAMESPACE: c_int = 22;
const VISIT_CONTINUE: c_int = 1;
const EVAL_INT: c_int = 1;
const DIAGNOSTIC_ERROR: c_int = 3;
const SKIP_FUNCTION_BODIES: c_uint = 0x40;

#[repr(C)]
#[derive(Clone, Copy)]
struct Cursor {
    kind: c_int,
    xdata: c_int,
    data: [*const c_void; 3],
}

#[repr(C)]
#[derive(Clone, Copy)]
struct ClangString {
    data: *const c_void,
    private_flags: c_uint,
}

type Index = *mut c_void;
type TranslationUnit = *mut c_void;
type Diagnostic = *mut c_void;
type Evaluation = *mut c_void;
type File = *mut c_void;
type ClientData = *mut c_void;

unsafe extern "C" {
    fn clang_createIndex(exclude_declarations_from_pch: c_int, display_diagnostics: c_int) -> Index;
    fn clang_disposeIndex(index: Index);
    fn clang_parseTranslationUnit2(
        index: Index,
        source_filename: *const c_char,
        command_line_args: *const *const c_char,
        num_command_line_args: c_int,
        unsaved_files: *mut c_void,
        num_unsaved_files: c_uint,
        options: c_uint,
        out_translation_unit: *mut TranslationUnit,
    ) -> c_int;
    fn clang_disposeTranslationUnit(unit: TranslationUnit);
    fn clang_getNumDiagnostics(unit: TranslationUnit) -> c_uint;
    fn clang_getDiagnostic(unit: TranslationUnit, index: c_uint) -> Diagnostic;
    fn clang_getDiagnosticSeverity(diagnostic: Diagnostic) -> c_int;
    fn clang_formatDiagnostic(diagnostic: Diagnostic, options: c_uint) -> ClangString;
    fn clang_defaultDiagnosticDisplayOptions() -> c_uint;
    fn clang_disposeDiagnostic(diagnostic: Diagnostic);
    fn clang_getTranslationUnitCursor(unit: TranslationUnit) -> Cursor;
    fn clang_visitChildren(
        parent: Cursor,
        visitor: extern "C" fn(Cursor, Cursor, ClientData) -> c_int,
        data: ClientData,
    ) -> c_uint;
    fn clang_getCursorKind(cursor: Cursor) -> c_int;
    fn clang_getCursorSpelling(cursor: Cursor) -> ClangString;
    fn clang_Cursor_Evaluate(cursor: Cursor) -> Evaluation;
    fn clang_EvalResult_getKind(result: Evaluation) -> c_int;
    fn clang_EvalResult_isUnsignedInt(result: Evaluation) -> c_uint;
    fn clang_EvalResult_getAsUnsigned(result: Evaluation) -> c_ulonglong;
    fn clang_EvalResult_getAsLongLong(result: Evaluation) -> c_longlong;
    fn clang_EvalResult_dispose(result: Evaluation);
    fn clang_getInclusions(
        unit: TranslationUnit,
        visitor: extern "C" fn(File, *mut c_void, c_uint, ClientData),
        data: ClientData,
    );
    fn clang_getFileName(file: File) -> ClangString;
    fn clang_getCString(string: ClangString) -> *const c_char;
    fn clang_disposeString(string: ClangString);
}

fn owned_string(string: ClangString) -> String {
    // SAFETY: libclang owns the bytes until clang_disposeString().
    unsafe {
        let pointer = clang_getCString(string);
        let result = if pointer.is_null() {
            String::new()
        } else {
            CStr::from_ptr(pointer).to_string_lossy().into_owned()
        };
        clang_disposeString(string);
        result
    }
}

pub(crate) struct Unit {
    index: Index,
    unit: TranslationUnit,
}

pub(crate) fn parse(path: &str, arguments: &[String]) -> Result<Unit, String> {
    let path = CString::new(path).map_err(|_| "probe path contains a NUL byte".to_string())?;
    let arguments = arguments
        .iter()
        .map(|argument| CString::new(argument.as_str()).map_err(|_| argument))
        .collect::<Result<Vec<_>, _>>()
        .map_err(|argument| format!("compiler argument contains a NUL byte: {argument}"))?;
    let argument_pointers = arguments.iter().map(|argument| argument.as_ptr()).collect::<Vec<_>>();
    let argument_count =
        c_int::try_from(argument_pointers.len()).map_err(|_| "too many compiler arguments".to_string())?;

    // SAFETY: all pointers remain valid through the synchronous parse call.
    unsafe {
        let index = clang_createIndex(0, 0);
        if index.is_null() {
            return Err("clang_createIndex failed".to_string());
        }
        let mut unit = std::ptr::null_mut();
        let error = clang_parseTranslationUnit2(
            index,
            path.as_ptr(),
            argument_pointers.as_ptr(),
            argument_count,
            std::ptr::null_mut(),
            0,
            SKIP_FUNCTION_BODIES,
            &raw mut unit,
        );
        if error != 0 || unit.is_null() {
            clang_disposeIndex(index);
            return Err(format!("clang_parseTranslationUnit2 failed with code {error}"));
        }
        Ok(Unit { index, unit })
    }
}

impl Unit {
    pub(crate) fn errors(&self) -> Vec<String> {
        let mut errors = Vec::new();
        // SAFETY: self.unit remains alive throughout the loop.
        unsafe {
            for index in 0..clang_getNumDiagnostics(self.unit) {
                let diagnostic = clang_getDiagnostic(self.unit, index);
                if clang_getDiagnosticSeverity(diagnostic) >= DIAGNOSTIC_ERROR {
                    errors.push(owned_string(clang_formatDiagnostic(
                        diagnostic,
                        clang_defaultDiagnosticDisplayOptions(),
                    )));
                }
                clang_disposeDiagnostic(diagnostic);
            }
        }
        errors
    }

    pub(crate) fn query_values(&self, count: usize) -> Vec<Option<u64>> {
        let mut values = vec![None; count];
        let mut state = QueryState {
            values: &raw mut values,
        };
        // SAFETY: state and values outlive this synchronous traversal.
        unsafe {
            clang_visitChildren(
                clang_getTranslationUnitCursor(self.unit),
                query_visitor,
                (&raw mut state).cast(),
            );
        }
        values
    }

    pub(crate) fn included_files(&self) -> Vec<String> {
        let mut files = Vec::new();
        // SAFETY: files outlives this synchronous traversal.
        unsafe {
            clang_getInclusions(self.unit, inclusion_visitor, (&raw mut files).cast());
        }
        files.sort_unstable();
        files.dedup();
        files
    }
}

impl Drop for Unit {
    fn drop(&mut self) {
        // SAFETY: both handles are owned by self and disposed once.
        unsafe {
            clang_disposeTranslationUnit(self.unit);
            clang_disposeIndex(self.index);
        }
    }
}

struct QueryState {
    values: *mut Vec<Option<u64>>,
}

extern "C" fn query_visitor(cursor: Cursor, _parent: Cursor, data: ClientData) -> c_int {
    // SAFETY: data points to the live QueryState passed to clang_visitChildren().
    let state = unsafe { &mut *data.cast::<QueryState>() };
    let kind = unsafe { clang_getCursorKind(cursor) };
    if kind == CURSOR_NAMESPACE {
        // SAFETY: libclang permits recursive synchronous traversal.
        unsafe {
            clang_visitChildren(cursor, query_visitor, data);
        }
    } else if kind == CURSOR_VAR_DECL {
        let name = owned_string(unsafe { clang_getCursorSpelling(cursor) });
        if let Some(index) = name
            .strip_prefix("flap_Q")
            .and_then(|index| index.parse::<usize>().ok())
        {
            // SAFETY: cursor belongs to the live translation unit. The evaluation
            // result is disposed before returning.
            unsafe {
                let evaluation = clang_Cursor_Evaluate(cursor);
                if !evaluation.is_null() {
                    if clang_EvalResult_getKind(evaluation) == EVAL_INT
                        && let Some(slot) = (&mut *state.values).get_mut(index)
                    {
                        *slot = Some(if clang_EvalResult_isUnsignedInt(evaluation) != 0 {
                            clang_EvalResult_getAsUnsigned(evaluation)
                        } else {
                            clang_EvalResult_getAsLongLong(evaluation) as u64
                        });
                    }
                    clang_EvalResult_dispose(evaluation);
                }
            }
        }
    }
    VISIT_CONTINUE
}

extern "C" fn inclusion_visitor(file: File, _inclusion_stack: *mut c_void, _include_length: c_uint, data: ClientData) {
    let name = owned_string(unsafe { clang_getFileName(file) });
    if let Ok(path) = std::fs::canonicalize(name)
        && let Some(path) = path.to_str()
    {
        // SAFETY: data points to the live Vec passed to clang_getInclusions().
        unsafe { &mut *data.cast::<Vec<String>>() }.push(path.to_string());
    }
}
