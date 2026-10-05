/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The bytecode cache: blobs of programs compiled with all their functions, which an embedder stores keyed by a hash
//! of the source and loads again instead of compiling, as C++ LibJS/ScriptCompilation.h, JS::DecodedBytecodeCache and
//! the bytecode cache methods of JS::Script and JS::SourceTextModule offer it.
//!
//! Serializing, decoding and validating need no VM, so any thread may do them. A decoded cache is reference counted
//! without atomics, like JS::DecodedBytecodeCache: the thread that decoded it may hand it to the VM's thread while
//! nothing else holds a reference to it, and from then on only the VM's thread may use it.
//!
//! Scripts and modules materialized from a cache run their bytecode in place in the blob's bytes, which stay alive as
//! long as any of them, or the cache, does. A blob written for the C++ runtime is rejected like a corrupt one.

use core::ffi::c_void;
use std::rc::Rc;

use crate::bytecode::bytecode_cache::DecodedBytecodeCache;
use crate::embedding::abi_types::{
    JSByteSink, JSRealm, JSSourceCode, JSUtf16View, cell_from_abi, cell_into_abi, vm_from_abi,
};
use crate::embedding::compile::{
    JSCompiledProgram, JSProgramType, compiled_program_from_abi, program_type_from_abi, source_text_module_from_abi,
};
use crate::embedding::script::{JSParserErrorSink, JSScript, append_to_parser_error_sink, host_defined_slot_from_abi};
use crate::embedding::source_code::shared_source_code_from_abi;
use crate::layout::host_class::{JSModule, JSVM};
use crate::runtime::module::Module;
use crate::runtime::source_text_module::SourceTextModule;
use crate::script::Script;
use libjs_rust::bytecode_cache::{
    BytecodeCacheRuntime, ForeignBytecodeCacheBlobOwner, decode_blob, serialize_compiled_program,
};
use libjs_rust::compile::FunctionPrecompileMode;

/// A decoded bytecode cache blob: C++ JS::DecodedBytecodeCache.
pub struct JSDecodedBytecodeCache {
    _opaque: [u8; 0],
}

/// The size of the hash of the source that keys a blob.
pub const JS_BYTECODE_CACHE_SOURCE_HASH_SIZE: usize = 32;

/// The alignment that the bytes of a blob need for the runtime to run them in place. The runtime copies the bytes of a
/// blob that is not aligned this way.
pub const JS_BYTECODE_CACHE_BLOB_ALIGNMENT: usize = 8;

/// The embedder's handle on the bytes of a blob, such as a Core::ImmutableBytes. The runtime calls `release` with
/// `owner` once, when nothing decoded from the bytes needs them anymore: on the decoding thread when it rejects the
/// blob, and on the VM's thread otherwise, possibly while the heap sweeps the last executable that ran from the
/// bytes, so `release` must not use the VM or its heap.
#[repr(C)]
pub struct JSBytecodeCacheBlobOwner {
    pub owner: *mut c_void,
    pub release: Option<unsafe extern "C" fn(owner: *mut c_void)>,
}

/// # Safety
///
/// `source_hash` must point to `source_hash_length` readable bytes.
unsafe fn source_hash_from_abi<'a>(
    source_hash: *const u8,
    source_hash_length: usize,
) -> Option<&'a [u8; JS_BYTECODE_CACHE_SOURCE_HASH_SIZE]> {
    if source_hash.is_null() || source_hash_length != JS_BYTECODE_CACHE_SOURCE_HASH_SIZE {
        return None;
    }
    // SAFETY: The caller passes readable bytes, which are as many as a hash has.
    unsafe { core::slice::from_raw_parts(source_hash, source_hash_length) }
        .try_into()
        .ok()
}

fn cache_into_abi(cache: DecodedBytecodeCache) -> *const JSDecodedBytecodeCache {
    Rc::into_raw(Rc::new(cache)).cast()
}

/// # Safety
///
/// `cache` must be a live decoded cache, which stays alive for `'a`.
unsafe fn cache_from_abi<'a>(cache: *const JSDecodedBytecodeCache) -> &'a DecodedBytecodeCache {
    assert!(!cache.is_null(), "the embedder passes a decoded bytecode cache");
    // SAFETY: The caller passes a live cache, which is the contents of an Rc.
    unsafe { &*cache.cast::<DecodedBytecodeCache>() }
}

