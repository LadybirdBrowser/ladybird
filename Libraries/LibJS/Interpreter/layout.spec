# Layout spec for the LibJS interpreter, read from the C++ headers by flapc.
# GC::Ref fields are inferred non-null; `nonnull` marks runtime-only invariants.

include <AK/Format.h>
include <AK/StringBase.h>
include <AK/Utf16StringData.h>
include <LibGC/PrimitiveStorage.h>
include <LibJS/Bytecode/Builtins.h>
include <LibJS/Bytecode/Executable.h>
include <LibJS/Bytecode/PropertyNameIterator.h>
include <LibJS/Bytecode/PutKind.h>
include <LibJS/Runtime/Accessor.h>
include <LibJS/Runtime/ArrayBuffer.h>
include <LibJS/Runtime/DeclarativeEnvironment.h>
include <LibJS/Runtime/ECMAScriptFunctionObject.h>
include <LibJS/Runtime/ExecutionContext.h>
include <LibJS/Runtime/FunctionObject.h>
include <LibJS/Runtime/GlobalEnvironment.h>
include <LibJS/Runtime/IndexedProperties.h>
include <LibJS/Runtime/NativeFunction.h>
include <LibJS/Runtime/Object.h>
include <LibJS/Runtime/PrimitiveString.h>
include <LibJS/Runtime/Realm.h>
include <LibJS/Runtime/Shape.h>
include <LibJS/Runtime/SharedFunctionInstanceData.h>
include <LibJS/Runtime/TypedArray.h>
include <LibJS/Runtime/VM.h>

namespace JS
namespace JS::Bytecode

prelude #if defined(HAS_ADDRESS_SANITIZER)
prelude constexpr unsigned long long flap_vm_stack_space_limit = 96 * KiB;
prelude #else
prelude constexpr unsigned long long flap_vm_stack_space_limit = 32 * KiB;
prelude #endif

type Object = Object
type Shape = Shape
type PropertyLookupCache = PropertyLookupCache
type Entry = PropertyLookupCache::Entry
type ObjectPropertyIteratorCacheData = ObjectPropertyIteratorCacheData
type ObjectPropertyIteratorCache = ObjectPropertyIteratorCache
type PropertyNameIterator = PropertyNameIterator
type Executable = Executable
type ExecutionContext = ExecutionContext
type InterpreterStack = InterpreterStack
type Realm = Realm
type VM = VM
type StackInfo = StackInfo
type PrototypeChainValidity = PrototypeChainValidity
type Accessor = Accessor
type DeclarativeEnvironment = DeclarativeEnvironment
type GlobalVariableCache = GlobalVariableCache
type FunctionObject = FunctionObject
type RawNativeFunction = RawNativeFunction
type DirectGetterFunction = DirectGetterFunction
type ECMAScriptFunctionObject = ECMAScriptFunctionObject
type SharedFunctionInstanceData = SharedFunctionInstanceData
type GlobalEnvironment = GlobalEnvironment
type PrimitiveString = PrimitiveString
type ShortString = AK::Detail::ShortString
type Utf16StringData = AK::Detail::Utf16StringData
type Environment = Environment
type PrivateEnvironment = PrivateEnvironment
type RareData = DeclarativeEnvironment::RareData
type EnvironmentShape = EnvironmentShape
type EnvironmentCoordinate = EnvironmentCoordinate
type TypedArrayBase = TypedArrayBase
type ByteLength = ByteLength
type NativeFunctionTableEntry = NativeFunctionTableEntry


section Object layout
field OBJECT_SHAPE Object.shape Shape = Object.m_shape
field OBJECT_NAMED_PROPERTIES Object.named_properties PropertyStorage = Object.m_named_properties nonnull
field OBJECT_INDEXED_ELEMENTS Object.indexed_elements IndexedElements = Object.m_indexed_elements
field OBJECT_INDEXED_STORAGE_KIND Object.indexed_storage_kind u8 = Object.m_indexed_storage_kind
field OBJECT_INDEXED_ARRAY_LIKE_SIZE Object.indexed_array_like_size u32 = Object.m_indexed_array_like_size
size OBJECT_SIZE = Object

section Accessor layout
field ACCESSOR_GETTER Accessor.getter FunctionObject = Accessor.m_getter

section Object flags
field OBJECT_FLAGS Object.flags u16 = Object.m_flags
const OBJECT_FLAG_HAS_MAGICAL_LENGTH = Object::Flag::HasMagicalLengthProperty
const OBJECT_FLAG_MAY_INTERFERE = Object::Flag::MayInterfereWithIndexedPropertyAccess
const OBJECT_FLAG_IS_TYPED_ARRAY = Object::Flag::IsTypedArray
const OBJECT_FLAG_IS_FUNCTION = Object::Flag::IsFunction
const OBJECT_FLAG_IS_ECMASCRIPT_FUNCTION_OBJECT = Object::Flag::IsECMAScriptFunctionObject
const OBJECT_FLAG_IS_RAW_NATIVE_FUNCTION = Object::Flag::IsRawNativeFunction
const OBJECT_FLAG_IS_DIRECT_GETTER_FUNCTION = Object::Flag::IsDirectGetterFunction
const OBJECT_FLAG_IS_GLOBAL_OBJECT = Object::Flag::IsGlobalObject

section Shape layout
field SHAPE_REALM Shape.realm Realm = Shape.m_realm
offset SHAPE_PROTOTYPE = Shape.m_prototype
field SHAPE_DICTIONARY_GENERATION Shape.dictionary_generation u32 = Shape.m_dictionary_generation
size SHAPE_SIZE = Shape

section PropertyLookupCache layout
offset PROPERTY_LOOKUP_CACHE_DATA = PropertyLookupCache.m_data
hex PROPERTY_LOOKUP_CACHE_DATA_POINTER_MASK = ~PropertyLookupCache::cache_data_tag_mask
size PROPERTY_LOOKUP_CACHE_SIZE = PropertyLookupCache

