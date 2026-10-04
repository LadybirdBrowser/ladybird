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
//! The functions of a script or module that do not run right away compile on their first call. An embedder can have
//! them compiled on its worker threads before that instead, through JSOffThreadCompilationCallbacks.
//!
//! Tokenizing source text for syntax highlighting, and finding the positions of a source where a debugger can stop,
//! need no VM either.

use core::ffi::c_void;
use std::borrow::Cow;
use std::sync::Arc;

use crate::bytecode::executable::Executable;
use crate::embedding::abi_types::{JSRealm, JSSourceCode, JSUtf16View, cell_from_abi, cell_into_abi, vm_from_abi};
use crate::embedding::script::{JSParserErrorSink, JSScript, append_to_parser_error_sink, host_defined_slot_from_abi};
use crate::embedding::source_code::{shared_source_code_from_abi, source_code_into_abi};
use crate::gc::root::Root;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::host_class::{JSModule, JSVM};
use crate::parser_error::ParserError;
use crate::runtime::module::Module;
use crate::runtime::shared_function_instance_data::{SharedFunctionInstanceData, discard_precompiled_function};
use crate::runtime::source_text_module::SourceTextModule;
use crate::script::Script;
use crate::utf16::Utf16View;
use libjs_rust::ast::{FunctionPayload, ProgramType};
use libjs_rust::bytecode::generator::PrecompiledFunction;
use libjs_rust::compile::{
    CompiledProgram, FunctionPrecompileMode, ParsedProgram, compile_function, compile_module,
    compile_parsed_program_off_thread, compile_script, parse,
};

/// A program that the frontend parsed, with or without syntax errors, which no VM owns: C++ JS::ParsedProgram.
pub struct JSParsedProgram {
    _opaque: [u8; 0],
}

/// A program compiled to bytecode, which no VM owns yet: C++ JS::CompiledProgram.
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

/// What the frontend parses from: UTF-16 code units, which a view in the ASCII storage kind is widened to.
///
/// # Safety
///
/// The view must be valid for `'a`.
pub unsafe fn code_units_of<'a>(source: JSUtf16View) -> Cow<'a, [u16]> {
    // SAFETY: The caller passes a valid view.
    match unsafe { source.as_view() } {
        Utf16View::Utf16(code_units) => Cow::Borrowed(code_units),
        ascii @ Utf16View::Ascii(_) => Cow::Owned(ascii.code_units().collect()),
    }
}

/// A parsed program together with the length of the source it was parsed from, which compiling it needs.
struct ParsedSourceText {
    program: ParsedProgram,
    source_length_in_code_units: usize,
}

fn parsed_program_into_abi(parsed: ParsedSourceText) -> *mut JSParsedProgram {
    Box::into_raw(Box::new(parsed)).cast()
}

/// # Safety
///
/// `parsed` must be a parsed program the ABI handed out and that is still alive for `'a`.
unsafe fn parsed_program_from_abi<'a>(parsed: *const JSParsedProgram) -> &'a ParsedSourceText {
    assert!(!parsed.is_null(), "the embedder passes a parsed program");
    // SAFETY: The caller passes a live parsed program, which is a boxed ParsedSourceText.
    unsafe { &*parsed.cast::<ParsedSourceText>() }
}

/// # Safety
///
/// `parsed` must be a parsed program the ABI handed out, which the caller gives up.
unsafe fn take_parsed_program_from_abi(parsed: *mut JSParsedProgram) -> ParsedSourceText {
    assert!(!parsed.is_null(), "the embedder passes a parsed program");
    // SAFETY: The caller gives up a boxed ParsedSourceText.
    *unsafe { Box::from_raw(parsed.cast::<ParsedSourceText>()) }
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
/// syntax errors, if it has any. The caller owns the program. Borrows the view. Any thread may call this.
///
/// # Safety
///
/// The view must be valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_compile_parse(
    source: JSUtf16View,
    program_type: JSProgramType,
    line_number_offset: usize,
) -> *mut JSParsedProgram {
    // SAFETY: The caller passes a valid view.
    let source = unsafe { code_units_of(source) };
    parsed_program_into_abi(ParsedSourceText {
        program: parse(&source, program_type_from_abi(program_type), line_number_offset),
        source_length_in_code_units: source.len(),
    })
}