/// CompiledProgram::serialize_for_bytecode_cache(type, source_hash): writes the blob of a program that
/// js_compile_parsed_program_with_all_functions() compiled, for a program of `program_type` whose source hashes to the
/// `source_hash_length` bytes of `source_hash`, to `sink`. Returns false, having written nothing or part of the blob,
/// for a program compiled without all its functions or of another type, for a hash of the wrong size, and when the
/// sink stops taking bytes. Borrows the program. Any thread may call this.
///
/// # Safety
///
/// `compiled` must be a live compiled program, `source_hash` must point to `source_hash_length` readable bytes, and
/// `sink` must be a valid sink.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_bytecode_cache_serialize(
    compiled: *const JSCompiledProgram,
    program_type: JSProgramType,
    source_hash: *const u8,
    source_hash_length: usize,
    sink: *const JSByteSink,
) -> bool {
    // SAFETY: The caller passes a live program, readable hash bytes and a valid sink.
    let (compiled, source_hash, sink) = unsafe {
        (
            compiled_program_from_abi(compiled),
            source_hash_from_abi(source_hash, source_hash_length),
            sink.as_ref().expect("the embedder passes a sink"),
        )
    };
    let program_type = program_type_from_abi(program_type);
    let Some(source_hash) = source_hash else {
        return false;
    };
    if compiled.function_precompile_mode() != FunctionPrecompileMode::All || compiled.program_type() != program_type {
        return false;
    }
    let blob = serialize_compiled_program(compiled, program_type, source_hash, BytecodeCacheRuntime::Rust);
    let append = sink.append.expect("a byte sink has an append function");
    // SAFETY: The embedder's sink takes the bytes with the context it came with, and copies them.
    unsafe { append(sink.context, blob.as_ptr(), blob.len()) }
}

/// The bytes of a blob that was not aligned for running in place, copied to storage that is.
struct AlignedBlobBytes {
    words: Box<[u64]>,
}

const _: () = assert!(align_of::<u64>() == JS_BYTECODE_CACHE_BLOB_ALIGNMENT);

unsafe extern "C" fn release_aligned_blob_bytes(owner: *mut c_void) {
    // SAFETY: The owner is the boxed copy that decode() handed over, which is released once.
    drop(unsafe { Box::from_raw(owner.cast::<AlignedBlobBytes>()) });
}

/// Decodes the blob in `bytes`, whose owner it takes over, releasing the owner if it rejects the blob.
///
/// # Safety
///
/// `bytes` must point to `length` bytes that stay alive and unchanged until the owner is released, and `source_hash`
/// must point to `source_hash_length` readable bytes.
unsafe fn decode(
    bytes: *const u8,
    length: usize,
    program_type: JSProgramType,
    source_hash: *const u8,
    source_hash_length: usize,
    owner: JSBytecodeCacheBlobOwner,
) -> Option<DecodedBytecodeCache> {
    let release = owner.release.expect("the owner of a blob has a release function");
    let release_owner = || {
        // SAFETY: The embedder's release function takes the owner it handed over, once.
        unsafe { release(owner.owner) };
    };
    // SAFETY: The caller passes readable hash bytes.
    let Some(source_hash) = (unsafe { source_hash_from_abi(source_hash, source_hash_length) }) else {
        release_owner();
        return None;
    };
    if bytes.is_null() {
        release_owner();
        return None;
    }
    let program_type = program_type_from_abi(program_type);

    let (bytes, owner) = if bytes.addr().is_multiple_of(JS_BYTECODE_CACHE_BLOB_ALIGNMENT) {
        // SAFETY: The caller passes bytes that stay alive and unchanged until the owner is released.
        let bytes = unsafe { core::slice::from_raw_parts(bytes, length) };
        let owner = ForeignBytecodeCacheBlobOwner {
            owner: owner.owner,
            free_owner: release,
        };
        (bytes, owner)
    } else {
        let mut copy = Box::new(AlignedBlobBytes {
            words: vec![0; length.div_ceil(size_of::<u64>())].into_boxed_slice(),
        });
        // SAFETY: The caller passes `length` readable bytes, and the words have room for them.
        unsafe { core::ptr::copy_nonoverlapping(bytes, copy.words.as_mut_ptr().cast::<u8>(), length) };
        release_owner();
        // SAFETY: The copy, which the owner below releases, keeps its words alive and unchanged.
        let bytes = unsafe { core::slice::from_raw_parts(copy.words.as_ptr().cast::<u8>(), length) };
        let owner = ForeignBytecodeCacheBlobOwner {
            owner: Box::into_raw(copy).cast(),
            free_owner: release_aligned_blob_bytes,
        };
        (bytes, owner)
    };
    // SAFETY: The owner keeps the bytes alive and unchanged until it is released.
    let blob = unsafe { decode_blob(bytes, program_type, source_hash, BytecodeCacheRuntime::Rust, owner) }?;
    Some(DecodedBytecodeCache::new(blob))
}