section PropertyLookupCache::Entry layout
field PROPERTY_LOOKUP_CACHE_ENTRY_TYPE PropertyLookupCache.entry_type u32 = Entry.type
const PROPERTY_LOOKUP_CACHE_ENTRY_TYPE_GET_MISSING_PROPERTY = to_underlying(PropertyLookupCache::Entry::Type::GetMissingProperty)
field PROPERTY_LOOKUP_CACHE_ENTRY_PROPERTY_OFFSET PropertyLookupCache.property_offset u32 = Entry.property_offset pair cache_details
field PROPERTY_LOOKUP_CACHE_ENTRY_DICTIONARY_GENERATION PropertyLookupCache.shape_dictionary_generation u32 = Entry.shape_dictionary_generation pair cache_details
field PROPERTY_LOOKUP_CACHE_ENTRY_DIRECT_GETTER_VALIDATED PropertyLookupCache.direct_getter_validated bool = Entry.direct_getter_validated
offset PROPERTY_LOOKUP_CACHE_ENTRY_FROM_SHAPE = Entry.from_shape
field PROPERTY_LOOKUP_CACHE_ENTRY_SHAPE PropertyLookupCache.shape Shape = Entry.shape pair cache_target
field PROPERTY_LOOKUP_CACHE_ENTRY_PROTOTYPE PropertyLookupCache.prototype Object = Entry.prototype pair cache_target
field PROPERTY_LOOKUP_CACHE_ENTRY_PROTOTYPE_CHAIN_VALIDITY PropertyLookupCache.prototype_chain_validity PrototypeChainValidity = Entry.prototype_chain_validity
size PROPERTY_LOOKUP_CACHE_ENTRY_SIZE = Entry

section ObjectPropertyIteratorCacheData layout
offset OBJECT_PROPERTY_ITERATOR_CACHE_DATA_PROPERTIES = ObjectPropertyIteratorCacheData.m_properties
offset OBJECT_PROPERTY_ITERATOR_CACHE_DATA_PROPERTY_VALUES = ObjectPropertyIteratorCacheData.m_property_values
field OBJECT_PROPERTY_ITERATOR_CACHE_DATA_SHAPE ObjectPropertyIteratorCacheData.shape Shape = ObjectPropertyIteratorCacheData.m_shape
field OBJECT_PROPERTY_ITERATOR_CACHE_DATA_PROTOTYPE_CHAIN_VALIDITY ObjectPropertyIteratorCacheData.prototype_chain_validity PrototypeChainValidity = ObjectPropertyIteratorCacheData.m_prototype_chain_validity
field OBJECT_PROPERTY_ITERATOR_CACHE_DATA_INDEXED_PROPERTY_COUNT ObjectPropertyIteratorCacheData.indexed_property_count u32 = ObjectPropertyIteratorCacheData.m_indexed_property_count
field OBJECT_PROPERTY_ITERATOR_CACHE_DATA_SHAPE_DICTIONARY_GENERATION ObjectPropertyIteratorCacheData.shape_dictionary_generation u32 = ObjectPropertyIteratorCacheData.m_shape_dictionary_generation
field OBJECT_PROPERTY_ITERATOR_CACHE_DATA_FAST_PATH ObjectPropertyIteratorCacheData.fast_path u8 = ObjectPropertyIteratorCacheData.m_fast_path

section ObjectPropertyIteratorCache layout
field OBJECT_PROPERTY_ITERATOR_CACHE_DATA_PTR ObjectPropertyIteratorCache.data ObjectPropertyIteratorCacheData = ObjectPropertyIteratorCache.data
field OBJECT_PROPERTY_ITERATOR_CACHE_REUSABLE_PROPERTY_NAME_ITERATOR ObjectPropertyIteratorCache.reusable_property_name_iterator Object = ObjectPropertyIteratorCache.reusable_property_name_iterator

section PropertyNameIterator layout
field PROPERTY_NAME_ITERATOR_OBJECT PropertyNameIterator.object Object = PropertyNameIterator.m_object
field PROPERTY_NAME_ITERATOR_PROPERTY_CACHE PropertyNameIterator.property_cache ObjectPropertyIteratorCacheData = PropertyNameIterator.m_property_cache pair cache_and_shape
field PROPERTY_NAME_ITERATOR_SHAPE PropertyNameIterator.shape Shape = PropertyNameIterator.m_shape pair cache_and_shape
field PROPERTY_NAME_ITERATOR_PROTOTYPE_CHAIN_VALIDITY PropertyNameIterator.prototype_chain_validity PrototypeChainValidity = PropertyNameIterator.m_prototype_chain_validity
field PROPERTY_NAME_ITERATOR_ITERATOR_CACHE_SLOT PropertyNameIterator.iterator_cache_slot ObjectPropertyIteratorCache = PropertyNameIterator.m_iterator_cache_slot
field PROPERTY_NAME_ITERATOR_INDEXED_PROPERTY_COUNT PropertyNameIterator.indexed_property_count u32 = PropertyNameIterator.m_indexed_property_count pair indexed_progress
field PROPERTY_NAME_ITERATOR_NEXT_INDEXED_PROPERTY PropertyNameIterator.next_indexed_property u32 = PropertyNameIterator.m_next_indexed_property pair indexed_progress
field PROPERTY_NAME_ITERATOR_NEXT_PROPERTY PropertyNameIterator.next_property u64 = PropertyNameIterator.m_next_property
field PROPERTY_NAME_ITERATOR_SHAPE_IS_DICTIONARY PropertyNameIterator.shape_is_dictionary bool = PropertyNameIterator.m_shape_is_dictionary
field PROPERTY_NAME_ITERATOR_SHAPE_DICTIONARY_GENERATION PropertyNameIterator.shape_dictionary_generation u32 = PropertyNameIterator.m_shape_dictionary_generation
field PROPERTY_NAME_ITERATOR_FAST_PATH PropertyNameIterator.fast_path u8 = PropertyNameIterator.m_fast_path

