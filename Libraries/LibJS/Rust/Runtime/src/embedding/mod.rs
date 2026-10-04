/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The embedding ABI: the C interface through which an embedder such as LibWeb runs JavaScript on this runtime.
//!
//! build.rs generates two headers for it. cbindgen writes LibJS/Embedding/ABI.h from this module alone, and
//! generate/layout_header.rs writes LibJS/Embedding/Layout.h, the offsets and constants of the runtime's structures
//! that an embedder reads directly, from crate::layout.
//!
//! The rules of the boundary:
//! - Every `extern "C"` function the ABI exports lives under this module, and so does every call through a function
//!   pointer an embedder supplied: host class hooks, host hooks, sinks, and console and debugger callbacks. The rest
//!   of the crate reaches an embedder only through Rust functions defined here.
//! - Exported functions are named js_<area>_<operation>, where the area is the file they live in, as in
//!   js_object_get in object.rs or js_host_object_create in host/host_object.rs.
//! - C types are named JS<Name>. The vocabulary types and the host class tables come from LibJS/HostObjectABI.h,
//!   which crate::layout::host_class mirrors, and ABI.h includes that header instead of declaring them again.

pub mod abi_types;
pub mod array;
pub mod array_buffer;
pub mod bigint;
pub mod bytecode_cache;
pub mod collections;
pub mod compile;
pub mod console;
pub mod date;
pub mod debugger;
pub mod environment;
pub mod error;
pub mod execution_context;
pub mod function;
pub mod hooks;
pub mod host;
pub mod iterator;
pub mod json;
pub mod module;
pub mod object;
pub mod promise;
pub mod realm;
pub mod regexp;
pub mod script;
pub mod source_code;
pub mod string;
pub mod symbol;
pub mod typed_array;
pub mod value;
pub mod vm;
pub mod weak;

mod layout_header_static_assertions {
    include!(concat!(env!("OUT_DIR"), "/layout_header_static_assertions.rs"));
}