/// DecodedBytecodeCache::create(bytes, type, source_hash): decodes the `length` bytes of a blob that
/// js_bytecode_cache_serialize() wrote for a program of `program_type` whose source hashes to the `source_hash_length`
/// bytes of `source_hash`. Validation against the source code happens when a script or module is created from it or
/// installs it. Returns the cache, whose one reference the caller owns, or null for a blob of another format version,
/// runtime, program type or source, and for a malformed one. Takes over `owner` either way. Any thread may call this.
///
/// # Safety
///
/// `bytes` must point to `length` bytes that stay alive and unchanged until the owner is released, `source_hash` must
/// point to `source_hash_length` readable bytes, and the owner's release function must accept the owner.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_bytecode_cache_decode(
    bytes: *const u8,
    length: usize,
    program_type: JSProgramType,
    source_hash: *const u8,
    source_hash_length: usize,
    owner: JSBytecodeCacheBlobOwner,
) -> *const JSDecodedBytecodeCache {
    // SAFETY: The caller's guarantees are decode()'s.
    unsafe { decode(bytes, length, program_type, source_hash, source_hash_length, owner) }
        .map_or(core::ptr::null(), cache_into_abi)
}

/// decode_and_validate_bytecode_cache(bytes, type, source_hash, source_length_in_code_units): decodes a blob like
/// js_bytecode_cache_decode(), then validates it against source code of `source_length_in_code_units` code units:
/// every source range, table index and the bytecode of every executable. Returns null for a blob that does not pass,
/// having released its owner. Any thread may call this.
///
/// # Safety
///
/// As for js_bytecode_cache_decode().
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_bytecode_cache_decode_and_validate(
    bytes: *const u8,
    length: usize,
    program_type: JSProgramType,
    source_hash: *const u8,
    source_hash_length: usize,
    source_length_in_code_units: usize,
    owner: JSBytecodeCacheBlobOwner,
) -> *const JSDecodedBytecodeCache {
    // SAFETY: The caller's guarantees are decode()'s.
    let Some(cache) = (unsafe { decode(bytes, length, program_type, source_hash, source_hash_length, owner) }) else {
        return core::ptr::null();
    };
    if !cache.validate(source_length_in_code_units) {
        return core::ptr::null();
    }
    cache_into_abi(cache)
}

/// Adds a reference to the cache, which the caller gives up with js_bytecode_cache_release(). Only the VM's thread may
/// call this.
///
/// # Safety
///
/// `cache` must be a live decoded cache.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_bytecode_cache_retain(cache: *const JSDecodedBytecodeCache) {
    assert!(!cache.is_null(), "the embedder passes a decoded bytecode cache");
    // SAFETY: The caller passes the contents of a live Rc.
    unsafe { Rc::increment_strong_count(cache.cast::<DecodedBytecodeCache>()) };
}

/// Gives up a reference to the cache. The blob's bytes stay alive while a script or module made from them does. Only
/// the thread that owns the cache, which is the VM's thread once a record was made from it, may call this.
///
/// # Safety
///
/// `cache` must be a live decoded cache, whose reference the caller gives up.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_bytecode_cache_release(cache: *const JSDecodedBytecodeCache) {
    assert!(!cache.is_null(), "the embedder passes a decoded bytecode cache");
    // SAFETY: The caller gives up a reference to the contents of a live Rc.
    unsafe { Rc::decrement_strong_count(cache.cast::<DecodedBytecodeCache>()) };
}