section Executable layout
offset EXECUTABLE_CONSTANTS = Executable.constants
offset EXECUTABLE_PROPERTY_LOOKUP_CACHES = Executable.property_lookup_caches
offset EXECUTABLE_GLOBAL_VARIABLE_CACHES = Executable.global_variable_caches
offset EXECUTABLE_ENVIRONMENT_COORDINATE_CACHES = Executable.environment_coordinate_caches
field EXECUTABLE_REGISTERS_AND_LOCALS_COUNT Executable.registers_and_locals_count u32 = Executable.registers_and_locals_count pair slot_counts
field EXECUTABLE_REGISTERS_AND_LOCALS_AND_CONSTANTS_COUNT Executable.registers_and_locals_and_constants_count u32 = Executable.registers_and_locals_and_constants_count pair slot_counts
field EXECUTABLE_ASM_CONSTANTS_SIZE Executable.asm_constants_size u64 = Executable.asm_constants_size pair constants
field EXECUTABLE_ASM_CONSTANTS_DATA Executable.asm_constants_data Sequence<Value> = Executable.asm_constants_data pair constants

section ExecutionContext layout
field EXECUTION_CONTEXT_FUNCTION ExecutionContext.function Object = ExecutionContext.function pair function_and_realm
field EXECUTION_CONTEXT_REALM ExecutionContext.realm Realm = ExecutionContext.realm pair function_and_realm
field EXECUTION_CONTEXT_SCRIPT_OR_MODULE ExecutionContext.script_or_module ScriptOrModule = ExecutionContext.script_or_module
field EXECUTION_CONTEXT_LEXICAL_ENVIRONMENT ExecutionContext.lexical_environment Environment = ExecutionContext.lexical_environment pair environments
field EXECUTION_CONTEXT_VARIABLE_ENVIRONMENT ExecutionContext.variable_environment Environment = ExecutionContext.variable_environment pair environments
field EXECUTION_CONTEXT_PRIVATE_ENVIRONMENT ExecutionContext.private_environment PrivateEnvironment = ExecutionContext.private_environment
field EXECUTION_CONTEXT_SKIP_WHEN_DETERMINING_INCUMBENT_COUNTER ExecutionContext.skip_when_determining_incumbent_counter u32 = ExecutionContext.skip_when_determining_incumbent_counter
field EXECUTION_CONTEXT_YIELD_CONTINUATION ExecutionContext.yield_continuation u32 = ExecutionContext.yield_continuation
field EXECUTION_CONTEXT_YIELD_IS_AWAIT ExecutionContext.yield_is_await bool = ExecutionContext.yield_is_await
field EXECUTION_CONTEXT_YIELD_VALUE_IS_ITERATOR_RESULT ExecutionContext.yield_value_is_iterator_result bool = ExecutionContext.yield_value_is_iterator_result
field EXECUTION_CONTEXT_CALLER_IS_CONSTRUCT ExecutionContext.caller_is_construct bool = ExecutionContext.caller_is_construct
field EXECUTION_CONTEXT_THIS_VALUE ExecutionContext.this_value Value = ExecutionContext.this_value pair this_and_executable
field EXECUTION_CONTEXT_EXECUTABLE ExecutionContext.executable Executable = ExecutionContext.executable pair this_and_executable
field EXECUTION_CONTEXT_CALLER_FRAME ExecutionContext.caller_frame ExecutionContext = ExecutionContext.caller_frame
field EXECUTION_CONTEXT_PASSED_ARGUMENT_COUNT ExecutionContext.passed_argument_count u32 = ExecutionContext.passed_argument_count
field EXECUTION_CONTEXT_CALLER_RETURN_PC ExecutionContext.caller_return_pc u32 = ExecutionContext.caller_return_pc pair caller_return
field EXECUTION_CONTEXT_CALLER_DST_RAW ExecutionContext.caller_dst_raw u32 = ExecutionContext.caller_dst_raw pair caller_return
field EXECUTION_CONTEXT_PROGRAM_COUNTER ExecutionContext.program_counter u32 = ExecutionContext.program_counter
field EXECUTION_CONTEXT_FRAME_ID ExecutionContext.frame_id u64 = ExecutionContext.frame_id
field EXECUTION_CONTEXT_REGISTERS_AND_CONSTANTS_AND_LOCALS_AND_ARGUMENTS_COUNT ExecutionContext.slot_count u32 = ExecutionContext.registers_and_constants_and_locals_and_arguments_count pair counts
field EXECUTION_CONTEXT_ARGUMENT_COUNT ExecutionContext.argument_count u32 = ExecutionContext.argument_count pair counts
size SIZEOF_EXECUTION_CONTEXT = ExecutionContext
raw field ExecutionContext.slots Sequence<Value> SIZEOF_EXECUTION_CONTEXT embedded scalar
const EXECUTION_CONTEXT_ACCUMULATOR = sizeof(ExecutionContext) + static_cast<size_t>(Register::accumulator().index()) * sizeof(Value)
const EXECUTION_CONTEXT_EXCEPTION = sizeof(ExecutionContext) + static_cast<size_t>(Register::exception().index()) * sizeof(Value)
const EXECUTION_CONTEXT_THIS_VALUE_REGISTER = sizeof(ExecutionContext) + static_cast<size_t>(Register::this_value().index()) * sizeof(Value)
const EXECUTION_CONTEXT_RETURN_VALUE = sizeof(ExecutionContext) + static_cast<size_t>(Register::return_value().index()) * sizeof(Value)
const EXECUTION_CONTEXT_SAVED_LEXICAL_ENVIRONMENT = sizeof(ExecutionContext) + static_cast<size_t>(Register::saved_lexical_environment().index()) * sizeof(Value)
raw field ExecutionContext.accumulator Value EXECUTION_CONTEXT_ACCUMULATOR nullable scalar accumulator_and_exception
raw field ExecutionContext.exception Value EXECUTION_CONTEXT_EXCEPTION nullable scalar accumulator_and_exception pinned values EXCEPTION_REG_OFFSET
raw field ExecutionContext.this_value_register Value EXECUTION_CONTEXT_THIS_VALUE_REGISTER nullable scalar pinned values THIS_VALUE_REG_OFFSET
raw field ExecutionContext.return_value Value EXECUTION_CONTEXT_RETURN_VALUE nullable scalar return_and_saved_environment
raw field ExecutionContext.saved_lexical_environment Value EXECUTION_CONTEXT_SAVED_LEXICAL_ENVIRONMENT nullable scalar return_and_saved_environment
const ALIGNOF_EXECUTION_CONTEXT = alignof(ExecutionContext)
const EXECUTION_CONTEXT_NO_YIELD_CONTINUATION = static_cast<u32>(ExecutionContext::no_yield_continuation)
const SIZEOF_SCRIPT_OR_MODULE = sizeof(ScriptOrModule)
const SIZEOF_VALUE = sizeof(Value)

