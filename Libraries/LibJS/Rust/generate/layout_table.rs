/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Writes the layout file that flapc compiles the interpreter against. Every offset and size it prints is also written
//! out as a static assertion that the crate compiles, since the offsets are computed here with stand-ins for the types
//! outside the layout module.

use std::fmt::Write;

use crate::layout::accessor::*;
use crate::layout::buffer::*;
use crate::layout::environment::*;
use crate::layout::executable::*;
use crate::layout::execution_context::*;
use crate::layout::function_object::*;
use crate::layout::object::*;
use crate::layout::primitive_string::*;
use crate::layout::property_lookup_cache::*;
use crate::layout::realm::*;
use crate::layout::shape::*;
use crate::layout::value::*;
use crate::layout::vm::*;
use crate::layout_forward::Utf16StringDataHeader;
use libjs_abi::{Builtin, PutKind, register, value as nan_box};

/// The imports the static assertions need to name the same types as the table.
const STATIC_ASSERTIONS_PREAMBLE: &str = "\
use crate::layout::accessor::*;
use crate::layout::buffer::*;
use crate::layout::environment::*;
use crate::layout::executable::*;
use crate::layout::execution_context::*;
use crate::layout::function_object::*;
use crate::layout::object::*;
use crate::layout::primitive_string::*;
use crate::layout::property_lookup_cache::*;
use crate::layout::realm::*;
use crate::layout::shape::*;
use crate::layout::value::*;
use crate::layout::vm::*;
use crate::layout_forward::Utf16StringDataHeader;
";

/// Build configuration that the layout depends on but Rust's own cfg cannot see.
pub struct LayoutConfiguration {
    pub heap_region_offset_mask: u64,
    pub primitive_storage_cage_offset_mask: u64,
    pub vm_stack_space_limit: u64,
}

pub struct LayoutWriter {
    pub text: String,
    pub static_assertions: String,
}

impl LayoutWriter {
    fn new() -> Self {
        Self {
            text: String::new(),
            static_assertions: STATIC_ASSERTIONS_PREAMBLE.to_string(),
        }
    }

    fn line(&mut self, line: &str) {
        self.text.push_str(line);
        self.text.push('\n');
    }

    fn section(&mut self, name: &str) {
        let _ = write!(self.text, "\n# {name}\n");
    }

    fn constant(&mut self, name: &str, value: impl std::fmt::Display) {
        let _ = writeln!(self.text, "const {name} = {value}");
    }

    fn hex_constant(&mut self, name: &str, value: u64) {
        let _ = writeln!(self.text, "const {name} = 0x{value:X}");
    }

    fn field(&mut self, field: &str, flap_type: &str, offset: &str, storage: &str, representation: &str) {
        let _ = writeln!(
            self.text,
            "field {field} {flap_type} {offset} {storage} {representation}"
        );
    }

    fn paired_field(
        &mut self,
        field: &str,
        flap_type: &str,
        offset: &str,
        storage: &str,
        representation: &str,
        pair: &str,
    ) {
        let _ = writeln!(
            self.text,
            "field {field} {flap_type} {offset} {storage} {representation} {pair}"
        );
    }

    fn assert_offset(&mut self, type_path: &str, field_path: &str, offset: usize) {
        let _ = writeln!(
            self.static_assertions,
            "const _: () = assert!(core::mem::offset_of!({type_path}, {field_path}) == {offset});"
        );
    }

    fn assert_size(&mut self, type_path: &str, size: usize) {
        let _ = writeln!(
            self.static_assertions,
            "const _: () = assert!(size_of::<{type_path}>() == {size});"
        );
    }
}

fn size_of_field<T, F>(_: fn(&T) -> &F) -> usize {
    size_of::<F>()
}

macro_rules! offset {
    ($writer:expr, $name:literal, $type:path, $($field:tt)+) => {{
        let offset = core::mem::offset_of!($type, $($field)+);
        $writer.constant($name, offset);
        $writer.assert_offset(stringify!($type), stringify!($($field)+), offset);
        offset
    }};
}

macro_rules! size {
    ($writer:expr, $name:literal, $type:path) => {{
        let size = size_of::<$type>();
        $writer.constant($name, size);
        $writer.assert_size(stringify!($type), size);
        size
    }};
}

/// Emits a field the interpreter accesses, checking that its Rust type has the size the interpreter accesses it with.
macro_rules! field {
    ($writer:expr, $name:literal, $field:literal, $flap_type:literal, $type:path, $member:ident, $size:expr, $storage:literal, $representation:literal) => {{
        assert_eq!(
            size_of_field(|object: &$type| &object.$member),
            $size,
            "size of {}",
            $field
        );
        offset!($writer, $name, $type, $member);
        $writer.field($field, $flap_type, $name, $storage, $representation);
    }};
    ($writer:expr, $name:literal, $field:literal, $flap_type:literal, $type:path, $member:ident, $size:expr, $storage:literal, $representation:literal, $pair:literal) => {{
        assert_eq!(
            size_of_field(|object: &$type| &object.$member),
            $size,
            "size of {}",
            $field
        );
        offset!($writer, $name, $type, $member);
        $writer.paired_field($field, $flap_type, $name, $storage, $representation, $pair);
    }};
}