/// ParsedProgram::has_errors(). Any thread may call this.
///
/// # Safety
///
/// `parsed` must be a live parsed program.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_compile_parsed_program_has_errors(parsed: *const JSParsedProgram) -> bool {
    // SAFETY: The caller passes a live parsed program.
    unsafe { parsed_program_from_abi(parsed) }.program.has_errors()
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
/// call, or through js_compile_remaining_functions_of_script_off_thread() and
/// js_compile_remaining_functions_of_module_off_thread(). The caller owns the compiled program. Any thread may call
/// this.
///
/// # Safety
///
/// `parsed` must be a live parsed program without errors, which the caller gives up.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_compile_parsed_program(parsed: *mut JSParsedProgram) -> *mut JSCompiledProgram {
    // SAFETY: The caller gives up a live parsed program.
    let parsed = unsafe { take_parsed_program_from_abi(parsed) };
    compiled_program_into_abi(compile_parsed_program_off_thread(
        parsed.program,
        parsed.source_length_in_code_units,
        FunctionPrecompileMode::EagerOnly,
    ))
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
        unsafe { take_compiled_program_from_abi(compiled) }.discard();
    }
}

/// The parsed program, if it has no syntax errors. Otherwise appends them to `errors` and destroys the program.
///
/// # Safety
///
/// `errors` must be null or a valid sink.
unsafe fn parsed_program_without_errors(
    parsed: ParsedSourceText,
    errors: *const JSParserErrorSink,
) -> Option<ParsedProgram> {
    if !parsed.program.has_errors() {
        return Some(parsed.program);
    }
    let parser_errors = ParserError::all_from_parsed_program(&parsed.program);
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
    let source_length = source_code.length_in_code_units();
    cell_into_abi(Script::create(
        vm,
        realm,
        compile_script(parsed, source_length),
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
    let source_length = source_code.length_in_code_units();
    module_into_abi(SourceTextModule::create(
        vm,
        realm,
        &filename,
        compile_module(parsed, source_length),
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
/// module with top-level await, whose body is compiled as an async function instead, as in C++. Only the VM's thread
/// may call this.
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

/// Work that the embedder runs once, on the thread that the callback receiving it names: C++ Function<void()>.
#[repr(C)]
pub struct JSOffThreadTask {
    pub run: Option<unsafe extern "C" fn(data: *mut c_void)>,
    pub data: *mut c_void,
}

/// OffThreadCompilationCallbacks: how an embedder runs background compilation work. The runtime copies the struct and
/// calls the functions with `context` from both threads, so they and the context must work on any thread.
///
/// - `submit_work` is called on the VM's thread, and must run the task on a worker thread.
/// - `post_to_main_thread` is called on a worker thread, and must run the task on the VM's thread.
/// - `release`, which may be null, is called once, on either thread, when the runtime no longer needs the callbacks.
///
/// A callback may also run its task right away when it is already on the task's thread, which is how an embedder
/// without threads runs the work synchronously. The tasks hold nothing of the VM that a callback must keep alive.
#[repr(C)]
pub struct JSOffThreadCompilationCallbacks {
    pub context: *mut c_void,
    pub submit_work: Option<unsafe extern "C" fn(context: *mut c_void, task: JSOffThreadTask)>,
    pub post_to_main_thread: Option<unsafe extern "C" fn(context: *mut c_void, task: JSOffThreadTask)>,
    pub release: Option<unsafe extern "C" fn(context: *mut c_void)>,
}

/// The embedder's callbacks, which the VM's thread and the worker share, as C++ SharedOffThreadCompilationCallbacks
/// does: the worker can still be inside post_to_main_thread() when the VM's thread has run the task it posted.
struct SharedOffThreadCompilationCallbacks(JSOffThreadCompilationCallbacks);

// SAFETY: The embedder's callbacks work on any thread, as JSOffThreadCompilationCallbacks requires.
unsafe impl Send for SharedOffThreadCompilationCallbacks {}
// SAFETY: As above.
unsafe impl Sync for SharedOffThreadCompilationCallbacks {}

impl SharedOffThreadCompilationCallbacks {
    fn submit_work(&self, run: unsafe extern "C" fn(*mut c_void), data: *mut c_void) {
        let submit_work = self.0.submit_work.expect("the embedder submits work to its workers");
        // SAFETY: The embedder's callback takes tasks with the context it came with.
        unsafe { submit_work(self.0.context, JSOffThreadTask { run: Some(run), data }) };
    }

    fn post_to_main_thread(&self, run: unsafe extern "C" fn(*mut c_void), data: *mut c_void) {
        let post_to_main_thread = self
            .0
            .post_to_main_thread
            .expect("the embedder posts tasks to the VM's thread");
        // SAFETY: As above.
        unsafe { post_to_main_thread(self.0.context, JSOffThreadTask { run: Some(run), data }) };
    }
}

impl Drop for SharedOffThreadCompilationCallbacks {
    fn drop(&mut self) {
        if let Some(release) = self.0.release {
            // SAFETY: The runtime is done with the callbacks, which the embedder releases once.
            unsafe { release(self.0.context) };
        }
    }
}

/// What the VM's thread keeps while a worker compiles copies of the ASTs of some functions: their shared data, which
/// stays alive until the compiled functions are installed in it.
struct LazyFunctionsAwaitingCompilation {
    vm: &'static Vm,
    shared_function_data: Root<'static, Vec<Gc<SharedFunctionInstanceData>>>,
    callbacks: Arc<SharedOffThreadCompilationCallbacks>,
}

/// What a worker carries without touching it, from the VM's thread and back, as C++ carries its GC roots.
struct OnlyForTheVmsThread(Box<LazyFunctionsAwaitingCompilation>);

// SAFETY: Only the VM's thread opens what the worker carries.
unsafe impl Send for OnlyForTheVmsThread {}

/// The task a worker runs.
#[allow(clippy::vec_box, reason = "the frontend compiles boxed ASTs")]
struct LazyFunctionCompilation {
    function_asts: Vec<Box<FunctionPayload>>,
    source_length_in_code_units: usize,
    callbacks: Arc<SharedOffThreadCompilationCallbacks>,
    awaiting_compilation: OnlyForTheVmsThread,
}

/// The task the worker posts back to the VM's thread.
#[allow(
    clippy::vec_box,
    reason = "the frontend returns boxed functions, which shared function data keeps boxed"
)]
struct CompiledLazyFunctions {
    compiled_functions: Vec<Box<PrecompiledFunction>>,
    awaiting_compilation: OnlyForTheVmsThread,
}

const _: fn() = || {
    fn assert_send<T: Send>() {}
    assert_send::<LazyFunctionCompilation>();
    assert_send::<CompiledLazyFunctions>();
};

/// Has a worker compile copies of the ASTs of the functions, and installs the results on the VM's thread, as C++
/// compile_lazy_functions_off_thread() does.
fn compile_lazy_functions_off_thread(
    vm: &'static Vm,
    functions: Vec<(Gc<SharedFunctionInstanceData>, Box<FunctionPayload>)>,
    source_length_in_code_units: usize,
    callbacks: Arc<SharedOffThreadCompilationCallbacks>,
) {
    let (shared_function_data, function_asts): (Vec<_>, Vec<_>) = functions.into_iter().unzip();
    let awaiting_compilation = LazyFunctionsAwaitingCompilation {
        vm,
        shared_function_data: Root::new(vm, shared_function_data),
        callbacks: callbacks.clone(),
    };
    let work = Box::new(LazyFunctionCompilation {
        function_asts,
        source_length_in_code_units,
        callbacks: callbacks.clone(),
        awaiting_compilation: OnlyForTheVmsThread(Box::new(awaiting_compilation)),
    });
    callbacks.submit_work(compile_lazy_functions_on_a_worker, Box::into_raw(work).cast());
}

unsafe extern "C" fn compile_lazy_functions_on_a_worker(work: *mut c_void) {
    // SAFETY: The embedder runs each task once, with the data that came with it, which is this boxed work.
    let work = *unsafe { Box::from_raw(work.cast::<LazyFunctionCompilation>()) };
    let compiled_functions = work
        .function_asts
        .into_iter()
        .map(|function_ast| {
            compile_function(
                function_ast,
                work.source_length_in_code_units,
                false,
                FunctionPrecompileMode::All,
            )
        })
        .collect();
    let compiled = Box::new(CompiledLazyFunctions {
        compiled_functions,
        awaiting_compilation: work.awaiting_compilation,
    });
    work.callbacks
        .post_to_main_thread(install_compiled_lazy_functions, Box::into_raw(compiled).cast());
}

unsafe extern "C" fn install_compiled_lazy_functions(compiled: *mut c_void) {
    // SAFETY: The embedder runs each task once, with the data that came with it, which is this boxed result.
    let compiled = *unsafe { Box::from_raw(compiled.cast::<CompiledLazyFunctions>()) };
    let awaiting_compilation = compiled.awaiting_compilation.0;
    let shared_function_data = awaiting_compilation.shared_function_data.get().clone();
    assert_eq!(shared_function_data.len(), compiled.compiled_functions.len());
    for (shared_data, compiled_function) in shared_function_data.into_iter().zip(compiled.compiled_functions) {
        // A function that started running in the meantime compiled on this thread, so the worker compiles the
        // functions it creates instead.
        if let Some(executable) = shared_data.executable() {
            compile_remaining_functions_of_executable_off_thread(
                awaiting_compilation.vm,
                executable,
                awaiting_compilation.callbacks.clone(),
            );
            discard_precompiled_function(compiled_function);
            continue;
        }
        // Installing a bytecode cache may have dropped the AST, and its bytecode with it, while the worker compiled.
        if !shared_data.has_function_ast() {
            discard_precompiled_function(compiled_function);
            continue;
        }
        shared_data.set_precompiled_bytecode_executable(compiled_function);
    }
    drop(awaiting_compilation);
}

fn compile_remaining_functions_of_executable_off_thread(
    vm: &'static Vm,
    executable: Gc<Executable>,
    callbacks: Arc<SharedOffThreadCompilationCallbacks>,
) {
    let functions = SharedFunctionInstanceData::uncompiled_functions_of(executable);
    if functions.is_empty() {
        return;
    }
    // NB: The frontend checks that nested functions lie within the source, which is unknown without a source code.
    let source_length_in_code_units = executable
        .source_code
        .as_ref()
        .map_or(usize::MAX, |source_code| source_code.length_in_code_units());
    compile_lazy_functions_off_thread(vm, functions, source_length_in_code_units, callbacks);
}

/// # Safety
///
/// `callbacks` must point to valid callbacks.
unsafe fn shared_callbacks_from_abi(
    callbacks: *const JSOffThreadCompilationCallbacks,
) -> Arc<SharedOffThreadCompilationCallbacks> {
    assert!(!callbacks.is_null(), "the embedder passes its callbacks");
    // SAFETY: The caller passes valid callbacks, which the runtime copies.
    let callbacks = unsafe { callbacks.read() };
    Arc::new(SharedOffThreadCompilationCallbacks(callbacks))
}

/// compile_remaining_functions_off_thread(Script&, source_code, callbacks): has the embedder's workers compile the
/// functions that the script's top-level code creates and that have not been compiled yet, so that their first calls
/// do not have to. Functions that start running in the meantime compile on the VM's thread as usual, and the functions
/// they create are then compiled off thread as well. The compiled functions are installed when the tasks the workers
/// post run on the VM's thread. The VM must outlive every task. Only the VM's thread may call this.
///
/// # Safety
///
/// `vm` and `script` must be live, and `callbacks` must point to valid callbacks.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_compile_remaining_functions_of_script_off_thread(
    vm: *mut JSVM,
    script: *mut JSScript,
    callbacks: *const JSOffThreadCompilationCallbacks,
) {
    // SAFETY: The caller passes a live VM and script, and valid callbacks, and the VM outlives the tasks.
    let (vm, script, callbacks) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi(script),
            shared_callbacks_from_abi(callbacks),
        )
    };
    compile_remaining_functions_of_executable_off_thread(vm, script.cached_executable(), callbacks);
}

