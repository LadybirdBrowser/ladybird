/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The embedding API of the runtime for embedders written in Rust: what a crate that implements host objects and
//! native functions on this runtime uses, as LibJS's C++ API is what a C++ embedder uses. Code outside LibJS reaches
//! the runtime only through this module (Meta/Linters/check_libjs_rust_api.py), so the rest of the crate stays the
//! runtime's own business.

pub use crate::embedding::host::host_object::{HostObject, host_class_of, host_data_of, is_host_instance_of};
pub use crate::gc::class::ExternalMemorySize;
pub use crate::gc::embedder_cell::{EmbedderCell, EmbedderClass, EmbedderValue};
pub use crate::gc::foreign::ForeignCellSlot;
pub use crate::gc::gc_ref_cell::{GcRefCell, GcRefMut};
pub use crate::gc::root::{MarkedVec, Root};
pub use crate::gc::visitor::{Trace, Visitor};
pub use crate::gc::weak::GcWeak;
pub use crate::interpreter::vm::{TypeErrorRealmScope, Vm};
pub use crate::layout::cell::Gc;
pub use crate::layout::function_object::FunctionObject;
pub use crate::layout::host_class::JSHostClass;
pub use crate::layout::value::Value;
pub use crate::layout_forward::{RawNativeFunctionPointer, RawNativeFunctionResult};
pub use crate::raw_native;
pub use crate::runtime::abstract_operations::{call, construct, get_function_realm};
pub use crate::runtime::array::Array;
pub use crate::runtime::array_buffer::{
    ArrayBuffer, DataBlock, Order as ArrayBufferOrder, Shared as ArrayBufferShared, clone_array_buffer,
    detach_array_buffer,
};
pub use crate::runtime::completion::{Must, Throw, ThrowCompletionOr};
pub use crate::runtime::data_view::{
    DataView, DataViewWithBufferWitness, get_view_byte_length, is_view_out_of_bounds,
    make_data_view_with_buffer_witness_record,
};
pub use crate::runtime::error::ErrorKind;
pub use crate::runtime::error_types::ErrorType;
pub use crate::runtime::intrinsics::Intrinsics;
pub use crate::runtime::iterator::{
    IteratorHint, IteratorRecord, create_iterator_result_object, get_iterator, get_iterator_from_method_impl,
    iterator_complete, iterator_next, iterator_step_value, iterator_value,
};
pub use crate::runtime::native_function::{NativeFunction, RawNativeFunction};
pub use crate::runtime::object::Object;
pub use crate::runtime::primitive_string::PrimitiveString;
pub use crate::runtime::promise::{Promise, PromiseState, RejectionOperation};
pub use crate::runtime::promise_capability::{PromiseCapability, new_promise_capability};
pub use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
pub use crate::runtime::property_key::PropertyKey;
pub use crate::runtime::realm::Realm;
pub use crate::runtime::typed_array::{
    Kind as TypedArrayKind, TypedArrayBase, TypedArrayWithBufferWitness, create_typed_array_on_buffer,
    is_typed_array_out_of_bounds, make_typed_array_with_buffer_witness_record, typed_array_byte_length,
};
pub use crate::runtime::value::{number_to_string, same_value};
pub use crate::utf16::{Utf16Display, to_well_formed};
/// Derives Trace for a type of the embedder, which marks the type `#[gc(crate = libjs_rust)]`.
pub use libjs_runtime_macros::Trace;