section InterpreterStack layout
offset INTERPRETER_STACK_LIMIT = InterpreterStack.m_limit
offset INTERPRETER_STACK_TOP = InterpreterStack.m_top
offset INTERPRETER_STACK_NEXT_FRAME_ID = InterpreterStack.m_next_frame_id

section Realm layout
field REALM_GLOBAL_ENVIRONMENT Realm.global_environment GlobalEnvironment = Realm.m_global_environment nonnull
field REALM_GLOBAL_OBJECT Realm.global_object Object = Realm.m_global_object pair global_state
field REALM_GLOBAL_DECLARATIVE_ENVIRONMENT Realm.global_declarative_environment DeclarativeEnvironment = Realm.m_global_declarative_environment pair global_state

section VM layout
offset VM_RUNNING_EXECUTION_CONTEXT = VM.m_running_execution_context
offset VM_INTERPRETER_STACK = VM.m_interpreter_stack
offset VM_STACK_INFO = VM.m_stack_info
offset VM_EXECUTION_GENERATION = VM.m_execution_generation
offset VM_PRIMITIVE_STORAGE_CAGE_BASE = VM.m_primitive_storage_cage_base
offset VM_HEAP_REGION_BASE = VM.m_heap_region_base
offset VM_NATIVE_FUNCTION_TABLE_DATA = VM.m_native_function_table_data
offset VM_BREAKPOINT_CONTROLLER = VM.m_debugger
raw field VM.primitive_storage_cage_base u64 VM_PRIMITIVE_STORAGE_CAGE_BASE nonnull scalar
raw field VM.heap_region_base u64 VM_HEAP_REGION_BASE nonnull scalar
raw field VM.native_function_table Sequence<NativeFunctionTableEntry> VM_NATIVE_FUNCTION_TABLE_DATA nonnull scalar
const VM_INTERPRETER_STACK_TOP = offsetof(VM, m_interpreter_stack) + offsetof(InterpreterStack, m_top)
const VM_INTERPRETER_STACK_LIMIT = offsetof(VM, m_interpreter_stack) + offsetof(InterpreterStack, m_limit)
const VM_INTERPRETER_STACK_NEXT_FRAME_ID = offsetof(VM, m_interpreter_stack) + offsetof(InterpreterStack, m_next_frame_id)
const VM_STACK_INFO_BASE = offsetof(VM, m_stack_info) + offsetof(StackInfo, m_base)
raw field VM.running_execution_context ExecutionContext VM_RUNNING_EXECUTION_CONTEXT nullable scalar
raw field VM.interpreter_stack_top u64 VM_INTERPRETER_STACK_TOP nonnull scalar interpreter_stack_bounds
raw field VM.interpreter_stack_limit u64 VM_INTERPRETER_STACK_LIMIT nonnull scalar interpreter_stack_bounds
raw field VM.interpreter_stack_next_frame_id u64 VM_INTERPRETER_STACK_NEXT_FRAME_ID nonnull scalar
raw field VM.stack_base u64 VM_STACK_INFO_BASE nullable scalar
const VM_STACK_SPACE_LIMIT = flap_vm_stack_space_limit

section StackInfo layout
offset STACK_INFO_BASE = StackInfo.m_base

section IndexedStorageKind enum values
const INDEXED_STORAGE_KIND_NONE = static_cast<u8>(IndexedStorageKind::None)
const INDEXED_STORAGE_KIND_PACKED = static_cast<u8>(IndexedStorageKind::Packed)
const INDEXED_STORAGE_KIND_HOLEY = static_cast<u8>(IndexedStorageKind::Holey)
const INDEXED_STORAGE_KIND_DICTIONARY = static_cast<u8>(IndexedStorageKind::Dictionary)

section ObjectPropertyIteratorFastPath enum values
const OBJECT_PROPERTY_ITERATOR_FAST_PATH_NONE = static_cast<u8>(ObjectPropertyIteratorFastPath::None)
const OBJECT_PROPERTY_ITERATOR_FAST_PATH_PLAIN_NAMED = static_cast<u8>(ObjectPropertyIteratorFastPath::PlainNamed)
const OBJECT_PROPERTY_ITERATOR_FAST_PATH_PACKED_INDEXED = static_cast<u8>(ObjectPropertyIteratorFastPath::PackedIndexed)

