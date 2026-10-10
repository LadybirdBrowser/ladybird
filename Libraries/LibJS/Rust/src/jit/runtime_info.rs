/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What generated code needs to know about the runtime: the addresses of the functions it calls, and the offsets
//! and encodings of the fields it accesses, from the same layout types the interpreter is compiled against.

use core::mem::offset_of;

use libjs_abi::Builtin;
use libjs_abi::value as nan_box;
use libjs_jit::bytecode::OpCode;
use libjs_jit::codegen::slow_path_symbol;
use libjs_jit::snapshot::{
    CellId, DynamicCallLayout, IntrinsicHelpers, RuntimeInfo, RuntimeLayout, RuntimeOffsets, TypeofStrings,
};

use super::allocation::{AllocationInfos, FUNCTION_ENVIRONMENT_FREE_LIST_COUNT};
use super::entry_table::{JIT_ENTRY_SLOT_MASK, JIT_ENTRY_TABLE_OWNERS_OFFSET};
use super::offset;
use crate::build_configuration::{HEAP_REGION_OFFSET_MASK, PRIMITIVE_STORAGE_CAGE_OFFSET_MASK};
use crate::bytecode::executable::{
    MAX_NUMBER_OF_SHAPES_TO_REMEMBER, MEGAMORPHIC_DATA_TAG, MEGAMORPHIC_HASH_MULTIPLIER, MEGAMORPHIC_INDEX_BITS,
    MEGAMORPHIC_PRIMARY_ENTRIES_OFFSET, MEGAMORPHIC_SECONDARY_ENTRIES_OFFSET,
};
use crate::gc::class::{Class, GcCell};
use crate::interpreter::runtime_functions::runtime_function_addresses;
use crate::interpreter::vm::{FLY_STRING_CACHE_SIZE, NUMERIC_STRING_CACHE_SIZE, Vm};
use crate::layout::accessor::Accessor;
use crate::layout::cell::Gc;
use crate::layout::environment::{
    BINDING_FLAG_MUTABLE, DeclarativeEnvironment, DeclarativeEnvironmentRareData, Environment, EnvironmentShape,
    PrivateEnvironment,
};
use crate::layout::executable::ExecutableHead;
use crate::layout::execution_context::ExecutionContext;
use crate::layout::function_object::{
    CallEnvironmentTemplate, EcmascriptFunctionObject, FUNCTION_ENVIRONMENT_WORDS, FunctionObject,
    NATIVE_FUNCTION_TABLE_INDEX_MASK, NativeFunctionTableEntry, RawNativeFunction, SharedFunctionInstanceData,
    asm_call_metadata,
};
use crate::layout::object::{
    INDEXED_ELEMENTS_HEADER_SIZE, IndexedStorageKind, Object, TYPED_ARRAY_CACHED_DATA_OFFSET_INVALID, TypedArrayBase,
    object_flag, typed_array_kind,
};
use crate::layout::primitive_string::{DEFERRED_KIND_MASK, DeferredKind, INTERNED_FLAG, InlineString, PrimitiveString};
use crate::layout::property_lookup_cache::{
    GlobalVariableCache, ObjectPropertyIteratorCacheData, ObjectPropertyIteratorFastPath,
    PROPERTY_LOOKUP_CACHE_KEYED_GENERIC_DATA, PROPERTY_LOOKUP_CACHE_POLYMORPHIC_DATA_TAG, PropertyLookupCacheEntry,
    PropertyLookupCacheEntryType,
};
use crate::layout::realm::Realm;
use crate::layout::shape::{PrototypeChainValidity, Shape};
use crate::layout::value::Value;
use crate::layout::vm::VmHead;
use crate::runtime::array::{ARRAY_IS_PROXY_TARGET_OFFSET, ARRAY_LENGTH_WRITABLE_OFFSET};
use crate::runtime::function_environment::{
    FUNCTION_ENVIRONMENT_FUNCTION_OBJECT_OFFSET, FUNCTION_ENVIRONMENT_THIS_VALUE_OFFSET,
};
use crate::runtime::module_environment::ModuleEnvironment;
use crate::runtime::object::ObjectMethods;