#[rustfmt::skip]
pub fn generate(configuration: &LayoutConfiguration) -> LayoutWriter {
    let mut w = LayoutWriter::new();
    let value_size = size_of::<Value>();

    w.line("# Generated by the libjs_rust build script -- DO NOT EDIT");

    w.section("Object layout");
    field!(w, "OBJECT_SHAPE", "Object.shape", "Shape", Object, shape, 8, "nonnull", "cell");
    field!(w, "OBJECT_NAMED_PROPERTIES", "Object.named_properties", "PropertyStorage", Object, named_properties, 8, "nonnull", "scalar");
    field!(w, "OBJECT_INDEXED_ELEMENTS", "Object.indexed_elements", "IndexedElements", Object, indexed_elements, 8, "nullable", "scalar");
    field!(w, "OBJECT_INDEXED_STORAGE_KIND", "Object.indexed_storage_kind", "u8", Object, indexed_storage_kind, 1, "nullable", "scalar");
    field!(w, "OBJECT_INDEXED_ARRAY_LIKE_SIZE", "Object.indexed_array_like_size", "u32", Object, indexed_array_like_size, 4, "nullable", "scalar");
    size!(w, "OBJECT_SIZE", Object);

    w.section("Accessor layout");
    field!(w, "ACCESSOR_GETTER", "Accessor.getter", "FunctionObject", Accessor, getter, 8, "nullable", "cell");

    w.section("Object flags");
    field!(w, "OBJECT_FLAGS", "Object.flags", "u16", Object, flags, 2, "nullable", "scalar");
    w.constant("OBJECT_FLAG_HAS_MAGICAL_LENGTH", object_flag::HAS_MAGICAL_LENGTH_PROPERTY);
    w.constant("OBJECT_FLAG_MAY_INTERFERE", object_flag::MAY_INTERFERE_WITH_INDEXED_PROPERTY_ACCESS);
    w.constant("OBJECT_FLAG_IS_TYPED_ARRAY", object_flag::IS_TYPED_ARRAY);
    w.constant("OBJECT_FLAG_IS_FUNCTION", object_flag::IS_FUNCTION);
    w.constant("OBJECT_FLAG_IS_ECMASCRIPT_FUNCTION_OBJECT", object_flag::IS_ECMASCRIPT_FUNCTION_OBJECT);
    w.constant("OBJECT_FLAG_IS_RAW_NATIVE_FUNCTION", object_flag::IS_RAW_NATIVE_FUNCTION);
    w.constant("OBJECT_FLAG_IS_DIRECT_GETTER_FUNCTION", object_flag::IS_DIRECT_GETTER_FUNCTION);
    w.constant("OBJECT_FLAG_IS_PLATFORM_OBJECT", object_flag::IS_PLATFORM_OBJECT);
    w.constant("OBJECT_FLAG_IS_GLOBAL_OBJECT", object_flag::IS_GLOBAL_OBJECT);
    w.constant("OBJECT_FLAG_IS_HTMLDDA", object_flag::IS_HTMLDDA);

    w.section("Shape layout");
    field!(w, "SHAPE_REALM", "Shape.realm", "Realm", Shape, realm, 8, "nonnull", "cell");
    offset!(w, "SHAPE_PROTOTYPE", Shape, prototype);
    field!(w, "SHAPE_DICTIONARY_GENERATION", "Shape.dictionary_generation", "u32", Shape, dictionary_generation, 4, "nullable", "scalar");
    w.constant("SHAPE_SIZE", size_of::<Shape>());

    w.section("PropertyLookupCache layout");
    offset!(w, "PROPERTY_LOOKUP_CACHE_DATA", PropertyLookupCache, data);
    w.hex_constant("PROPERTY_LOOKUP_CACHE_DATA_POINTER_MASK", !(PROPERTY_LOOKUP_CACHE_DATA_TAG_MASK as u64));
    w.constant("PROPERTY_LOOKUP_CACHE_KEYED_GENERIC", PROPERTY_LOOKUP_CACHE_KEYED_GENERIC_DATA);
    size!(w, "PROPERTY_LOOKUP_CACHE_SIZE", PropertyLookupCache);

    w.section("PropertyLookupCache::Entry layout");
    field!(w, "PROPERTY_LOOKUP_CACHE_ENTRY_TYPE", "PropertyLookupCache.entry_type", "u32", PropertyLookupCacheEntry, entry_type, 4, "nullable", "scalar");
    w.constant("PROPERTY_LOOKUP_CACHE_ENTRY_TYPE_GET_MISSING_PROPERTY", PropertyLookupCacheEntryType::GetMissingProperty as u32);
    field!(w, "PROPERTY_LOOKUP_CACHE_ENTRY_PROPERTY_OFFSET", "PropertyLookupCache.property_offset", "u32", PropertyLookupCacheEntry, property_offset, 4, "nullable", "scalar", "cache_details");
    field!(w, "PROPERTY_LOOKUP_CACHE_ENTRY_DICTIONARY_GENERATION", "PropertyLookupCache.shape_dictionary_generation", "u32", PropertyLookupCacheEntry, shape_dictionary_generation, 4, "nullable", "scalar", "cache_details");
    field!(w, "PROPERTY_LOOKUP_CACHE_ENTRY_DIRECT_GETTER_VALIDATED", "PropertyLookupCache.direct_getter_validated", "bool", PropertyLookupCacheEntry, direct_getter_validated, 1, "nullable", "scalar");
    field!(w, "PROPERTY_LOOKUP_CACHE_ENTRY_WRITES_DATA_PROPERTY", "PropertyLookupCache.writes_data_property", "bool", PropertyLookupCacheEntry, writes_data_property, 1, "nullable", "scalar");
    offset!(w, "PROPERTY_LOOKUP_CACHE_ENTRY_FROM_SHAPE", PropertyLookupCacheEntry, from_shape);
    field!(w, "PROPERTY_LOOKUP_CACHE_ENTRY_SHAPE", "PropertyLookupCache.shape", "Shape", PropertyLookupCacheEntry, shape, 8, "nullable", "cell", "cache_target");
    field!(w, "PROPERTY_LOOKUP_CACHE_ENTRY_PROTOTYPE", "PropertyLookupCache.prototype", "Object", PropertyLookupCacheEntry, prototype, 8, "nullable", "cell", "cache_target");
    field!(w, "PROPERTY_LOOKUP_CACHE_ENTRY_PROTOTYPE_CHAIN_VALIDITY", "PropertyLookupCache.prototype_chain_validity", "PrototypeChainValidity", PropertyLookupCacheEntry, prototype_chain_validity, 8, "nullable", "cell");
    field!(w, "PROPERTY_LOOKUP_CACHE_ENTRY_KEY", "PropertyLookupCache.key", "Value", PropertyLookupCacheEntry, key, 8, "nullable", "scalar");
    size!(w, "PROPERTY_LOOKUP_CACHE_ENTRY_SIZE", PropertyLookupCacheEntry);

    w.section("ObjectPropertyIteratorCacheData layout");
    offset!(w, "OBJECT_PROPERTY_ITERATOR_CACHE_DATA_PROPERTIES", ObjectPropertyIteratorCacheData, storage);
    offset!(w, "OBJECT_PROPERTY_ITERATOR_CACHE_DATA_PROPERTY_VALUES", ObjectPropertyIteratorCacheData, property_values);
    field!(w, "OBJECT_PROPERTY_ITERATOR_CACHE_DATA_SHAPE", "ObjectPropertyIteratorCacheData.shape", "Shape", ObjectPropertyIteratorCacheData, shape, 8, "nullable", "cell");
    field!(w, "OBJECT_PROPERTY_ITERATOR_CACHE_DATA_PROTOTYPE_CHAIN_VALIDITY", "ObjectPropertyIteratorCacheData.prototype_chain_validity", "PrototypeChainValidity", ObjectPropertyIteratorCacheData, prototype_chain_validity, 8, "nullable", "cell");
    field!(w, "OBJECT_PROPERTY_ITERATOR_CACHE_DATA_INDEXED_PROPERTY_COUNT", "ObjectPropertyIteratorCacheData.indexed_property_count", "u32", ObjectPropertyIteratorCacheData, indexed_property_count, 4, "nullable", "scalar");
    field!(w, "OBJECT_PROPERTY_ITERATOR_CACHE_DATA_SHAPE_DICTIONARY_GENERATION", "ObjectPropertyIteratorCacheData.shape_dictionary_generation", "u32", ObjectPropertyIteratorCacheData, shape_dictionary_generation, 4, "nullable", "scalar");
    field!(w, "OBJECT_PROPERTY_ITERATOR_CACHE_DATA_SHAPE_IS_DICTIONARY", "ObjectPropertyIteratorCacheData.shape_is_dictionary", "bool", ObjectPropertyIteratorCacheData, shape_is_dictionary, 1, "nullable", "scalar");
    field!(w, "OBJECT_PROPERTY_ITERATOR_CACHE_DATA_FAST_PATH", "ObjectPropertyIteratorCacheData.fast_path", "u8", ObjectPropertyIteratorCacheData, fast_path, 1, "nullable", "scalar");

    w.section("ObjectPropertyIteratorCache layout");
    field!(w, "OBJECT_PROPERTY_ITERATOR_CACHE_DATA_PTR", "ObjectPropertyIteratorCache.data", "ObjectPropertyIteratorCacheData", ObjectPropertyIteratorCache, data, 8, "nullable", "cell");

    w.constant("SLOW_PATH_CONTINUATION_BIT", 32);

    w.section("Executable layout");
    offset!(w, "EXECUTABLE_CONSTANTS", ExecutableHead, constants);
    offset!(w, "EXECUTABLE_PROPERTY_LOOKUP_CACHES", ExecutableHead, property_lookup_caches);
    offset!(w, "EXECUTABLE_GLOBAL_VARIABLE_CACHES", ExecutableHead, global_variable_caches);
    offset!(w, "EXECUTABLE_ENVIRONMENT_COORDINATE_CACHES", ExecutableHead, environment_coordinate_caches);
    field!(w, "EXECUTABLE_REGISTERS_AND_LOCALS_COUNT", "Executable.registers_and_locals_count", "u32", ExecutableHead, registers_and_locals_count, 4, "nullable", "scalar", "slot_counts");
    field!(w, "EXECUTABLE_REGISTERS_AND_LOCALS_AND_CONSTANTS_COUNT", "Executable.registers_and_locals_and_constants_count", "u32", ExecutableHead, registers_and_locals_and_constants_count, 4, "nullable", "scalar", "slot_counts");
    field!(w, "EXECUTABLE_ASM_CONSTANTS_SIZE", "Executable.asm_constants_size", "u64", ExecutableHead, asm_constants_size, 8, "nullable", "scalar", "constants");
    field!(w, "EXECUTABLE_ASM_CONSTANTS_DATA", "Executable.asm_constants_data", "Sequence<Value>", ExecutableHead, asm_constants_data, 8, "nullable", "scalar", "constants");

    w.section("ExecutionContext layout");
    assert_eq!(align_of::<ExecutionContext>(), value_size);
    field!(w, "EXECUTION_CONTEXT_FUNCTION", "ExecutionContext.function", "Object", ExecutionContext, function, 8, "nullable", "cell", "function_and_realm");
    field!(w, "EXECUTION_CONTEXT_REALM", "ExecutionContext.realm", "Realm", ExecutionContext, realm, 8, "nullable", "cell", "function_and_realm");
    field!(w, "EXECUTION_CONTEXT_SCRIPT_OR_MODULE", "ExecutionContext.script_or_module", "ScriptOrModule", ExecutionContext, script_or_module, 16, "nullable", "scalar");
    field!(w, "EXECUTION_CONTEXT_LEXICAL_ENVIRONMENT", "ExecutionContext.lexical_environment", "Environment", ExecutionContext, lexical_environment, 8, "nullable", "cell", "environments");
    field!(w, "EXECUTION_CONTEXT_VARIABLE_ENVIRONMENT", "ExecutionContext.variable_environment", "Environment", ExecutionContext, variable_environment, 8, "nullable", "cell", "environments");
    field!(w, "EXECUTION_CONTEXT_PRIVATE_ENVIRONMENT", "ExecutionContext.private_environment", "PrivateEnvironment", ExecutionContext, private_environment, 8, "nullable", "cell");
    field!(w, "EXECUTION_CONTEXT_SKIP_WHEN_DETERMINING_INCUMBENT_COUNTER", "ExecutionContext.skip_when_determining_incumbent_counter", "u32", ExecutionContext, skip_when_determining_incumbent_counter, 4, "nullable", "scalar");
    field!(w, "EXECUTION_CONTEXT_YIELD_CONTINUATION", "ExecutionContext.yield_continuation", "u32", ExecutionContext, yield_continuation, 4, "nullable", "scalar");
    field!(w, "EXECUTION_CONTEXT_YIELD_IS_AWAIT", "ExecutionContext.yield_is_await", "bool", ExecutionContext, yield_is_await, 1, "nullable", "scalar");
    field!(w, "EXECUTION_CONTEXT_YIELD_VALUE_IS_ITERATOR_RESULT", "ExecutionContext.yield_value_is_iterator_result", "bool", ExecutionContext, yield_value_is_iterator_result, 1, "nullable", "scalar");
    field!(w, "EXECUTION_CONTEXT_CALLER_IS_CONSTRUCT", "ExecutionContext.caller_is_construct", "bool", ExecutionContext, caller_is_construct, 1, "nullable", "scalar");
    field!(w, "EXECUTION_CONTEXT_FRAME_INITIALIZED", "ExecutionContext.frame_initialized", "bool", ExecutionContext, frame_initialized, 1, "nullable", "scalar");
    field!(w, "EXECUTION_CONTEXT_THIS_VALUE", "ExecutionContext.this_value", "Value", ExecutionContext, this_value, 8, "nullable", "scalar", "this_and_executable");
    field!(w, "EXECUTION_CONTEXT_EXECUTABLE", "ExecutionContext.executable", "Executable", ExecutionContext, executable, 8, "nullable", "cell", "this_and_executable");
    field!(w, "EXECUTION_CONTEXT_CALLER_FRAME", "ExecutionContext.caller_frame", "ExecutionContext", ExecutionContext, caller_frame, 8, "nullable", "scalar");
    field!(w, "EXECUTION_CONTEXT_PASSED_ARGUMENT_COUNT", "ExecutionContext.passed_argument_count", "u32", ExecutionContext, passed_argument_count, 4, "nullable", "scalar");
    field!(w, "EXECUTION_CONTEXT_CALLER_RETURN_PC", "ExecutionContext.caller_return_pc", "u32", ExecutionContext, caller_return_pc, 4, "nullable", "scalar", "caller_return");
    field!(w, "EXECUTION_CONTEXT_CALLER_DST_RAW", "ExecutionContext.caller_dst_raw", "u32", ExecutionContext, caller_dst_raw, 4, "nullable", "scalar", "caller_return");
    field!(w, "EXECUTION_CONTEXT_PROGRAM_COUNTER", "ExecutionContext.program_counter", "u32", ExecutionContext, program_counter, 4, "nullable", "scalar");
    field!(w, "EXECUTION_CONTEXT_FRAME_ID", "ExecutionContext.frame_id", "u64", ExecutionContext, frame_id, 8, "nullable", "scalar");
    field!(w, "EXECUTION_CONTEXT_REGISTERS_AND_CONSTANTS_AND_LOCALS_AND_ARGUMENTS_COUNT", "ExecutionContext.slot_count", "u32", ExecutionContext, registers_and_constants_and_locals_and_arguments_count, 4, "nullable", "scalar", "counts");
    field!(w, "EXECUTION_CONTEXT_ARGUMENT_COUNT", "ExecutionContext.argument_count", "u32", ExecutionContext, argument_count, 4, "nullable", "scalar", "counts");
    let sizeof_execution_context = size!(w, "SIZEOF_EXECUTION_CONTEXT", ExecutionContext);
    w.line("field ExecutionContext.slots Sequence<Value> SIZEOF_EXECUTION_CONTEXT embedded scalar");
    let slot_offset = |index: u32| sizeof_execution_context + index as usize * value_size;
    w.constant("EXECUTION_CONTEXT_ACCUMULATOR", slot_offset(register::ACCUMULATOR));
    w.constant("EXECUTION_CONTEXT_EXCEPTION", slot_offset(register::EXCEPTION));
    w.constant("EXECUTION_CONTEXT_THIS_VALUE_REGISTER", slot_offset(register::THIS_VALUE));
    w.constant("EXECUTION_CONTEXT_RETURN_VALUE", slot_offset(register::RETURN_VALUE));
    w.constant("EXECUTION_CONTEXT_SAVED_LEXICAL_ENVIRONMENT", slot_offset(register::SAVED_LEXICAL_ENVIRONMENT));
    w.line("field ExecutionContext.accumulator Value EXECUTION_CONTEXT_ACCUMULATOR nullable scalar accumulator_and_exception");
    w.line("field ExecutionContext.exception Value EXECUTION_CONTEXT_EXCEPTION nullable scalar accumulator_and_exception pinned values EXCEPTION_REG_OFFSET");
    w.line("field ExecutionContext.this_value_register Value EXECUTION_CONTEXT_THIS_VALUE_REGISTER nullable scalar pinned values THIS_VALUE_REG_OFFSET");
    w.line("field ExecutionContext.return_value Value EXECUTION_CONTEXT_RETURN_VALUE nullable scalar return_and_saved_environment");
    w.line("field ExecutionContext.saved_lexical_environment Value EXECUTION_CONTEXT_SAVED_LEXICAL_ENVIRONMENT nullable scalar return_and_saved_environment");
    w.constant("ALIGNOF_EXECUTION_CONTEXT", align_of::<ExecutionContext>());
    w.constant("EXECUTION_CONTEXT_NO_YIELD_CONTINUATION", ExecutionContext::NO_YIELD_CONTINUATION);
    size!(w, "SIZEOF_SCRIPT_OR_MODULE", ScriptOrModule);
    size!(w, "SIZEOF_VALUE", Value);

    w.section("InterpreterStack layout");
    offset!(w, "INTERPRETER_STACK_LIMIT", InterpreterStack, limit);
    offset!(w, "INTERPRETER_STACK_TOP", InterpreterStack, top);
    offset!(w, "INTERPRETER_STACK_NEXT_FRAME_ID", InterpreterStack, next_frame_id);

    w.section("Realm layout");
    field!(w, "REALM_GLOBAL_ENVIRONMENT", "Realm.global_environment", "GlobalEnvironment", Realm, global_environment, 8, "nonnull", "cell");
    field!(w, "REALM_GLOBAL_OBJECT", "Realm.global_object", "Object", Realm, global_object, 8, "nullable", "cell", "global_state");
    field!(w, "REALM_GLOBAL_DECLARATIVE_ENVIRONMENT", "Realm.global_declarative_environment", "DeclarativeEnvironment", Realm, global_declarative_environment, 8, "nullable", "cell", "global_state");

    w.section("VM layout");
    offset!(w, "VM_RUNNING_EXECUTION_CONTEXT", VmHead, running_execution_context);
    offset!(w, "VM_INTERPRETER_STACK", VmHead, interpreter_stack);
    offset!(w, "VM_STACK_INFO", VmHead, stack_base);
    offset!(w, "VM_EXECUTION_GENERATION", VmHead, execution_generation);
    offset!(w, "VM_PRIMITIVE_STORAGE_CAGE_BASE", VmHead, primitive_storage_cage_base);
    offset!(w, "VM_HEAP_REGION_BASE", VmHead, heap_region_base);
    offset!(w, "VM_NATIVE_FUNCTION_TABLE_DATA", VmHead, native_function_table_data);
    offset!(w, "VM_BREAKPOINT_CONTROLLER", VmHead, debugger);
    w.line("field VM.primitive_storage_cage_base u64 VM_PRIMITIVE_STORAGE_CAGE_BASE nonnull scalar");
    w.line("field VM.heap_region_base u64 VM_HEAP_REGION_BASE nonnull scalar");
    w.line("field VM.native_function_table Sequence<NativeFunctionTableEntry> VM_NATIVE_FUNCTION_TABLE_DATA nonnull scalar");
    offset!(w, "VM_INTERPRETER_STACK_TOP", VmHead, interpreter_stack.top);
    offset!(w, "VM_INTERPRETER_STACK_LIMIT", VmHead, interpreter_stack.limit);
    offset!(w, "VM_INTERPRETER_STACK_NEXT_FRAME_ID", VmHead, interpreter_stack.next_frame_id);
    offset!(w, "VM_STACK_INFO_BASE", VmHead, stack_base);
    w.line("field VM.running_execution_context ExecutionContext VM_RUNNING_EXECUTION_CONTEXT nullable scalar");
    w.line("field VM.interpreter_stack_top u64 VM_INTERPRETER_STACK_TOP nonnull scalar interpreter_stack_bounds");
    w.line("field VM.interpreter_stack_limit u64 VM_INTERPRETER_STACK_LIMIT nonnull scalar interpreter_stack_bounds");
    w.line("field VM.interpreter_stack_next_frame_id u64 VM_INTERPRETER_STACK_NEXT_FRAME_ID nonnull scalar");
    w.line("field VM.stack_base u64 VM_STACK_INFO_BASE nullable scalar");
    w.constant("VM_STACK_SPACE_LIMIT", configuration.vm_stack_space_limit);

    w.section("StackInfo layout");
    w.constant("STACK_INFO_BASE", 0);

    w.section("IndexedStorageKind enum values");
    w.constant("INDEXED_STORAGE_KIND_NONE", IndexedStorageKind::None as u8);
    w.constant("INDEXED_STORAGE_KIND_PACKED", IndexedStorageKind::Packed as u8);
    w.constant("INDEXED_STORAGE_KIND_HOLEY", IndexedStorageKind::Holey as u8);
    w.constant("INDEXED_STORAGE_KIND_DICTIONARY", IndexedStorageKind::Dictionary as u8);

    w.section("ObjectPropertyIteratorFastPath enum values");
    w.constant("OBJECT_PROPERTY_ITERATOR_FAST_PATH_NONE", ObjectPropertyIteratorFastPath::None as u8);
    w.constant("OBJECT_PROPERTY_ITERATOR_FAST_PATH_PLAIN_NAMED", ObjectPropertyIteratorFastPath::PlainNamed as u8);
    w.constant("OBJECT_PROPERTY_ITERATOR_FAST_PATH_PACKED_INDEXED", ObjectPropertyIteratorFastPath::PackedIndexed as u8);

    w.section("Vector<Value> layout");
    offset!(w, "VECTOR_DATA", InterpreterBuffer<Value>, data);
    offset!(w, "VECTOR_SIZE", InterpreterBuffer<Value>, size);
    w.constant("INDEXED_ELEMENTS_CAPACITY", -(INDEXED_ELEMENTS_HEADER_SIZE as i64));
    w.line("field IndexedElements.capacity u32 INDEXED_ELEMENTS_CAPACITY nullable scalar");
    offset!(w, "EXECUTABLE_BYTECODE_DATA", ExecutableHead, bytecode_data);
    w.line("field Executable.bytecode_data u64 EXECUTABLE_BYTECODE_DATA nonnull scalar");
    offset!(w, "EXECUTABLE_PROPERTY_LOOKUP_CACHES_DATA", ExecutableHead, property_lookup_caches.data);
    w.line("field Executable.property_lookup_caches PropertyLookupCaches EXECUTABLE_PROPERTY_LOOKUP_CACHES_DATA nonnull scalar");
    offset!(w, "EXECUTABLE_GLOBAL_VARIABLE_CACHES_DATA", ExecutableHead, global_variable_caches.data);
    w.line("field Executable.global_variable_caches GlobalVariableCaches EXECUTABLE_GLOBAL_VARIABLE_CACHES_DATA nonnull scalar");
    offset!(w, "EXECUTABLE_ENVIRONMENT_COORDINATE_CACHES_DATA", ExecutableHead, environment_coordinate_caches.data);
    w.line("field Executable.environment_coordinate_caches Sequence<EnvironmentCoordinateEntry> EXECUTABLE_ENVIRONMENT_COORDINATE_CACHES_DATA nullable scalar");
    offset!(w, "EXECUTABLE_CONSTANTS_DATA", ExecutableHead, constants.data);
    offset!(w, "EXECUTABLE_CONSTANTS_SIZE", ExecutableHead, constants.size);
    offset!(w, "OBJECT_PROPERTY_ITERATOR_CACHE_DATA_PROPERTY_VALUES_DATA", ObjectPropertyIteratorCacheData, property_values.data);
    offset!(w, "OBJECT_PROPERTY_ITERATOR_CACHE_DATA_PROPERTY_VALUES_SIZE", ObjectPropertyIteratorCacheData, property_values.size);
    w.line("field ObjectPropertyIteratorCacheData.property_values Sequence<Value> OBJECT_PROPERTY_ITERATOR_CACHE_DATA_PROPERTY_VALUES_DATA nullable scalar");
    w.line("field ObjectPropertyIteratorCacheData.property_value_count u64 OBJECT_PROPERTY_ITERATOR_CACHE_DATA_PROPERTY_VALUES_SIZE nullable scalar");

    w.section("PutKind enum");
    w.constant("PUT_KIND_NORMAL", PutKind::Normal as u8);

    w.section("PrototypeChainValidity layout");
    field!(w, "PROTOTYPE_CHAIN_VALIDITY_VALID", "PrototypeChainValidity.valid", "bool", PrototypeChainValidity, valid, 1, "nonnull", "scalar");

    w.section("DeclarativeEnvironment layout");
    field!(w, "DECLARATIVE_ENVIRONMENT_RARE_DATA", "DeclarativeEnvironment.rare_data", "DeclarativeEnvironmentRareData", DeclarativeEnvironment, rare_data, 8, "nullable", "scalar");
    field!(w, "DECLARATIVE_ENVIRONMENT_SERIAL", "DeclarativeEnvironment.serial_number", "u64", DeclarativeEnvironment, serial_number, 8, "nullable", "scalar");

    w.section("GlobalVariableCache layout");
    offset!(w, "GLOBAL_VARIABLE_CACHE_ENTRY", GlobalVariableCache, entry);
    offset!(w, "GLOBAL_VARIABLE_CACHE_ENVIRONMENT_SERIAL", GlobalVariableCache, environment_serial_number);
    offset!(w, "GLOBAL_VARIABLE_CACHE_ENVIRONMENT_BINDING_INDEX", GlobalVariableCache, environment_binding_index);
    offset!(w, "GLOBAL_VARIABLE_CACHE_HAS_ENVIRONMENT_BINDING", GlobalVariableCache, has_environment_binding_index);
    size!(w, "GLOBAL_VARIABLE_CACHE_SIZE", GlobalVariableCache);
    offset!(w, "GLOBAL_VARIABLE_CACHE_ENTRY_PROPERTY_OFFSET", GlobalVariableCache, entry.property_offset);
    offset!(w, "GLOBAL_VARIABLE_CACHE_ENTRY_DICTIONARY_GENERATION", GlobalVariableCache, entry.shape_dictionary_generation);
    offset!(w, "GLOBAL_VARIABLE_CACHE_ENTRY_SHAPE", GlobalVariableCache, entry.shape);
    offset!(w, "GLOBAL_VARIABLE_CACHE_ENTRY_WRITES_DATA_PROPERTY", GlobalVariableCache, entry.writes_data_property);
    w.line("field GlobalVariableCache.property_offset u32 GLOBAL_VARIABLE_CACHE_ENTRY_PROPERTY_OFFSET nullable scalar global_cache_details stride GLOBAL_VARIABLE_CACHE_SIZE");
    w.line("field GlobalVariableCache.shape_dictionary_generation u32 GLOBAL_VARIABLE_CACHE_ENTRY_DICTIONARY_GENERATION nullable scalar global_cache_details");
    w.line("field GlobalVariableCache.shape Shape GLOBAL_VARIABLE_CACHE_ENTRY_SHAPE nullable cell");
    w.line("field GlobalVariableCache.writes_data_property bool GLOBAL_VARIABLE_CACHE_ENTRY_WRITES_DATA_PROPERTY nullable scalar");
    w.line("field GlobalVariableCache.environment_serial_number u64 GLOBAL_VARIABLE_CACHE_ENVIRONMENT_SERIAL nullable scalar");
    w.line("field GlobalVariableCache.environment_binding_index u32 GLOBAL_VARIABLE_CACHE_ENVIRONMENT_BINDING_INDEX nullable scalar");
    w.line("field GlobalVariableCache.has_environment_binding_index u8 GLOBAL_VARIABLE_CACHE_HAS_ENVIRONMENT_BINDING nullable scalar");

    w.section("Builtin enum values");
    for (name, builtin) in [
        ("BUILTIN_MATH_ABS", Builtin::MathAbs),
        ("BUILTIN_MATH_FLOOR", Builtin::MathFloor),
        ("BUILTIN_MATH_CEIL", Builtin::MathCeil),
        ("BUILTIN_MATH_ROUND", Builtin::MathRound),
        ("BUILTIN_MATH_SQRT", Builtin::MathSqrt),
        ("BUILTIN_MATH_EXP", Builtin::MathExp),
        ("BUILTIN_STRING_FROM_CHAR_CODE", Builtin::StringFromCharCode),
        (
            "BUILTIN_STRING_PROTOTYPE_CHAR_CODE_AT",
            Builtin::StringPrototypeCharCodeAt,
        ),
        ("BUILTIN_STRING_PROTOTYPE_CHAR_AT", Builtin::StringPrototypeCharAt),
    ] {
        w.constant(name, builtin as u8);
    }

    w.section("FunctionObject layout");
    offset!(w, "FUNCTION_OBJECT_BUILTIN", FunctionObject, builtin);
    offset!(w, "FUNCTION_OBJECT_BUILTIN_VALUE", FunctionObject, builtin);
    offset!(w, "FUNCTION_OBJECT_BUILTIN_HAS_VALUE", FunctionObject, has_builtin);
    w.line("field FunctionObject.builtin u8 FUNCTION_OBJECT_BUILTIN_VALUE nullable scalar");
    w.line("field FunctionObject.has_builtin bool FUNCTION_OBJECT_BUILTIN_HAS_VALUE nullable scalar");

    w.section("RawNativeFunction layout");
    field!(w, "RAW_NATIVE_FUNCTION_NATIVE_FUNCTION_INDEX", "RawNativeFunction.native_function_index", "u32", RawNativeFunction, native_function_index, 4, "nullable", "scalar");
    assert_eq!(size_of_field(|entry: &NativeFunctionTableEntry| &entry.function), 8);
    assert_eq!(size_of_field(|entry: &NativeFunctionTableEntry| &entry.function_type), 4);
    offset!(w, "NATIVE_FUNCTION_TABLE_ENTRY_FUNCTION", NativeFunctionTableEntry, function);
    offset!(w, "NATIVE_FUNCTION_TABLE_ENTRY_TYPE", NativeFunctionTableEntry, function_type);
    w.constant("NATIVE_FUNCTION_TYPE_COUNT", NativeFunctionType::RawNativeFunction as u32 + 1);
    let native_function_table_entry_size = size_of::<NativeFunctionTableEntry>();
    w.assert_size("NativeFunctionTableEntry", native_function_table_entry_size);
    w.line(&format!("field NativeFunctionTableEntry.function u64 NATIVE_FUNCTION_TABLE_ENTRY_FUNCTION nonnull scalar native_function_table_entry stride {native_function_table_entry_size}"));
    w.line("field NativeFunctionTableEntry.type u32 NATIVE_FUNCTION_TABLE_ENTRY_TYPE nullable scalar native_function_table_entry");

    w.section("DirectGetterFunction layout");
    field!(w, "DIRECT_GETTER_FUNCTION_WRAPPER_IMPLEMENTATION_WORD_OFFSET", "DirectGetterFunction.wrapper_implementation_word_offset", "u32", DirectGetterFunction, wrapper_implementation_word_offset, 4, "nullable", "scalar");
    field!(w, "DIRECT_GETTER_FUNCTION_IMPLEMENTATION_VALUE_WORD_OFFSET", "DirectGetterFunction.implementation_value_word_offset", "u32", DirectGetterFunction, implementation_value_word_offset, 4, "nullable", "scalar");
    field!(w, "DIRECT_GETTER_FUNCTION_MAIN_WORLD_WRAPPER_WORD_OFFSET", "DirectGetterFunction.main_world_wrapper_word_offset", "u32", DirectGetterFunction, main_world_wrapper_word_offset, 4, "nullable", "scalar");
    field!(w, "DIRECT_GETTER_FUNCTION_WEAK_IMPL_VALUE_WORD_OFFSET", "DirectGetterFunction.weak_impl_value_word_offset", "u32", DirectGetterFunction, weak_impl_value_word_offset, 4, "nullable", "scalar");

    w.section("ECMAScriptFunctionObject layout");
    field!(w, "ECMASCRIPT_FUNCTION_OBJECT_SHARED_DATA", "ECMAScriptFunctionObject.shared_data", "SharedFunctionInstanceData", EcmascriptFunctionObject, shared_data, 8, "nonnull", "cell");
    field!(w, "ECMASCRIPT_FUNCTION_OBJECT_ENVIRONMENT", "ECMAScriptFunctionObject.environment", "Environment", EcmascriptFunctionObject, environment, 8, "nullable", "cell", "environment_state");
    field!(w, "ECMASCRIPT_FUNCTION_OBJECT_PRIVATE_ENVIRONMENT", "ECMAScriptFunctionObject.private_environment", "PrivateEnvironment", EcmascriptFunctionObject, private_environment, 8, "nullable", "cell", "environment_state");
    field!(w, "ECMASCRIPT_FUNCTION_OBJECT_SCRIPT_OR_MODULE", "ECMAScriptFunctionObject.script_or_module", "ScriptOrModule", EcmascriptFunctionObject, script_or_module, 16, "nullable", "scalar");

    w.section("SharedFunctionInstanceData layout");
    field!(w, "SHARED_FUNCTION_INSTANCE_DATA_EXECUTABLE", "SharedFunctionInstanceData.executable", "Executable", SharedFunctionInstanceData, executable, 8, "nullable", "cell", "call_data");
    field!(w, "SHARED_FUNCTION_INSTANCE_DATA_ASM_CALL_METADATA", "SharedFunctionInstanceData.asm_call_metadata", "u64", SharedFunctionInstanceData, asm_call_metadata, 8, "nullable", "scalar", "call_data");
    offset!(w, "SHARED_FUNCTION_INSTANCE_DATA_FORMAL_PARAMETER_COUNT", SharedFunctionInstanceData, formal_parameter_count);
    offset!(w, "SHARED_FUNCTION_INSTANCE_DATA_STRICT", SharedFunctionInstanceData, strict);
    offset!(w, "SHARED_FUNCTION_INSTANCE_DATA_FUNCTION_ENVIRONMENT_NEEDED", SharedFunctionInstanceData, function_environment_needed);
    offset!(w, "SHARED_FUNCTION_INSTANCE_DATA_USES_THIS", SharedFunctionInstanceData, uses_this);
    offset!(w, "SHARED_FUNCTION_INSTANCE_DATA_CAN_INLINE_CALL", SharedFunctionInstanceData, can_inline_call);
    w.constant("SHARED_FUNCTION_INSTANCE_DATA_ASM_CALL_METADATA_CAN_INLINE_CALL", asm_call_metadata::CAN_INLINE_CALL);
    w.constant("SHARED_FUNCTION_INSTANCE_DATA_ASM_CALL_METADATA_NEEDS_ENVIRONMENT_OR_THIS_VALUE_RESOLUTION", asm_call_metadata::NEEDS_ENVIRONMENT_OR_THIS_VALUE_RESOLUTION);
    w.constant("SHARED_FUNCTION_INSTANCE_DATA_ASM_CALL_METADATA_USES_THIS", asm_call_metadata::USES_THIS);
    w.constant("SHARED_FUNCTION_INSTANCE_DATA_ASM_CALL_METADATA_STRICT", asm_call_metadata::STRICT);

    w.section("GlobalEnvironment layout");
    field!(w, "GLOBAL_ENVIRONMENT_GLOBAL_THIS_VALUE", "GlobalEnvironment.global_this_value", "Object", GlobalEnvironment, global_this_value, 8, "nullable", "cell");

    w.section("PrimitiveString layout");
    field!(w, "PRIMITIVE_STRING_DEFERRED_KIND", "PrimitiveString.deferred_kind", "u8", PrimitiveString, deferred_kind, 1, "nullable", "scalar");
    field!(w, "PRIMITIVE_STRING_LENGTH_IN_UTF16_CODE_UNITS", "PrimitiveString.length_in_utf16_code_units", "u32", PrimitiveString, length_in_utf16_code_units, 4, "nullable", "scalar");
    assert_eq!(size_of_field(|string: &PrimitiveString| &string.utf16_string), 8);
    let utf16_string_offset = offset!(w, "PRIMITIVE_STRING_UTF16_STRING", PrimitiveString, utf16_string);
    w.line("field PrimitiveString.utf16_data Utf16StringData PRIMITIVE_STRING_UTF16_STRING nullable scalar");
    w.constant("PRIMITIVE_STRING_DEFERRED_KIND_NONE", DeferredKind::None as u8);

    // A short AK string is stored inline in the string's word: a tag byte holding the flag and the byte count, then
    // the bytes themselves. That puts the tag in the word's lowest byte on the little-endian targets the
    // interpreter supports.
    w.section("Utf16String layout");
    w.constant("UTF16_SHORT_STRING_FLAG", 1);
    w.constant("UTF16_SHORT_STRING_BYTE_COUNT_SHIFT_COUNT", 2);
    w.constant("UTF16_SHORT_STRING_BYTE_COUNT_AND_FLAG", 0);
    w.constant("UTF16_SHORT_STRING_STORAGE", 1);
    w.constant("PRIMITIVE_STRING_UTF16_SHORT_STRING_BYTE_COUNT_AND_FLAG", utf16_string_offset);
    w.constant("PRIMITIVE_STRING_UTF16_SHORT_STRING_STORAGE", utf16_string_offset + 1);
    w.line("field PrimitiveString.utf16_short_string_byte_count_and_flag u8 PRIMITIVE_STRING_UTF16_SHORT_STRING_BYTE_COUNT_AND_FLAG nullable scalar");
    w.line("field PrimitiveString.utf16_short_string_storage Sequence<u8> PRIMITIVE_STRING_UTF16_SHORT_STRING_STORAGE embedded scalar");

    w.section("Utf16StringData layout");
    offset!(w, "UTF16_STRING_DATA_LENGTH_IN_CODE_UNITS", Utf16StringDataHeader, length_in_code_units);
    offset!(w, "UTF16_STRING_DATA_FLAGS", Utf16StringDataHeader, flags);
    size!(w, "UTF16_STRING_DATA_STRING_STORAGE", Utf16StringDataHeader);
    w.constant("UTF16_STRING_DATA_HAS_UTF16_STORAGE", 1);
    w.line("field Utf16StringData.length_in_code_units u32 UTF16_STRING_DATA_LENGTH_IN_CODE_UNITS nullable scalar");
    w.line("field Utf16StringData.flags u32 UTF16_STRING_DATA_FLAGS nullable scalar");
    w.line("field Utf16StringData.string_storage Sequence<u8> UTF16_STRING_DATA_STRING_STORAGE embedded scalar");

    w.section("Environment layout");
    field!(w, "ENVIRONMENT_SCREWED_BY_EVAL", "Environment.permanently_screwed_by_eval", "bool", Environment, permanently_screwed_by_eval, 1, "nullable", "scalar");
    field!(w, "ENVIRONMENT_DECLARATIVE", "Environment.declarative", "bool", Environment, declarative, 1, "nullable", "scalar");
    field!(w, "ENVIRONMENT_OUTER", "Environment.outer", "Environment", Environment, outer, 8, "nullable", "cell");

    w.section("PrivateEnvironment layout");
    field!(w, "PRIVATE_ENVIRONMENT_OUTER", "PrivateEnvironment.outer", "PrivateEnvironment", PrivateEnvironment, outer, 8, "nullable", "cell");

    w.section("DeclarativeEnvironment binding storage layout");
    field!(w, "DECLARATIVE_ENVIRONMENT_SHAPE", "DeclarativeEnvironment.shape", "EnvironmentShape", DeclarativeEnvironment, shape, 8, "nullable", "cell");
    offset!(w, "DECLARATIVE_ENVIRONMENT_BINDING_VALUES", DeclarativeEnvironment, binding_values);
    offset!(w, "DECLARATIVE_ENVIRONMENT_RARE_DATA_BINDING_FLAGS", DeclarativeEnvironmentRareData, binding_flags);
    offset!(w, "ENVIRONMENT_SHAPE_BINDING_FLAGS", EnvironmentShape, binding_flags);
    w.constant("BINDING_FLAG_MUTABLE", BINDING_FLAG_MUTABLE);
    offset!(w, "BINDING_VALUES_DATA_PTR", DeclarativeEnvironment, binding_values.data);
    w.line("field DeclarativeEnvironment.binding_values BindingValues BINDING_VALUES_DATA_PTR nonnull scalar");
    offset!(w, "BINDING_FLAGS_DATA_PTR", DeclarativeEnvironmentRareData, binding_flags.data);
    w.line("field DeclarativeEnvironmentRareData.binding_flags BindingFlags BINDING_FLAGS_DATA_PTR nonnull scalar");
    offset!(w, "ENVIRONMENT_SHAPE_BINDING_FLAGS_DATA_PTR", EnvironmentShape, binding_flags.data);
    offset!(w, "ENVIRONMENT_SHAPE_BINDING_FLAGS_SIZE", EnvironmentShape, binding_flags.size);
    w.line("field EnvironmentShape.binding_flags_size u64 ENVIRONMENT_SHAPE_BINDING_FLAGS_SIZE nullable scalar");
    w.line("field EnvironmentShape.binding_flags BindingFlags ENVIRONMENT_SHAPE_BINDING_FLAGS_DATA_PTR nonnull scalar");

    w.section("EnvironmentCoordinate layout");
    offset!(w, "ENVIRONMENT_COORDINATE_HOPS", EnvironmentCoordinate, hops);
    offset!(w, "ENVIRONMENT_COORDINATE_INDEX", EnvironmentCoordinate, index);
    w.hex_constant("ENVIRONMENT_COORDINATE_INVALID", u64::from(EnvironmentCoordinate::INVALID_MARKER));
    let environment_coordinate_size = size!(w, "ENVIRONMENT_COORDINATE_SIZE", EnvironmentCoordinate);
    w.line(&format!("field EnvironmentCoordinateEntry.hops u32 ENVIRONMENT_COORDINATE_HOPS nullable scalar environment_coordinate stride {environment_coordinate_size}"));
    w.line("field EnvironmentCoordinateEntry.binding_index u32 ENVIRONMENT_COORDINATE_INDEX nullable scalar environment_coordinate");

    w.section("TypedArrayBase layout");
    offset!(w, "TYPED_ARRAY_ELEMENT_SIZE", TypedArrayBase, element_size);
    offset!(w, "TYPED_ARRAY_ARRAY_LENGTH", TypedArrayBase, array_length);
    offset!(w, "TYPED_ARRAY_BYTE_OFFSET", TypedArrayBase, byte_offset);
    field!(w, "TYPED_ARRAY_KIND", "Object.typed_array_kind", "u8", TypedArrayBase, kind, 1, "nullable", "scalar");
    field!(w, "TYPED_ARRAY_CACHED_DATA_OFFSET", "Object.typed_array_cached_data_offset", "u64", TypedArrayBase, cached_data_offset, 8, "nullable", "scalar");
    w.hex_constant("TYPED_ARRAY_CACHED_DATA_OFFSET_INVALID", TYPED_ARRAY_CACHED_DATA_OFFSET_INVALID as u64);
    w.hex_constant("PRIMITIVE_STORAGE_CAGE_OFFSET_MASK", configuration.primitive_storage_cage_offset_mask);
    w.hex_constant("HEAP_REGION_OFFSET_MASK", configuration.heap_region_offset_mask);

    w.section("ByteLength layout");
    w.constant("BYTE_LENGTH_U32_INDEX", BYTE_LENGTH_U32_INDEX);
    size!(w, "BYTE_LENGTH_SIZE", ByteLengthSlot);
    offset!(w, "TYPED_ARRAY_ARRAY_LENGTH_VALUE", TypedArrayBase, array_length.length);
    w.line("field Object.typed_array_array_length u32 TYPED_ARRAY_ARRAY_LENGTH_VALUE nullable scalar");
    offset!(w, "TYPED_ARRAY_ARRAY_LENGTH_INDEX", TypedArrayBase, array_length.alternative_index);

    w.section("TypedArrayBase::Kind values");
    w.constant("TYPED_ARRAY_KIND_UINT8", typed_array_kind::UINT8);
    w.constant("TYPED_ARRAY_KIND_UINT8_CLAMPED", typed_array_kind::UINT8_CLAMPED);
    w.constant("TYPED_ARRAY_KIND_UINT16", typed_array_kind::UINT16);
    w.constant("TYPED_ARRAY_KIND_UINT32", typed_array_kind::UINT32);
    w.constant("TYPED_ARRAY_KIND_INT8", typed_array_kind::INT8);
    w.constant("TYPED_ARRAY_KIND_INT16", typed_array_kind::INT16);
    w.constant("TYPED_ARRAY_KIND_INT32", typed_array_kind::INT32);
    w.constant("TYPED_ARRAY_KIND_FLOAT32", typed_array_kind::FLOAT32);
    w.constant("TYPED_ARRAY_KIND_FLOAT64", typed_array_kind::FLOAT64);

    w.section("Value tags");
    w.hex_constant("OBJECT_TAG", nan_box::OBJECT_TAG);
    w.hex_constant("STRING_TAG", nan_box::STRING_TAG);
    w.hex_constant("SYMBOL_TAG", nan_box::SYMBOL_TAG);
    w.hex_constant("BIGINT_TAG", nan_box::BIGINT_TAG);
    w.hex_constant("ACCESSOR_TAG", nan_box::ACCESSOR_TAG);
    w.hex_constant("IS_CELL_PATTERN", nan_box::IS_CELL_PATTERN);
    w.hex_constant("INT32_TAG", nan_box::INT32_TAG);
    w.hex_constant("BOOLEAN_TAG", nan_box::BOOLEAN_TAG);
    w.hex_constant("UNDEFINED_TAG", nan_box::UNDEFINED_TAG);
    w.hex_constant("NULL_TAG", nan_box::NULL_TAG);

    w.section("Shifted value constants");
    w.hex_constant("OBJECT_TAG_SHIFTED", nan_box::OBJECT_TAG << nan_box::TAG_SHIFT);
    w.hex_constant("EMPTY_VALUE", nan_box::EMPTY_VALUE);
    w.hex_constant("INT32_TAG_SHIFTED", nan_box::SHIFTED_INT32_TAG);
    w.hex_constant("BOOLEAN_TRUE", nan_box::TRUE_VALUE);
    w.hex_constant("BOOLEAN_FALSE", nan_box::FALSE_VALUE);
    w.hex_constant("UNDEFINED_SHIFTED", nan_box::UNDEFINED_VALUE);
    w.hex_constant("NULL_VALUE", nan_box::NULL_VALUE);
    w.hex_constant("EMPTY_TAG_SHIFTED", nan_box::EMPTY_VALUE);
    w.hex_constant("NAN_BASE_TAG", nan_box::BASE_TAG);
    w.hex_constant("CANON_NAN_BITS", nan_box::CANON_NAN_BITS);
    w.hex_constant("DOUBLE_ONE", 1.0f64.to_bits());
    w.hex_constant("NEGATIVE_ZERO", nan_box::NEGATIVE_ZERO_BITS);
    w.hex_constant("SHIFTED_IS_CELL_PATTERN", nan_box::SHIFTED_IS_CELL_PATTERN);

    let register_offset = |index: u32| index as usize * value_size;
    w.constant("ACCUMULATOR_REG_OFFSET", register_offset(register::ACCUMULATOR));
    w.constant("EXCEPTION_REG_OFFSET", register_offset(register::EXCEPTION));
    w.constant("THIS_VALUE_REG_OFFSET", register_offset(register::THIS_VALUE));
    w.constant("RETURN_VALUE_REG_OFFSET", register_offset(register::RETURN_VALUE));
    w.constant("SAVED_LEXICAL_ENVIRONMENT_REG_OFFSET", register_offset(register::SAVED_LEXICAL_ENVIRONMENT));
    w.constant("RESERVED_REGISTER_COUNT", register::RESERVED_REGISTER_COUNT);

    w
}