section Vector<Value> layout
const VECTOR_DATA = __builtin_offsetof(Vector<Value>, m_metadata.outline_buffer)
const VECTOR_SIZE = __builtin_offsetof(Vector<Value>, m_size)
const INDEXED_ELEMENTS_CAPACITY = static_cast<ptrdiff_t>(__builtin_offsetof(Vector<Value>, m_capacity)) - static_cast<ptrdiff_t>(__builtin_offsetof(Vector<Value>, m_metadata.outline_buffer))
raw field IndexedElements.capacity u32 INDEXED_ELEMENTS_CAPACITY nullable scalar
const EXECUTABLE_BYTECODE_DATA = offsetof(Executable, bytecode) + InstructionStream::data_member_offset()
raw field Executable.bytecode_data u64 EXECUTABLE_BYTECODE_DATA nonnull scalar
const EXECUTABLE_PROPERTY_LOOKUP_CACHES_DATA = offsetof(Executable, property_lookup_caches) + __builtin_offsetof(Vector<Value>, m_metadata.outline_buffer)
raw field Executable.property_lookup_caches PropertyLookupCaches EXECUTABLE_PROPERTY_LOOKUP_CACHES_DATA nonnull scalar
const EXECUTABLE_GLOBAL_VARIABLE_CACHES_DATA = offsetof(Executable, global_variable_caches) + __builtin_offsetof(Vector<Value>, m_metadata.outline_buffer)
raw field Executable.global_variable_caches GlobalVariableCaches EXECUTABLE_GLOBAL_VARIABLE_CACHES_DATA nonnull scalar
const EXECUTABLE_ENVIRONMENT_COORDINATE_CACHES_DATA = offsetof(Executable, environment_coordinate_caches) + __builtin_offsetof(Vector<Value>, m_metadata.outline_buffer)
raw field Executable.environment_coordinate_caches Sequence<EnvironmentCoordinateEntry> EXECUTABLE_ENVIRONMENT_COORDINATE_CACHES_DATA nullable scalar
const EXECUTABLE_CONSTANTS_DATA = offsetof(Executable, constants) + __builtin_offsetof(Vector<Value>, m_metadata.outline_buffer)
const EXECUTABLE_CONSTANTS_SIZE = offsetof(Executable, constants) + __builtin_offsetof(Vector<Value>, m_size)
const OBJECT_PROPERTY_ITERATOR_CACHE_DATA_PROPERTY_VALUES_DATA = offsetof(ObjectPropertyIteratorCacheData, m_property_values) + __builtin_offsetof(Vector<Value>, m_metadata.outline_buffer)
const OBJECT_PROPERTY_ITERATOR_CACHE_DATA_PROPERTY_VALUES_SIZE = offsetof(ObjectPropertyIteratorCacheData, m_property_values) + __builtin_offsetof(Vector<Value>, m_size)
raw field ObjectPropertyIteratorCacheData.property_values Sequence<Value> OBJECT_PROPERTY_ITERATOR_CACHE_DATA_PROPERTY_VALUES_DATA nullable scalar
raw field ObjectPropertyIteratorCacheData.property_value_count u64 OBJECT_PROPERTY_ITERATOR_CACHE_DATA_PROPERTY_VALUES_SIZE nullable scalar

section PutKind enum
const PUT_KIND_NORMAL = static_cast<u8>(JS::Bytecode::PutKind::Normal)

section PrototypeChainValidity layout
field PROTOTYPE_CHAIN_VALIDITY_VALID PrototypeChainValidity.valid bool = PrototypeChainValidity.m_valid

section DeclarativeEnvironment layout
field DECLARATIVE_ENVIRONMENT_RARE_DATA DeclarativeEnvironment.rare_data DeclarativeEnvironmentRareData = DeclarativeEnvironment.m_rare_data
field DECLARATIVE_ENVIRONMENT_SERIAL DeclarativeEnvironment.serial_number u64 = DeclarativeEnvironment.m_environment_serial_number

section GlobalVariableCache layout
offset GLOBAL_VARIABLE_CACHE_ENTRY = GlobalVariableCache.entry
offset GLOBAL_VARIABLE_CACHE_ENVIRONMENT_SERIAL = GlobalVariableCache.environment_serial_number
offset GLOBAL_VARIABLE_CACHE_ENVIRONMENT_BINDING_INDEX = GlobalVariableCache.environment_binding_index
offset GLOBAL_VARIABLE_CACHE_HAS_ENVIRONMENT_BINDING = GlobalVariableCache.has_environment_binding_index
offset GLOBAL_VARIABLE_CACHE_IN_MODULE_ENVIRONMENT = GlobalVariableCache.in_module_environment
size GLOBAL_VARIABLE_CACHE_SIZE = GlobalVariableCache
const GLOBAL_VARIABLE_CACHE_ENTRY_PROPERTY_OFFSET = offsetof(GlobalVariableCache, entry) + offsetof(PropertyLookupCache::Entry, property_offset)
const GLOBAL_VARIABLE_CACHE_ENTRY_DICTIONARY_GENERATION = offsetof(GlobalVariableCache, entry) + offsetof(PropertyLookupCache::Entry, shape_dictionary_generation)
const GLOBAL_VARIABLE_CACHE_ENTRY_SHAPE = offsetof(GlobalVariableCache, entry) + offsetof(PropertyLookupCache::Entry, shape)
raw field GlobalVariableCache.property_offset u32 GLOBAL_VARIABLE_CACHE_ENTRY_PROPERTY_OFFSET nullable scalar global_cache_details stride GLOBAL_VARIABLE_CACHE_SIZE
raw field GlobalVariableCache.shape_dictionary_generation u32 GLOBAL_VARIABLE_CACHE_ENTRY_DICTIONARY_GENERATION nullable scalar global_cache_details
raw field GlobalVariableCache.shape Shape GLOBAL_VARIABLE_CACHE_ENTRY_SHAPE nullable cell
raw field GlobalVariableCache.environment_serial_number u64 GLOBAL_VARIABLE_CACHE_ENVIRONMENT_SERIAL nullable scalar
raw field GlobalVariableCache.environment_binding_index u32 GLOBAL_VARIABLE_CACHE_ENVIRONMENT_BINDING_INDEX nullable scalar
raw field GlobalVariableCache.has_environment_binding_index u8 GLOBAL_VARIABLE_CACHE_HAS_ENVIRONMENT_BINDING nullable scalar
raw field GlobalVariableCache.in_module_environment u8 GLOBAL_VARIABLE_CACHE_IN_MODULE_ENVIRONMENT nullable scalar

section Builtin enum values
const BUILTIN_MATH_ABS = static_cast<u8>(Bytecode::Builtin::MathAbs)
const BUILTIN_MATH_FLOOR = static_cast<u8>(Bytecode::Builtin::MathFloor)
const BUILTIN_MATH_CEIL = static_cast<u8>(Bytecode::Builtin::MathCeil)
const BUILTIN_MATH_ROUND = static_cast<u8>(Bytecode::Builtin::MathRound)
const BUILTIN_MATH_SQRT = static_cast<u8>(Bytecode::Builtin::MathSqrt)
const BUILTIN_MATH_EXP = static_cast<u8>(Bytecode::Builtin::MathExp)
const BUILTIN_STRING_FROM_CHAR_CODE = static_cast<u8>(Bytecode::Builtin::StringFromCharCode)
const BUILTIN_STRING_PROTOTYPE_CHAR_CODE_AT = static_cast<u8>(Bytecode::Builtin::StringPrototypeCharCodeAt)
const BUILTIN_STRING_PROTOTYPE_CHAR_AT = static_cast<u8>(Bytecode::Builtin::StringPrototypeCharAt)