/// The address of the slow path JIT code calls for each generic opcode, indexed by opcode, or 0 for opcodes without
/// one (the compiler refuses to compile those generically).
pub fn slow_path_addresses() -> Vec<u64> {
    let addresses = runtime_function_addresses();
    (0..=u8::MAX)
        .map_while(OpCode::from_u8)
        .map(|opcode| {
            let Some(symbol) = slow_path_symbol(opcode) else {
                return 0;
            };
            let (_, address) = addresses
                .iter()
                .find(|(name, _)| *name == symbol)
                .unwrap_or_else(|| panic!("the runtime has the slow path {symbol}"));
            *address as u64
        })
        .collect()
}

fn runtime_offsets() -> RuntimeOffsets {
    RuntimeOffsets {
        execution_context_program_counter: offset(offset_of!(ExecutionContext, program_counter)),
        execution_context_lexical_environment: offset(offset_of!(ExecutionContext, lexical_environment)),
        execution_context_private_environment: offset(offset_of!(ExecutionContext, private_environment)),
        execution_context_frame_initialized: offset(offset_of!(ExecutionContext, frame_initialized)),
        execution_context_executable: offset(offset_of!(ExecutionContext, executable)),
        execution_context_slots: offset(size_of::<ExecutionContext>()),
        vm_running_execution_context: offset(offset_of!(VmHead, running_execution_context)),
        vm_jit_native_stack_limit: offset(offset_of!(Vm, jit_native_stack_limit)),
        private_environment_outer: offset(offset_of!(PrivateEnvironment, outer)),
        object_flags: offset(offset_of!(Object, flags)),
        vm_interpreter_stack_top: offset(offset_of!(VmHead, interpreter_stack.top)),
        vm_interpreter_stack_limit: offset(offset_of!(VmHead, interpreter_stack.limit)),
        object_shape: offset(offset_of!(Object, shape)),
        object_named_properties: offset(offset_of!(Object, named_properties)),
        shape_dictionary_generation: offset(offset_of!(Shape, dictionary_generation)),
        prototype_chain_validity_valid: offset(offset_of!(PrototypeChainValidity, valid)),
        accessor_getter: offset(offset_of!(Accessor, getter)),
        accessor_setter: offset(offset_of!(Accessor, setter)),
        execution_context_function: offset(offset_of!(ExecutionContext, function)),
        execution_context_realm: offset(offset_of!(ExecutionContext, realm)),
        execution_context_script_or_module: offset(offset_of!(ExecutionContext, script_or_module)),
        execution_context_variable_environment: offset(offset_of!(ExecutionContext, variable_environment)),
        execution_context_frame_id: offset(offset_of!(ExecutionContext, frame_id)),
        execution_context_skip_when_determining_incumbent_counter: offset(offset_of!(
            ExecutionContext,
            skip_when_determining_incumbent_counter
        )),
        execution_context_yield_continuation: offset(offset_of!(ExecutionContext, yield_continuation)),
        execution_context_yield_is_await: offset(offset_of!(ExecutionContext, yield_is_await)),
        execution_context_yield_value_is_iterator_result: offset(offset_of!(
            ExecutionContext,
            yield_value_is_iterator_result
        )),
        execution_context_caller_is_construct: offset(offset_of!(ExecutionContext, caller_is_construct)),
        execution_context_this_value: offset(offset_of!(ExecutionContext, this_value)),
        execution_context_caller_frame: offset(offset_of!(ExecutionContext, caller_frame)),
        execution_context_passed_argument_count: offset(offset_of!(ExecutionContext, passed_argument_count)),
        execution_context_caller_return_pc: offset(offset_of!(ExecutionContext, caller_return_pc)),
        execution_context_caller_dst_raw: offset(offset_of!(ExecutionContext, caller_dst_raw)),
        execution_context_slot_count: offset(offset_of!(
            ExecutionContext,
            registers_and_constants_and_locals_and_arguments_count
        )),
        execution_context_argument_count: offset(offset_of!(ExecutionContext, argument_count)),
        execution_context_returns_to_native_caller: offset(offset_of!(ExecutionContext, returns_to_native_caller)),
        execution_context_runs_jit_code: offset(offset_of!(ExecutionContext, runs_jit_code)),
        ecmascript_function_environment: offset(offset_of!(EcmascriptFunctionObject, environment)),
        ecmascript_function_private_environment: offset(offset_of!(EcmascriptFunctionObject, private_environment)),
        ecmascript_function_script_or_module: offset(offset_of!(EcmascriptFunctionObject, script_or_module)),
        vm_execution_generation: offset(offset_of!(VmHead, execution_generation)),
    }
}