/// compile_remaining_functions_off_thread(SourceTextModule&, source_code, callbacks): like
/// js_compile_remaining_functions_of_script_off_thread(), for the functions of a module, whose body is the async
/// function of a module with top-level await. Only the VM's thread may call this.
///
/// # Safety
///
/// `vm` and `module` must be live, `module` a Source Text Module Record, and `callbacks` must point to valid
/// callbacks.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_compile_remaining_functions_of_module_off_thread(
    vm: *mut JSVM,
    module: *mut JSModule,
    callbacks: *const JSOffThreadCompilationCallbacks,
) {
    // SAFETY: The caller passes a live VM and module, and valid callbacks, and the VM outlives the tasks.
    let (vm, module, callbacks) = unsafe {
        (
            vm_from_abi(vm),
            source_text_module_from_abi(module),
            shared_callbacks_from_abi(callbacks),
        )
    };
    let executable = module.cached_executable().or_else(|| {
        module
            .top_level_await_shared_data()
            .and_then(|shared_data| shared_data.executable())
    });
    if let Some(executable) = executable {
        compile_remaining_functions_of_executable_off_thread(vm, executable, callbacks);
    }
}

/// A token of a source text, with the trivia (whitespace and comments) before it, laid out like the FFIToken that C++
/// JS::SyntaxHighlighter reads. token_type and category are the values of JS::TokenType and JS::TokenCategory, and the
/// offsets and lengths count UTF-16 code units.
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
    use libjs_rust::token::TokenCategory;
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
    let (source, tokens) = unsafe { (code_units_of(source), &*tokens) };
    let append = tokens.append.expect("a token sink has an append function");
    for token in libjs_rust::tokenize::tokenize(&source) {
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
    let (source, sink) = unsafe { (code_units_of(source), &*positions) };
    let append = sink.append.expect("a position sink has an append function");
    let positions: Vec<JSPosition> = libjs_rust::breakpoint_positions::breakpoint_positions_for_source(
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