section FunctionObject layout
offset FUNCTION_OBJECT_BUILTIN = FunctionObject.m_builtin
const FUNCTION_OBJECT_BUILTIN_VALUE = offsetof(FunctionObject, m_builtin)
const FUNCTION_OBJECT_BUILTIN_HAS_VALUE = offsetof(FunctionObject, m_builtin) + 1
raw field FunctionObject.builtin u8 FUNCTION_OBJECT_BUILTIN_VALUE nullable scalar
raw field FunctionObject.has_builtin bool FUNCTION_OBJECT_BUILTIN_HAS_VALUE nullable scalar

section RawNativeFunction layout
field RAW_NATIVE_FUNCTION_NATIVE_FUNCTION_INDEX RawNativeFunction.native_function_index u32 = RawNativeFunction.m_native_function_index
offset NATIVE_FUNCTION_TABLE_ENTRY_FUNCTION = NativeFunctionTableEntry.function
offset NATIVE_FUNCTION_TABLE_ENTRY_TYPE = NativeFunctionTableEntry.type
size NATIVE_FUNCTION_TABLE_ENTRY_SIZE = NativeFunctionTableEntry
const NATIVE_FUNCTION_TYPE_COUNT = to_underlying(NativeFunctionType::RawNativeFunction) + 1
raw field NativeFunctionTableEntry.function u64 NATIVE_FUNCTION_TABLE_ENTRY_FUNCTION nonnull scalar native_function_table_entry stride {NATIVE_FUNCTION_TABLE_ENTRY_SIZE}
raw field NativeFunctionTableEntry.type u32 NATIVE_FUNCTION_TABLE_ENTRY_TYPE nullable scalar native_function_table_entry

section DirectGetterFunction layout
field DIRECT_GETTER_FUNCTION_WRAPPER_IMPLEMENTATION_WORD_OFFSET DirectGetterFunction.wrapper_implementation_word_offset u32 = DirectGetterFunction.m_wrapper_implementation_word_offset
field DIRECT_GETTER_FUNCTION_IMPLEMENTATION_VALUE_WORD_OFFSET DirectGetterFunction.implementation_value_word_offset u32 = DirectGetterFunction.m_implementation_value_word_offset
field DIRECT_GETTER_FUNCTION_MAIN_WORLD_WRAPPER_WORD_OFFSET DirectGetterFunction.main_world_wrapper_word_offset u32 = DirectGetterFunction.m_main_world_wrapper_word_offset
field DIRECT_GETTER_FUNCTION_WEAK_IMPL_VALUE_WORD_OFFSET DirectGetterFunction.weak_impl_value_word_offset u32 = DirectGetterFunction.m_weak_impl_value_word_offset

section ECMAScriptFunctionObject layout
field ECMASCRIPT_FUNCTION_OBJECT_SHARED_DATA ECMAScriptFunctionObject.shared_data SharedFunctionInstanceData = ECMAScriptFunctionObject.m_shared_data
field ECMASCRIPT_FUNCTION_OBJECT_ENVIRONMENT ECMAScriptFunctionObject.environment Environment = ECMAScriptFunctionObject.m_environment pair environment_state
field ECMASCRIPT_FUNCTION_OBJECT_PRIVATE_ENVIRONMENT ECMAScriptFunctionObject.private_environment PrivateEnvironment = ECMAScriptFunctionObject.m_private_environment pair environment_state
field ECMASCRIPT_FUNCTION_OBJECT_SCRIPT_OR_MODULE ECMAScriptFunctionObject.script_or_module ScriptOrModule = ECMAScriptFunctionObject.m_script_or_module

section SharedFunctionInstanceData layout
field SHARED_FUNCTION_INSTANCE_DATA_EXECUTABLE SharedFunctionInstanceData.executable Executable = SharedFunctionInstanceData.m_executable pair call_data
field SHARED_FUNCTION_INSTANCE_DATA_ASM_CALL_METADATA SharedFunctionInstanceData.asm_call_metadata u64 = SharedFunctionInstanceData.m_asm_call_metadata pair call_data
offset SHARED_FUNCTION_INSTANCE_DATA_FORMAL_PARAMETER_COUNT = SharedFunctionInstanceData.m_formal_parameter_count
offset SHARED_FUNCTION_INSTANCE_DATA_STRICT = SharedFunctionInstanceData.m_strict
offset SHARED_FUNCTION_INSTANCE_DATA_FUNCTION_ENVIRONMENT_NEEDED = SharedFunctionInstanceData.m_function_environment_needed
offset SHARED_FUNCTION_INSTANCE_DATA_USES_THIS = SharedFunctionInstanceData.m_uses_this
offset SHARED_FUNCTION_INSTANCE_DATA_CAN_INLINE_CALL = SharedFunctionInstanceData.m_can_inline_call
const SHARED_FUNCTION_INSTANCE_DATA_ASM_CALL_METADATA_CAN_INLINE_CALL = static_cast<u64>(SharedFunctionInstanceData::asm_call_metadata_can_inline_call)
const SHARED_FUNCTION_INSTANCE_DATA_ASM_CALL_METADATA_NEEDS_ENVIRONMENT_OR_THIS_VALUE_RESOLUTION = static_cast<u64>(SharedFunctionInstanceData::asm_call_metadata_needs_environment_or_this_value_resolution)
const SHARED_FUNCTION_INSTANCE_DATA_ASM_CALL_METADATA_USES_THIS = static_cast<u64>(SharedFunctionInstanceData::asm_call_metadata_uses_this)
const SHARED_FUNCTION_INSTANCE_DATA_ASM_CALL_METADATA_STRICT = static_cast<u64>(SharedFunctionInstanceData::asm_call_metadata_strict)

section GlobalEnvironment layout
field GLOBAL_ENVIRONMENT_GLOBAL_THIS_VALUE GlobalEnvironment.global_this_value Object = GlobalEnvironment.m_global_this_value