fn runtime_layout() -> RuntimeLayout {
    let entry = |field: usize| offset(offset_of!(GlobalVariableCache, entry) + field);
    RuntimeLayout {
        object_indexed_elements: offset(offset_of!(Object, indexed_elements)),
        object_indexed_storage_kind: offset(offset_of!(Object, indexed_storage_kind)),
        object_indexed_array_like_size: offset(offset_of!(Object, indexed_array_like_size)),
        indexed_elements_capacity: -(INDEXED_ELEMENTS_HEADER_SIZE as i32),
        indexed_storage_kind_none: IndexedStorageKind::None as u8,
        indexed_storage_kind_packed: IndexedStorageKind::Packed as u8,
        indexed_storage_kind_holey: IndexedStorageKind::Holey as u8,
        object_flag_is_extensible: object_flag::IS_EXTENSIBLE,
        object_flag_has_magical_length: object_flag::HAS_MAGICAL_LENGTH_PROPERTY,
        object_flag_may_interfere: object_flag::MAY_INTERFERE_WITH_INDEXED_PROPERTY_ACCESS,
        array_length_writable: offset(ARRAY_LENGTH_WRITABLE_OFFSET),
        array_is_proxy_target: offset(ARRAY_IS_PROXY_TARGET_OFFSET),
        shape_prototype: offset(offset_of!(Shape, prototype)),
        object_flag_is_typed_array: object_flag::IS_TYPED_ARRAY,
        object_flag_is_htmldda: object_flag::IS_HTMLDDA,
        object_flag_requires_slow_add_own_property: object_flag::REQUIRES_SLOW_ADD_OWN_PROPERTY,
        // NB: Named properties beyond the inline storage live in storage with its capacity in front, like indexed
        //     elements.
        named_properties_capacity: -(INDEXED_ELEMENTS_HEADER_SIZE as i32),
        shape_property_count: offset(offset_of!(Shape, property_count)),
        typed_array_cached_data_offset: offset(offset_of!(TypedArrayBase, cached_data_offset)),
        typed_array_cached_data_offset_invalid: TYPED_ARRAY_CACHED_DATA_OFFSET_INVALID as u64,
        typed_array_array_length: offset(offset_of!(TypedArrayBase, array_length.length)),
        typed_array_kind: offset(offset_of!(TypedArrayBase, kind)),
        typed_array_kind_uint8: typed_array_kind::UINT8,
        typed_array_kind_uint8_clamped: typed_array_kind::UINT8_CLAMPED,
        typed_array_kind_uint16: typed_array_kind::UINT16,
        typed_array_kind_uint32: typed_array_kind::UINT32,
        typed_array_kind_int8: typed_array_kind::INT8,
        typed_array_kind_int16: typed_array_kind::INT16,
        typed_array_kind_int32: typed_array_kind::INT32,
        typed_array_kind_float32: typed_array_kind::FLOAT32,
        typed_array_kind_float64: typed_array_kind::FLOAT64,
        vm_primitive_storage_cage_base: offset(offset_of!(VmHead, primitive_storage_cage_base)),
        primitive_storage_cage_offset_mask: PRIMITIVE_STORAGE_CAGE_OFFSET_MASK,
        primitive_string_length: offset(offset_of!(PrimitiveString, length_in_utf16_code_units)),
        primitive_string_storage: offset(offset_of!(PrimitiveString, utf16_string)),
        primitive_string_is_interned: offset(offset_of!(PrimitiveString, deferred_kind_and_flags)),
        primitive_string_interned_mask: INTERNED_FLAG,
        primitive_string_deferred_kind_mask: DEFERRED_KIND_MASK,
        primitive_string_deferred_kind_inline: DeferredKind::Inline as u8,
        primitive_string_inline_storage: offset(offset_of!(InlineString, characters)),
        utf16_short_string_flag: ak::SHORT_STRING_FLAG as u8,
        utf16_short_string_byte_count_shift: ak::SHORT_STRING_BYTE_COUNT_SHIFT as u8,
        utf16_string_data_flags: offset(offset_of!(ak::Utf16StringDataHeader, flags)),
        utf16_string_data_has_utf16_storage: ak::HAS_UTF16_STORAGE,
        utf16_string_data_storage: offset(size_of::<ak::Utf16StringDataHeader>()),
        // NB: Filled in by runtime_info().
        single_ascii_character_strings: 0,
        function_object_builtin: offset(offset_of!(FunctionObject, builtin)),
        function_object_has_builtin: offset(offset_of!(FunctionObject, has_builtin)),
        builtin_string_prototype_char_code_at: Builtin::StringPrototypeCharCodeAt as u8,
        builtin_string_prototype_char_at: Builtin::StringPrototypeCharAt as u8,
        builtin_math_abs: Builtin::MathAbs as u8,
        builtin_math_floor: Builtin::MathFloor as u8,
        builtin_math_ceil: Builtin::MathCeil as u8,
        builtin_math_round: Builtin::MathRound as u8,
        builtin_math_sqrt: Builtin::MathSqrt as u8,
        // NB: Filled in by runtime_info().
        typeof_strings: TypeofStrings::default(),
        fly_string_cache: 0,
        fly_string_cache_mask: (FLY_STRING_CACHE_SIZE - 1) as u32,
        numeric_string_cache: 0,
        numeric_string_cache_size: NUMERIC_STRING_CACHE_SIZE as u32,
        environment_outer: offset(offset_of!(Environment, outer)),
        environment_declarative: offset(offset_of!(Environment, declarative)),
        module_environment_class: core::ptr::from_ref(ModuleEnvironment::CLASS) as u64,
        declarative_environment_binding_values_size: offset(offset_of!(DeclarativeEnvironment, binding_values.size)),
        declarative_environment_binding_values_capacity: offset(offset_of!(
            DeclarativeEnvironment,
            binding_values.capacity
        )),
        environment_shape_binding_names: offset(offset_of!(EnvironmentShape, binding_names)),
        environment_shape_has_unique_binding_names: offset(offset_of!(EnvironmentShape, has_unique_binding_names)),
        declarative_environment_binding_values: offset(offset_of!(DeclarativeEnvironment, binding_values.data)),
        declarative_environment_shape: offset(offset_of!(DeclarativeEnvironment, shape)),
        declarative_environment_rare_data: offset(offset_of!(DeclarativeEnvironment, rare_data)),
        rare_data_binding_flags: offset(offset_of!(DeclarativeEnvironmentRareData, binding_flags.data)),
        environment_shape_binding_flags_size: offset(offset_of!(EnvironmentShape, binding_flags.size)),
        environment_shape_binding_flags: offset(offset_of!(EnvironmentShape, binding_flags.data)),
        declarative_environment_serial: offset(offset_of!(DeclarativeEnvironment, serial_number)),
        realm_global_object: offset(offset_of!(Realm, global_object)),
        realm_global_declarative_environment: offset(offset_of!(Realm, global_declarative_environment)),
        executable_global_variable_caches: offset(offset_of!(ExecutableHead, global_variable_caches.data)),
        global_variable_cache_size: offset(size_of::<GlobalVariableCache>()),
        global_variable_cache_environment_serial: offset(offset_of!(GlobalVariableCache, environment_serial_number)),
        global_variable_cache_environment_binding_index: offset(offset_of!(
            GlobalVariableCache,
            environment_binding_index
        )),
        global_variable_cache_has_environment_binding: offset(offset_of!(
            GlobalVariableCache,
            has_environment_binding_index
        )),
        global_variable_cache_shape: entry(offset_of!(PropertyLookupCacheEntry, shape)),
        global_variable_cache_dictionary_generation: entry(offset_of!(
            PropertyLookupCacheEntry,
            shape_dictionary_generation
        )),
        global_variable_cache_property_offset: entry(offset_of!(PropertyLookupCacheEntry, property_offset)),
        global_variable_cache_writes_data_property: entry(offset_of!(PropertyLookupCacheEntry, writes_data_property)),
        binding_flag_mutable: BINDING_FLAG_MUTABLE,
        binding_flag_strict: EnvironmentShape::BINDING_FLAG_STRICT,
        binding_flag_can_be_deleted: EnvironmentShape::BINDING_FLAG_CAN_BE_DELETED,
        property_iterator_fast_path: offset(offset_of!(ObjectPropertyIteratorCacheData, fast_path)),
        property_iterator_shape: offset(offset_of!(ObjectPropertyIteratorCacheData, shape)),
        property_iterator_shape_is_dictionary: offset(offset_of!(ObjectPropertyIteratorCacheData, shape_is_dictionary)),
        property_iterator_shape_dictionary_generation: offset(offset_of!(
            ObjectPropertyIteratorCacheData,
            shape_dictionary_generation
        )),
        property_iterator_indexed_property_count: offset(offset_of!(
            ObjectPropertyIteratorCacheData,
            indexed_property_count
        )),
        property_iterator_prototype_chain_validity: offset(offset_of!(
            ObjectPropertyIteratorCacheData,
            prototype_chain_validity
        )),
        property_iterator_property_values: offset(offset_of!(ObjectPropertyIteratorCacheData, property_values.data)),
        property_iterator_property_value_count: offset(offset_of!(
            ObjectPropertyIteratorCacheData,
            property_values.size
        )),
        property_iterator_fast_path_none: ObjectPropertyIteratorFastPath::None as u8,
        property_iterator_fast_path_packed_indexed: ObjectPropertyIteratorFastPath::PackedIndexed as u8,
        executable_property_lookup_caches: offset(offset_of!(ExecutableHead, property_lookup_caches.data)),
        property_lookup_cache_data_pointer_mask:
            !(crate::layout::property_lookup_cache::PROPERTY_LOOKUP_CACHE_DATA_TAG_MASK as u64),
        property_lookup_cache_entry_type: offset(offset_of!(PropertyLookupCacheEntry, entry_type)),
        property_lookup_cache_entry_property_offset: offset(offset_of!(PropertyLookupCacheEntry, property_offset)),
        property_lookup_cache_entry_dictionary_generation: offset(offset_of!(
            PropertyLookupCacheEntry,
            shape_dictionary_generation
        )),
        property_lookup_cache_entry_writes_data_property: offset(offset_of!(
            PropertyLookupCacheEntry,
            writes_data_property
        )),
        property_lookup_cache_entry_shape: offset(offset_of!(PropertyLookupCacheEntry, shape)),
        property_lookup_cache_entry_prototype: offset(offset_of!(PropertyLookupCacheEntry, prototype)),
        property_lookup_cache_entry_prototype_chain_validity: offset(offset_of!(
            PropertyLookupCacheEntry,
            prototype_chain_validity
        )),
        property_lookup_cache_entry_type_get_missing_property: PropertyLookupCacheEntryType::GetMissingProperty as u32,
        property_lookup_cache_entry_type_add_own_property: PropertyLookupCacheEntryType::AddOwnProperty as u32,
        property_lookup_cache_entry_type_get_own_property: PropertyLookupCacheEntryType::GetOwnProperty as u32,
        property_lookup_cache_entry_type_change_own_property: PropertyLookupCacheEntryType::ChangeOwnProperty as u32,
        class_object_methods: offset(offset_of!(Class, object_methods)),
        object_methods_get_prototype_of: offset(offset_of!(ObjectMethods, internal_get_prototype_of)),
        property_lookup_cache_polymorphic_tag: PROPERTY_LOOKUP_CACHE_POLYMORPHIC_DATA_TAG as u64,
        property_lookup_cache_polymorphic_entry_count: MAX_NUMBER_OF_SHAPES_TO_REMEMBER as u32,
        property_lookup_cache_entry_size: offset(size_of::<PropertyLookupCacheEntry>()),
        property_lookup_cache_entry_key: offset(offset_of!(PropertyLookupCacheEntry, key)),
        property_lookup_cache_keyed_generic: PROPERTY_LOOKUP_CACHE_KEYED_GENERIC_DATA as u64,
        property_lookup_cache_entry_from_shape: offset(offset_of!(PropertyLookupCacheEntry, from_shape)),
        property_lookup_cache_megamorphic_tag: MEGAMORPHIC_DATA_TAG as u64,
        property_lookup_cache_megamorphic_primary_entries: offset(MEGAMORPHIC_PRIMARY_ENTRIES_OFFSET),
        property_lookup_cache_megamorphic_secondary_entries: offset(MEGAMORPHIC_SECONDARY_ENTRIES_OFFSET),
        property_lookup_cache_megamorphic_index_bits: MEGAMORPHIC_INDEX_BITS,
        property_lookup_cache_megamorphic_hash_multiplier: MEGAMORPHIC_HASH_MULTIPLIER,
        // NB: Filled in by runtime_info().
        try_get_by_id_cache: 0,
        try_put_by_id_cache: 0,
        keyed_lookup_cache_entries: 0,
        keyed_store_cache_entries: 0,
        keyed_lookup_cache_index_bits: 0,
        keyed_lookup_cache_entry_size: 0,
        keyed_lookup_cache_entry_type: 0,
        keyed_lookup_cache_property_offset: 0,
        keyed_lookup_cache_dictionary_generation: 0,
        keyed_lookup_cache_shape: 0,
        keyed_lookup_cache_name: 0,
    }
}