/// Script::create_from_bytecode_cache(cache, source_code, realm, filename, host_defined): the Script Record of a
/// classic script whose blob, decoded in `cache`, was compiled from the code of `source_code`, which reports the
/// filename of the source code. Its dynamic imports resolve against `filename`, and `host_defined` (null for none) is
/// its [[HostDefined]]. Returns the script, which the caller keeps alive, or null after appending one error to `errors`
/// (which may be null) if the blob does not match the source code or turns out to be malformed. Borrows the cache.
/// Only the VM's thread may call this.
///
/// # Safety
///
/// `vm`, `cache`, `source_code` and `realm` must be live, `filename` a valid view, `host_defined` null or a live cell,
/// and `errors` null or a valid sink.
#[unsafe(no_mangle)]
#[allow(
    clippy::too_many_arguments,
    reason = "C++ Script::create_from_bytecode_cache takes all of these"
)]
pub unsafe extern "C" fn js_bytecode_cache_create_script(
    vm: *mut JSVM,
    cache: *const JSDecodedBytecodeCache,
    source_code: *const JSSourceCode,
    realm: *mut JSRealm,
    filename: JSUtf16View,
    host_defined: *mut c_void,
    errors: *const JSParserErrorSink,
) -> *mut JSScript {
    // SAFETY: The caller passes a live VM, cache, source code, realm and host-defined cell, and a valid view.
    let (vm, cache, source_code, realm, filename, host_defined) = unsafe {
        (
            vm_from_abi(vm),
            cache_from_abi(cache),
            shared_source_code_from_abi(source_code),
            cell_from_abi(realm),
            filename.as_view().to_utf8(),
            host_defined_slot_from_abi(host_defined),
        )
    };
    match Script::create_from_bytecode_cache(vm, realm, cache, source_code, &filename, host_defined) {
        Ok(script) => cell_into_abi(script),
        Err(parser_errors) => {
            // SAFETY: The caller passes null or a valid sink.
            unsafe { append_to_parser_error_sink(errors, &parser_errors) };
            core::ptr::null_mut()
        }
    }
}

/// SourceTextModule::parse_from_bytecode_cache(cache, source_code, realm, filename, host_defined): like
/// js_bytecode_cache_create_script(), but for a module, whose imports resolve against `filename`. Returns the Source
/// Text Module Record, or null after appending one error to `errors`. Only the VM's thread may call this.
///
/// # Safety
///
/// As for js_bytecode_cache_create_script().
#[unsafe(no_mangle)]
#[allow(
    clippy::too_many_arguments,
    reason = "C++ SourceTextModule::parse_from_bytecode_cache takes all of these"
)]
pub unsafe extern "C" fn js_bytecode_cache_create_module(
    vm: *mut JSVM,
    cache: *const JSDecodedBytecodeCache,
    source_code: *const JSSourceCode,
    realm: *mut JSRealm,
    filename: JSUtf16View,
    host_defined: *mut c_void,
    errors: *const JSParserErrorSink,
) -> *mut JSModule {
    // SAFETY: The caller passes a live VM, cache, source code, realm and host-defined cell, and a valid view.
    let (vm, cache, source_code, realm, filename, host_defined) = unsafe {
        (
            vm_from_abi(vm),
            cache_from_abi(cache),
            shared_source_code_from_abi(source_code),
            cell_from_abi(realm),
            filename.as_view().to_utf8(),
            host_defined_slot_from_abi(host_defined),
        )
    };
    match SourceTextModule::create_from_bytecode_cache(vm, realm, &filename, cache, source_code, host_defined) {
        Ok(module) => cell_into_abi(module.upcast::<Module>()),
        Err(parser_errors) => {
            // SAFETY: The caller passes null or a valid sink.
            unsafe { append_to_parser_error_sink(errors, &parser_errors) };
            core::ptr::null_mut()
        }
    }
}