section PrimitiveString layout
field PRIMITIVE_STRING_DEFERRED_KIND PrimitiveString.deferred_kind u8 = PrimitiveString.m_deferred_kind
offset PRIMITIVE_STRING_UTF16_STRING = PrimitiveString.m_utf16_string
raw field PrimitiveString.utf16_data Utf16StringData PRIMITIVE_STRING_UTF16_STRING nullable scalar
const PRIMITIVE_STRING_DEFERRED_KIND_NONE = static_cast<u8>(PrimitiveString::DeferredKind::None)

section Utf16String layout
const UTF16_SHORT_STRING_FLAG = AK::Detail::StringBase::SHORT_STRING_FLAG
const UTF16_SHORT_STRING_BYTE_COUNT_SHIFT_COUNT = AK::Detail::StringBase::SHORT_STRING_BYTE_COUNT_SHIFT_COUNT
offset UTF16_SHORT_STRING_BYTE_COUNT_AND_FLAG = ShortString.byte_count_and_short_string_flag
offset UTF16_SHORT_STRING_STORAGE = ShortString.storage
const PRIMITIVE_STRING_UTF16_SHORT_STRING_BYTE_COUNT_AND_FLAG = offsetof(PrimitiveString, m_utf16_string) + offsetof(AK::Detail::ShortString, byte_count_and_short_string_flag)
const PRIMITIVE_STRING_UTF16_SHORT_STRING_STORAGE = offsetof(PrimitiveString, m_utf16_string) + offsetof(AK::Detail::ShortString, storage)
raw field PrimitiveString.utf16_short_string_byte_count_and_flag u8 PRIMITIVE_STRING_UTF16_SHORT_STRING_BYTE_COUNT_AND_FLAG nullable scalar
raw field PrimitiveString.utf16_short_string_storage Sequence<u8> PRIMITIVE_STRING_UTF16_SHORT_STRING_STORAGE embedded scalar

section Utf16StringData layout
const UTF16_STRING_DATA_LENGTH_IN_CODE_UNITS = __builtin_offsetof(AK::Detail::Utf16StringData, m_header.length_in_code_units)
const UTF16_STRING_DATA_FLAGS = __builtin_offsetof(AK::Detail::Utf16StringData, m_header.flags)
const UTF16_STRING_DATA_STRING_STORAGE = AK::Detail::Utf16StringData::offset_of_string_storage()
const UTF16_STRING_DATA_HAS_UTF16_STORAGE = static_cast<u32>(AK::Detail::Utf16StringData::HasUtf16Storage)
raw field Utf16StringData.length_in_code_units u32 UTF16_STRING_DATA_LENGTH_IN_CODE_UNITS nullable scalar
raw field Utf16StringData.flags u32 UTF16_STRING_DATA_FLAGS nullable scalar
raw field Utf16StringData.string_storage Sequence<u8> UTF16_STRING_DATA_STRING_STORAGE embedded scalar

section Environment layout
field ENVIRONMENT_SCREWED_BY_EVAL Environment.permanently_screwed_by_eval bool = Environment.m_permanently_screwed_by_eval
field ENVIRONMENT_DECLARATIVE Environment.declarative bool = Environment.m_declarative
field ENVIRONMENT_OUTER Environment.outer Environment = Environment.m_outer_environment

section PrivateEnvironment layout
field PRIVATE_ENVIRONMENT_OUTER PrivateEnvironment.outer PrivateEnvironment = PrivateEnvironment.m_outer_environment

section DeclarativeEnvironment binding storage layout
field DECLARATIVE_ENVIRONMENT_SHAPE DeclarativeEnvironment.shape EnvironmentShape = DeclarativeEnvironment.m_shape
offset DECLARATIVE_ENVIRONMENT_BINDING_VALUES = DeclarativeEnvironment.m_binding_values
offset DECLARATIVE_ENVIRONMENT_RARE_DATA_BINDING_FLAGS = RareData.m_binding_flags
offset ENVIRONMENT_SHAPE_BINDING_FLAGS = EnvironmentShape.m_binding_flags
const BINDING_FLAG_MUTABLE = 1 << 1
const BINDING_VALUES_DATA_PTR = offsetof(DeclarativeEnvironment, m_binding_values) + __builtin_offsetof(decltype(DeclarativeEnvironment::m_binding_values), m_metadata.outline_buffer)
raw field DeclarativeEnvironment.binding_values BindingValues BINDING_VALUES_DATA_PTR nonnull scalar
const BINDING_FLAGS_DATA_PTR = offsetof(DeclarativeEnvironment::RareData, m_binding_flags) + __builtin_offsetof(decltype(DeclarativeEnvironment::RareData::m_binding_flags), m_metadata.outline_buffer)
raw field DeclarativeEnvironmentRareData.binding_flags BindingFlags BINDING_FLAGS_DATA_PTR nonnull scalar
const ENVIRONMENT_SHAPE_BINDING_FLAGS_DATA_PTR = offsetof(EnvironmentShape, m_binding_flags) + __builtin_offsetof(decltype(EnvironmentShape::m_binding_flags), m_metadata.outline_buffer)
const ENVIRONMENT_SHAPE_BINDING_FLAGS_SIZE = offsetof(EnvironmentShape, m_binding_flags) + __builtin_offsetof(decltype(EnvironmentShape::m_binding_flags), m_size)
raw field EnvironmentShape.binding_flags_size u64 ENVIRONMENT_SHAPE_BINDING_FLAGS_SIZE nullable scalar
raw field EnvironmentShape.binding_flags BindingFlags ENVIRONMENT_SHAPE_BINDING_FLAGS_DATA_PTR nonnull scalar

section EnvironmentCoordinate layout
const ENVIRONMENT_COORDINATE_HOPS = offsetof(EnvironmentCoordinate, hops)
const ENVIRONMENT_COORDINATE_INDEX = offsetof(EnvironmentCoordinate, index)
hex ENVIRONMENT_COORDINATE_INVALID = EnvironmentCoordinate::invalid_marker
size ENVIRONMENT_COORDINATE_SIZE = EnvironmentCoordinate
raw field EnvironmentCoordinateEntry.hops u32 ENVIRONMENT_COORDINATE_HOPS nullable scalar environment_coordinate stride {ENVIRONMENT_COORDINATE_SIZE}
raw field EnvironmentCoordinateEntry.binding_index u32 ENVIRONMENT_COORDINATE_INDEX nullable scalar environment_coordinate