// NB: The compiler has its own copy of the bits of the interpreter's feedback.
const _: () = {
    assert!(libjs_jit::snapshot::arith_feedback::INT32 == crate::layout::feedback::arith_feedback::INT32);
    assert!(libjs_jit::snapshot::arith_feedback::DOUBLE == crate::layout::feedback::arith_feedback::DOUBLE);
    assert!(
        libjs_jit::snapshot::arith_feedback::INT32_OVERFLOW == crate::layout::feedback::arith_feedback::INT32_OVERFLOW
    );
    assert!(libjs_jit::snapshot::arith_feedback::STRING == crate::layout::feedback::arith_feedback::STRING);
    assert!(libjs_jit::snapshot::arith_feedback::BIG_INT == crate::layout::feedback::arith_feedback::BIG_INT);
    assert!(libjs_jit::snapshot::arith_feedback::OTHER == crate::layout::feedback::arith_feedback::OTHER);
    assert!(
        libjs_jit::snapshot::call_feedback_flags::POLYMORPHIC
            == crate::layout::feedback::call_feedback_flags::POLYMORPHIC
    );
    assert!(
        libjs_jit::snapshot::call_feedback_flags::FORWARDED_CLOSURES
            == crate::layout::feedback::call_feedback_flags::FORWARDED_CLOSURES
    );
    assert!(
        libjs_jit::snapshot::keyed_feedback_bits::INT32_INDEX
            == crate::layout::feedback::keyed_feedback_bits::INT32_INDEX
    );
    assert!(
        libjs_jit::snapshot::keyed_feedback_bits::STRING_KEY
            == crate::layout::feedback::keyed_feedback_bits::STRING_KEY
    );
    assert!(
        libjs_jit::snapshot::keyed_feedback_bits::SYMBOL_KEY
            == crate::layout::feedback::keyed_feedback_bits::SYMBOL_KEY
    );
    assert!(
        libjs_jit::snapshot::keyed_feedback_bits::OTHER_KEY == crate::layout::feedback::keyed_feedback_bits::OTHER_KEY
    );
    assert!(
        libjs_jit::snapshot::keyed_feedback_bits::KEY_KINDS_MASK
            == crate::layout::feedback::keyed_feedback_bits::KEY_KINDS_MASK
    );
    assert!(libjs_jit::snapshot::keyed_feedback_bits::PACKED == crate::layout::feedback::keyed_feedback_bits::PACKED);
    assert!(libjs_jit::snapshot::keyed_feedback_bits::HOLEY == crate::layout::feedback::keyed_feedback_bits::HOLEY);
    assert!(
        libjs_jit::snapshot::keyed_feedback_bits::OTHER_ELEMENTS
            == crate::layout::feedback::keyed_feedback_bits::OTHER_ELEMENTS
    );
    assert!(
        libjs_jit::snapshot::keyed_feedback_bits::OUT_OF_BOUNDS
            == crate::layout::feedback::keyed_feedback_bits::OUT_OF_BOUNDS
    );
    assert!(
        libjs_jit::snapshot::keyed_feedback_bits::TYPED_ARRAY_SHIFT
            == crate::layout::feedback::keyed_feedback_bits::TYPED_ARRAY_SHIFT
    );
};