/// Script::begin_bytecode_cache_generation(): marks the script as one whose bytecode cache is being generated, until
/// js_bytecode_cache_script_install_generated() installs it or js_bytecode_cache_script_finish_generation_without_install()
/// gives up. Panics unless the script is in the SOURCE or HEAP_BYTECODE state. Only the VM's thread may call this.
///
/// # Safety
///
/// `vm` and `script` must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_bytecode_cache_script_begin_generation(vm: *mut JSVM, script: *mut JSScript) {
    // SAFETY: The caller passes a live VM and script.
    let (vm, script) = unsafe { (vm_from_abi(vm), cell_from_abi(script)) };
    script.begin_bytecode_cache_generation(vm);
}

/// Script::finish_bytecode_cache_generation_without_install(). Panics unless a bytecode cache is being generated for
/// the script. Only the VM's thread may call this.
///
/// # Safety
///
/// `vm` and `script` must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_bytecode_cache_script_finish_generation_without_install(
    vm: *mut JSVM,
    script: *mut JSScript,
) {
    // SAFETY: The caller passes a live VM and script.
    let (vm, script) = unsafe { (vm_from_abi(vm), cell_from_abi(script)) };
    script.finish_bytecode_cache_generation_without_install(vm);
}

/// Script::install_generated_bytecode_cache(cache, source_code): installs the cache that was generated for the script,
/// so that from now on the script and its functions run from the blob, compiled from the code of `source_code`.
/// Functions that already ran get executables from the blob, which take over their inline caches; the others compile
/// from the blob on their first call. Panics unless a bytecode cache is being generated for the script, and if the
/// blob does not match it. Only the VM's thread may call this.
///
/// # Safety
///
/// `vm`, `script`, `cache` and `source_code` must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_bytecode_cache_script_install_generated(
    vm: *mut JSVM,
    script: *mut JSScript,
    cache: *const JSDecodedBytecodeCache,
    source_code: *const JSSourceCode,
) {
    // SAFETY: The caller passes a live VM, script, cache and source code.
    let (vm, script, cache, source_code) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi(script),
            cache_from_abi(cache),
            shared_source_code_from_abi(source_code),
        )
    };
    script.install_generated_bytecode_cache(vm, cache, &source_code);
}

/// SourceTextModule::begin_bytecode_cache_generation(): like js_bytecode_cache_script_begin_generation(). Only the VM's
/// thread may call this.
///
/// # Safety
///
/// `vm` must be live, and `module` a live Source Text Module Record.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_bytecode_cache_module_begin_generation(vm: *mut JSVM, module: *mut JSModule) {
    // SAFETY: The caller passes a live VM and Source Text Module Record.
    let (vm, module) = unsafe { (vm_from_abi(vm), source_text_module_from_abi(module)) };
    module.begin_bytecode_cache_generation(vm);
}

/// SourceTextModule::finish_bytecode_cache_generation_without_install(): like
/// js_bytecode_cache_script_finish_generation_without_install(). Only the VM's thread may call this.
///
/// # Safety
///
/// `vm` must be live, and `module` a live Source Text Module Record.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_bytecode_cache_module_finish_generation_without_install(
    vm: *mut JSVM,
    module: *mut JSModule,
) {
    // SAFETY: The caller passes a live VM and Source Text Module Record.
    let (vm, module) = unsafe { (vm_from_abi(vm), source_text_module_from_abi(module)) };
    module.finish_bytecode_cache_generation_without_install(vm);
}

/// SourceTextModule::install_generated_bytecode_cache(cache, source_code): like
/// js_bytecode_cache_script_install_generated(), for a module. Only the VM's thread may call this.
///
/// # Safety
///
/// `vm`, `cache` and `source_code` must be live, and `module` a live Source Text Module Record.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_bytecode_cache_module_install_generated(
    vm: *mut JSVM,
    module: *mut JSModule,
    cache: *const JSDecodedBytecodeCache,
    source_code: *const JSSourceCode,
) {
    // SAFETY: The caller passes a live VM, Source Text Module Record, cache and source code.
    let (vm, module, cache, source_code) = unsafe {
        (
            vm_from_abi(vm),
            source_text_module_from_abi(module),
            cache_from_abi(cache),
            shared_source_code_from_abi(source_code),
        )
    };
    module.install_generated_bytecode_cache(vm, cache, &source_code);
}