section TypedArrayBase layout
offset TYPED_ARRAY_ELEMENT_SIZE = TypedArrayBase.m_element_size
offset TYPED_ARRAY_ARRAY_LENGTH = TypedArrayBase.m_array_length
offset TYPED_ARRAY_BYTE_OFFSET = TypedArrayBase.m_byte_offset
field TYPED_ARRAY_KIND Object.typed_array_kind u8 = TypedArrayBase.m_kind
field TYPED_ARRAY_CACHED_DATA_OFFSET Object.typed_array_cached_data_offset u64 = TypedArrayBase.m_cached_data_offset
hex TYPED_ARRAY_CACHED_DATA_OFFSET_INVALID = static_cast<size_t>(TypedArrayBase::invalid_cached_data_offset)
hex PRIMITIVE_STORAGE_CAGE_OFFSET_MASK = static_cast<size_t>(GC::PrimitiveStorage::cage_offset_mask)
hex HEAP_REGION_OFFSET_MASK = static_cast<size_t>(GC::HEAP_REGION_OFFSET_MASK)

section ByteLength layout
raw const BYTE_LENGTH_U32_INDEX = 2
size BYTE_LENGTH_SIZE = ByteLength
const TYPED_ARRAY_ARRAY_LENGTH_VALUE = offsetof(TypedArrayBase, m_array_length)
raw field Object.typed_array_array_length u32 TYPED_ARRAY_ARRAY_LENGTH_VALUE nullable scalar
const TYPED_ARRAY_ARRAY_LENGTH_INDEX = offsetof(TypedArrayBase, m_array_length) + 4

section TypedArrayBase::Kind values
const TYPED_ARRAY_KIND_UINT8 = static_cast<u8>(TypedArrayBase::Kind::Uint8Array)
const TYPED_ARRAY_KIND_UINT8_CLAMPED = static_cast<u8>(TypedArrayBase::Kind::Uint8ClampedArray)
const TYPED_ARRAY_KIND_UINT16 = static_cast<u8>(TypedArrayBase::Kind::Uint16Array)
const TYPED_ARRAY_KIND_UINT32 = static_cast<u8>(TypedArrayBase::Kind::Uint32Array)
const TYPED_ARRAY_KIND_INT8 = static_cast<u8>(TypedArrayBase::Kind::Int8Array)
const TYPED_ARRAY_KIND_INT16 = static_cast<u8>(TypedArrayBase::Kind::Int16Array)
const TYPED_ARRAY_KIND_INT32 = static_cast<u8>(TypedArrayBase::Kind::Int32Array)
const TYPED_ARRAY_KIND_FLOAT32 = static_cast<u8>(TypedArrayBase::Kind::Float32Array)
const TYPED_ARRAY_KIND_FLOAT64 = static_cast<u8>(TypedArrayBase::Kind::Float64Array)

section Value tags
hex OBJECT_TAG = static_cast<u64>(OBJECT_TAG)
hex STRING_TAG = static_cast<u64>(STRING_TAG)
hex SYMBOL_TAG = static_cast<u64>(SYMBOL_TAG)
hex BIGINT_TAG = static_cast<u64>(BIGINT_TAG)
hex ACCESSOR_TAG = static_cast<u64>(ACCESSOR_TAG)
hex IS_CELL_PATTERN = static_cast<u64>(GC::IS_CELL_PATTERN)
hex INT32_TAG = static_cast<u64>(INT32_TAG)
hex BOOLEAN_TAG = static_cast<u64>(BOOLEAN_TAG)
hex UNDEFINED_TAG = static_cast<u64>(UNDEFINED_TAG)
hex NULL_TAG = static_cast<u64>(NULL_TAG)

section Shifted value constants
hex OBJECT_TAG_SHIFTED = static_cast<u64>(OBJECT_TAG << GC::TAG_SHIFT)
hex EMPTY_VALUE = static_cast<u64>(EMPTY_TAG << GC::TAG_SHIFT)
hex INT32_TAG_SHIFTED = static_cast<u64>(INT32_TAG << GC::TAG_SHIFT)
hex BOOLEAN_TRUE = static_cast<u64>((BOOLEAN_TAG << GC::TAG_SHIFT) | 1)
hex BOOLEAN_FALSE = static_cast<u64>(BOOLEAN_TAG << GC::TAG_SHIFT)
hex UNDEFINED_SHIFTED = static_cast<u64>(UNDEFINED_TAG << GC::TAG_SHIFT)
hex NULL_VALUE = static_cast<u64>(NULL_TAG << GC::TAG_SHIFT)
hex EMPTY_TAG_SHIFTED = static_cast<u64>(EMPTY_TAG << GC::TAG_SHIFT)
hex NAN_BASE_TAG = static_cast<u64>(GC::BASE_TAG)
hex CANON_NAN_BITS = static_cast<u64>(GC::CANON_NAN_BITS)
hex DOUBLE_ONE = bit_cast<u64>(1.0)
hex NEGATIVE_ZERO = static_cast<u64>(NEGATIVE_ZERO_BITS)
hex SHIFTED_IS_CELL_PATTERN = static_cast<u64>(GC::SHIFTED_IS_CELL_PATTERN)
const ACCUMULATOR_REG_OFFSET = static_cast<size_t>(Register::accumulator().index()) * sizeof(Value)
const EXCEPTION_REG_OFFSET = static_cast<size_t>(Register::exception().index()) * sizeof(Value)
const THIS_VALUE_REG_OFFSET = static_cast<size_t>(Register::this_value().index()) * sizeof(Value)
const RETURN_VALUE_REG_OFFSET = static_cast<size_t>(Register::return_value().index()) * sizeof(Value)
const SAVED_LEXICAL_ENVIRONMENT_REG_OFFSET = static_cast<size_t>(Register::saved_lexical_environment().index()) * sizeof(Value)
const RESERVED_REGISTER_COUNT = static_cast<size_t>(Register::reserved_register_count)