// NB: JIT code reads the cells of the fly string cache as words, null if empty.
const _: () = assert!(size_of::<core::cell::Cell<Option<Gc<PrimitiveString>>>>() == size_of::<u64>());

/// The runtime info of a compile job for code of `realm`, with what JIT code needs to allocate cells itself (which
/// refers to the cells in `allocation.cells`).
pub fn runtime_info(vm: &Vm, realm: Gc<Realm>, allocation: AllocationInfos) -> RuntimeInfo {
    let address = |symbol: &str| {
        runtime_function_addresses()
            .iter()
            .find(|(name, _)| *name == symbol)
            .map(|(_, address)| *address as u64)
            .unwrap_or_else(|| panic!("the runtime has no function {symbol}"))
    };
    let mut layout = runtime_layout();
    layout.try_get_by_id_cache = address("asm_try_get_by_id_cache");
    layout.try_put_by_id_cache = address("asm_try_put_by_id_cache");
    let keyed_lookup_cache = vm.keyed_property_lookup_cache().jit_layout();
    layout.keyed_lookup_cache_entries = keyed_lookup_cache.entries;
    layout.keyed_store_cache_entries = vm.keyed_property_store_cache().jit_layout().entries;
    layout.keyed_lookup_cache_index_bits = keyed_lookup_cache.index_bits;
    layout.keyed_lookup_cache_entry_size = keyed_lookup_cache.entry_size;
    layout.keyed_lookup_cache_entry_type = keyed_lookup_cache.entry_type;
    layout.keyed_lookup_cache_property_offset = keyed_lookup_cache.property_offset;
    layout.keyed_lookup_cache_dictionary_generation = keyed_lookup_cache.shape_dictionary_generation;
    layout.keyed_lookup_cache_shape = keyed_lookup_cache.shape;
    layout.keyed_lookup_cache_name = keyed_lookup_cache.property_name;
    layout.single_ascii_character_strings = vm.single_ascii_character_strings_address();
    layout.fly_string_cache = vm.fly_string_cache().as_ptr().expose_provenance() as u64;
    layout.numeric_string_cache = vm.numeric_string_cache().as_ptr().expose_provenance() as u64;
    let strings = vm.cached_strings();
    let string = |string| Value::from_string(string).0;
    layout.typeof_strings = TypeofStrings {
        number: string(strings.number),
        undefined: string(strings.undefined),
        object: string(strings.object),
        string: string(strings.string),
        symbol: string(strings.symbol),
        boolean: string(strings.boolean),
        bigint: string(strings.bigint),
        function: string(strings.function),
    };
    let mut info = RuntimeInfo {
        // NB: Nothing notes the creation of objects with the `[[IsHTMLDDA]]` slot, so code cannot rely on there being none.
        no_htmldda_objects: false,
        slow_paths: vm.jit.slow_paths().to_vec(),
        jit_call: super::calls::libjs_jit_call as *const () as u64,
        call_forwarding_arguments: super::calls::libjs_jit_call_forwarding_arguments as *const () as u64,
        finish_direct_call: super::calls::libjs_jit_finish_direct_call as *const () as u64,
        raw_native_exception: if RAW_NATIVE_FUNCTIONS_RETURN_IN_REGISTERS {
            address("asm_helper_handle_raw_native_exception")
        } else {
            0
        },
        push_inlined_call_frames: super::entry_exit::libjs_jit_push_inlined_call_frames as *const () as u64,
        finish_inlined_direct_call: super::entry_exit::libjs_jit_finish_inlined_direct_call as *const () as u64,
        inlined_raw_native_exception: if RAW_NATIVE_FUNCTIONS_RETURN_IN_REGISTERS {
            super::entry_exit::libjs_jit_inlined_raw_native_exception as *const () as u64
        } else {
            0
        },
        array_push: super::calls::libjs_jit_array_push as *const () as u64,
        slice_arguments: super::calls::libjs_jit_slice_arguments as *const () as u64,
        jit_exit: super::entry_exit::libjs_jit_exit as *const () as u64,
        to_boolean: address("asm_helper_to_boolean"),
        primitive_to_string: super::calls::libjs_jit_primitive_to_string as *const () as u64,
        to_object: super::calls::libjs_jit_to_object as *const () as u64,
        array_create: super::calls::libjs_jit_array_create as *const () as u64,
        intrinsic_helpers: IntrinsicHelpers {
            has_own_property: super::intrinsics::libjs_jit_has_own_property as *const () as u64,
            has_property: super::intrinsics::libjs_jit_has_property as *const () as u64,
        },
        create_arguments: super::entry_exit::libjs_jit_create_arguments as *const () as u64,
        object_allocation: allocation.object,
        array_allocation: allocation.array,
        rope_allocation: allocation.rope,
        function_allocation: allocation.function,
        create_lexical_environment: super::allocation::libjs_jit_create_lexical_environment as *const () as u64,
        array_prototype: CellId(realm.array_prototype().as_ptr() as u64),
        object_prototype: CellId(realm.object_prototype().as_ptr() as u64),
        no_yield_continuation: ExecutionContext::NO_YIELD_CONTINUATION,
        heap_region_base: vm.head.heap_region_base.get() as u64,
        heap_region_offset_mask: HEAP_REGION_OFFSET_MASK,
        shifted_is_cell_pattern: nan_box::SHIFTED_IS_CELL_PATTERN,
        object_flag_is_function: object_flag::IS_FUNCTION,
        offsets: runtime_offsets(),
        layout,
        dynamic_calls: dynamic_call_layout(vm),
    };
    info.dynamic_calls.call_stub = vm.jit.call_stub(&info);
    info
}

