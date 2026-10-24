/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The embedding API of the runtime for embedders written in Rust: what a crate that implements host objects and
//! native functions on this runtime uses, as LibJS's C++ API is what a C++ embedder uses. Code outside LibJS reaches
//! the runtime only through this module (Meta/Linters/check_libjs_rust_api.py), so the rest of the crate stays the
//! runtime's own business.

pub use crate::embedding::abi_types::{completion_into_abi, object_from_abi, vm_from_abi};
pub use crate::embedding::host::host_function::HostFunction;
pub use crate::embedding::host::host_module::{HostModule, host_class_of as host_module_class_of};
pub use crate::embedding::host::host_object::{HostObject, host_class_of, host_data_of, is_host_instance_of};
pub use crate::gc::class::ExternalMemorySize;
pub use crate::gc::defer_gc::DeferGc;
pub use crate::gc::embedder_cell::{EmbedderCell, EmbedderClass, EmbedderValue};
pub use crate::gc::foreign::ForeignCellSlot;
pub use crate::gc::gc_ref_cell::{GcRefCell, GcRefMut};
pub use crate::gc::root::{MarkedVec, Root};
pub use crate::gc::visitor::{Trace, Visitor};
pub use crate::gc::weak::GcWeak;
pub use crate::interpreter::vm::{TypeErrorRealmScope, Vm};
pub use crate::layout::cell::{CellHeader, CellKind, Gc};
pub use crate::layout::function_object::FunctionObject;
pub use crate::layout::host_class::{
    JS_HOST_CLASS_FUNCTION, JS_HOST_CLASS_HAS_CONSTRUCTOR, JS_HOST_CLASS_MODULE,
    JS_HOST_CLASS_NOT_CACHEABLE_FOR_PROPERTY_ABSENCE, JS_HOST_CLASS_OBJECT, JS_RESOLVED_BINDING_BINDING_NAME,
    JS_RESOLVED_BINDING_NULL, JSCompletion, JSGetCacheMetadata, JSHostClass, JSHostFunctionHooks, JSHostModuleHooks,
    JSHostObjectHooks, JSModule, JSObject, JSPromiseCapability, JSPropertyDescriptor, JSPropertyKey, JSResolvedBinding,
    JSSetCacheMetadata, JSStringSink, JSVM, JSValue, JSValueSink,
};
pub use crate::layout::value::Value;
pub use crate::layout_forward::{RawNativeFunctionPointer, RawNativeFunctionResult};
pub use crate::raw_native;
pub use crate::runtime::abstract_operations::{call, construct, get_function_realm};
pub use crate::runtime::agent::agent_can_suspend;
pub use crate::runtime::array::Array;
pub use crate::runtime::array_buffer::{
    ArrayBuffer, DataBlock, DataBlockStorage, ExternalPrimitiveStorage, Order as ArrayBufferOrder,
    Shared as ArrayBufferShared, clone_array_buffer, detach_array_buffer,
};
pub use crate::runtime::big_int::BigInt;
pub use crate::runtime::completion::{Must, Throw, ThrowCompletionOr};
pub use crate::runtime::data_view::{
    DataView, DataViewWithBufferWitness, get_view_byte_length, is_view_out_of_bounds,
    make_data_view_with_buffer_witness_record,
};
pub use crate::runtime::environment::InitializeBindingHint;
pub use crate::runtime::error::{Error, ErrorKind};
pub use crate::runtime::error_data::CompactTraceback;
pub use crate::runtime::error_types::ErrorType;
pub use crate::runtime::intrinsics::Intrinsics;
pub use crate::runtime::iterator::{
    IteratorHint, IteratorRecord, create_iterator_result_object, get_iterator, get_iterator_from_method_impl,
    iterator_complete, iterator_next, iterator_step_value, iterator_to_list, iterator_value,
};
pub use crate::runtime::module::Module;
pub use crate::runtime::module_environment::ModuleEnvironment;
pub use crate::runtime::module_request::ModuleRequest;
pub use crate::runtime::native_function::{NativeFunction, RawNativeFunction};
pub use crate::runtime::object::{IntegrityLevel, Object};
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