/// Whether raw native functions return their completion in two registers, the way JIT code calls them. Elsewhere they
/// return it through a pointer, and JIT code leaves calls of them to the runtime.
pub const RAW_NATIVE_FUNCTIONS_RETURN_IN_REGISTERS: bool = !cfg!(any(
    all(target_arch = "x86_64", target_vendor = "apple"),
    target_os = "windows"
));

fn dynamic_call_layout(vm: &Vm) -> DynamicCallLayout {
    let entry_table = vm
        .jit
        .entry_table
        .as_ref()
        .expect("the VM has a JIT entry table while the JIT is on");
    DynamicCallLayout {
        object_flag_is_ecmascript_function: object_flag::IS_ECMASCRIPT_FUNCTION_OBJECT,
        ecmascript_function_shared_data: offset(offset_of!(EcmascriptFunctionObject, shared_data)),
        shared_data_executable: offset(offset_of!(SharedFunctionInstanceData, executable)),
        shared_data_asm_call_metadata: offset(offset_of!(SharedFunctionInstanceData, asm_call_metadata)),
        executable_jit_entry_slot: offset(offset_of!(ExecutableHead, jit_entry_slot)),
        jit_entry_table: entry_table.address(),
        jit_entry_slot_mask: JIT_ENTRY_SLOT_MASK,
        jit_entry_table_owners: offset(JIT_ENTRY_TABLE_OWNERS_OFFSET),
        metadata_can_inline_call: asm_call_metadata::CAN_INLINE_CALL,
        metadata_needs_environment_or_this_value_resolution:
            asm_call_metadata::NEEDS_ENVIRONMENT_OR_THIS_VALUE_RESOLUTION,
        metadata_uses_this: asm_call_metadata::USES_THIS,
        metadata_strict: asm_call_metadata::STRICT,
        executable_registers_and_locals_and_constants_count: offset(offset_of!(
            ExecutableHead,
            registers_and_locals_and_constants_count
        )),
        shape_realm: offset(offset_of!(Shape, realm)),
        object_flag_is_raw_native_function: if RAW_NATIVE_FUNCTIONS_RETURN_IN_REGISTERS {
            object_flag::IS_RAW_NATIVE_FUNCTION
        } else {
            0
        },
        raw_native_function_index: offset(offset_of!(RawNativeFunction, native_function_index)),
        vm_native_function_table: offset(offset_of!(VmHead, native_function_table_data)),
        native_function_table_index_mask: NATIVE_FUNCTION_TABLE_INDEX_MASK,
        native_function_table_entry_size: offset(size_of::<NativeFunctionTableEntry>()),
        native_function_table_entry_function: offset(offset_of!(NativeFunctionTableEntry, function)),
        prepare_call_environment: super::calls::libjs_jit_prepare_call_environment as *const () as u64,
        shared_data_call_environment_template: offset(offset_of!(
            SharedFunctionInstanceData,
            call_environment_template
        )),
        call_environment_template_size_class: offset(offset_of!(CallEnvironmentTemplate, size_class)),
        function_environment_free_lists: vm.jit.function_environment_free_lists.address(vm),
        function_environment_size_class_mask: offset(FUNCTION_ENVIRONMENT_FREE_LIST_COUNT - 1),
        call_environment_template_cell_size: offset(offset_of!(CallEnvironmentTemplate, cell_size)),
        call_environment_template_binding_values_offset: offset(offset_of!(
            CallEnvironmentTemplate,
            binding_values_offset
        )),
        call_environment_template_binds_this: offset(offset_of!(CallEnvironmentTemplate, binds_this)),
        call_environment_template_words: offset(offset_of!(CallEnvironmentTemplate, words)),
        function_environment_words: offset(FUNCTION_ENVIRONMENT_WORDS),
        function_environment_binding_values: offset(offset_of!(DeclarativeEnvironment, binding_values.data)),
        function_environment_outer: offset(offset_of!(Environment, outer)),
        function_environment_function_object: offset(FUNCTION_ENVIRONMENT_FUNCTION_OBJECT_OFFSET),
        function_environment_this_value: offset(FUNCTION_ENVIRONMENT_THIS_VALUE_OFFSET),
        call_stub: 0,
    }
}
