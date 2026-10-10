/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! How JIT code allocates cells itself (the allocation contract of `libjs_jit::snapshot::ObjectAllocationInfo` and
//! its siblings), and the runtime functions it calls when it cannot: plain objects in their size classes, arrays with
//! packed elements in ValueStorage cells, rope strings and the function objects of closures.
//!
//! JIT code pops the local free lists of the heap's allocators inline, the same way the heap itself allocates (see
//! gc_heap_allocator_local_free_list()). The contract's `allocator` of a size class is the address of the head of its
//! local free list, so its `local_free_list_offset` is 0.

use core::ptr::NonNull;

use libjs_jit::snapshot::{
    ArrayAllocationInfo, CellId, ClosureTemplateSnapshot, EnvironmentTemplateSnapshot, FunctionAllocationInfo,
    LexicalEnvironmentTemplateSnapshot, ObjectAllocationInfo, ObjectSizeClass, RopeAllocationInfo, StorageSizeClass,
};

use super::offset;
use crate::bytecode::executable::Executable;
use crate::gc::class::{GcCell, class_of};
use crate::interpreter::slow_paths::bindings::create_lexical_environment;
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::environment::{DeclarativeEnvironment, Environment, PrivateEnvironment};
use crate::layout::function_object::{CallEnvironmentTemplate, EcmascriptFunctionObject, FUNCTION_ENVIRONMENT_WORDS};
use crate::layout::object::{INLINE_NAMED_STORAGE_CAPACITY, Object};
use crate::layout::primitive_string::PrimitiveString;
use crate::layout::realm::Realm;
use crate::layout::shape::Shape;
use crate::layout::value::Value;
use crate::runtime::array::Array;
use crate::runtime::completion::Must;
use crate::runtime::declarative_environment::{DECLARATIVE_ENVIRONMENT_CELL_SIZES, inline_binding_size_class};
use crate::runtime::function_environment::{
    FUNCTION_ENVIRONMENT_CELL_SIZES, FUNCTION_ENVIRONMENT_FUNCTION_OBJECT_OFFSET,
    FUNCTION_ENVIRONMENT_THIS_VALUE_OFFSET, FunctionEnvironment,
};
use crate::runtime::object::{
    EMPTY_PLAIN_OBJECT_INLINE_CAPACITY, PLAIN_OBJECT_INLINE_CAPACITIES, plain_object_size_classes,
};
use crate::runtime::primitive_string::{
    ROPE_STRING_LHS_OFFSET, ROPE_STRING_MIN_LENGTH, ROPE_STRING_RHS_OFFSET, RopeString,
};
use crate::runtime::shared_function_instance_data::{FunctionKind, SharedFunctionInstanceData, ThisMode};
use crate::runtime::value_storage::{self, SIZE_CLASS_CAPACITIES, VALUES_OFFSET};

fn size(size: usize) -> u32 {
    u32::try_from(size).expect("cells are smaller than 4 GiB")
}

fn address(local_free_list: Option<NonNull<*mut core::ffi::c_void>>) -> u64 {
    local_free_list.map_or(0, |list| list.as_ptr() as u64)
}

/// The bytes of `cell` (of `size` bytes) as u64 words, with a clear mark: cells allocated while the incremental sweep
/// is active start out marked, but cells from local free lists never need marking.
fn cell_words<T>(cell: Gc<T>, size: usize) -> Vec<u64> {
    let mut words = vec![0u64; size.div_ceil(8)];
    // SAFETY: The cell is live and has `size` bytes, which the words have room for.
    unsafe { core::ptr::copy_nonoverlapping(cell.as_ptr().cast::<u8>(), words.as_mut_ptr().cast::<u8>(), size) };
    let mark = core::mem::offset_of!(CellHeader, mark);
    // SAFETY: The mark is a byte within the words.
    unsafe { words.as_mut_ptr().cast::<u8>().add(mark).write(0) };
    words
}

/// What JIT code needs to allocate cells itself, for code of `realm`. The templates come from cells allocated here,
/// so this may collect garbage.
pub struct AllocationInfos {
    pub object: ObjectAllocationInfo,
    pub array: ArrayAllocationInfo,
    pub rope: RopeAllocationInfo,
    pub function: FunctionAllocationInfo,
    /// The cells the templates refer to that code embeds (the shape of new arrays).
    pub cells: Vec<Gc<CellHeader>>,
}

pub fn allocation_infos(vm: &Vm, realm: Gc<Realm>) -> AllocationInfos {
    let heap = vm.heap();
    let inline = heap.inline_allocation_info();
    let heap_address = heap.raw() as u64;
    let heap_offset = |field: NonNull<usize>| {
        u32::try_from(field.as_ptr() as u64 - heap_address).expect("the counters are fields of the heap")
    };

    let object_template = Object::create(vm, realm, Some(realm.object_prototype()));
    let mut object = ObjectAllocationInfo {
        empty_object_inline_capacity: u32::from(EMPTY_PLAIN_OBJECT_INLINE_CAPACITY),
        heap: heap_address,
        heap_allocated_bytes_offset: heap_offset(inline.allocated_bytes_since_last_gc),
        heap_threshold_offset: heap_offset(inline.gc_bytes_threshold),
        heap_total_allocated_bytes_offset: heap_offset(inline.total_allocated_bytes),
        local_free_list_offset: 0,
        freelist_next_offset: offset(inline.free_cell_next_offset),
        freelist_link_mask: u32::try_from(inline.free_cell_link_mask).expect("links are offsets within a block"),
        template: cell_words(object_template, size_of::<Object>()),
        shape_offset: offset(core::mem::offset_of!(Object, shape)),
        named_properties_offset: offset(core::mem::offset_of!(Object, named_properties)),
        inline_storage_offset: offset(core::mem::offset_of!(Object, inline_named_storage)),
        inline_capacity_offset: offset(core::mem::offset_of!(Object, inline_named_capacity)),
        slow_path: libjs_jit_allocate_object as *const () as u64,
        ..ObjectAllocationInfo::default()
    };
    // NB: Free lists are only popped inline where the heap gives their addresses (not in AddressSanitizer builds).
    if let Some(local_free_list) = heap.local_free_list_of(Object::CLASS) {
        object.size_classes.push(ObjectSizeClass {
            allocator: local_free_list.as_ptr() as u64,
            cell_size: size(size_of::<Object>()),
            inline_capacity: INLINE_NAMED_STORAGE_CAPACITY as u32,
        });
        for (size_class, inline_capacity) in plain_object_size_classes(heap)
            .iter()
            .zip(&PLAIN_OBJECT_INLINE_CAPACITIES[1..])
        {
            object.size_classes.push(ObjectSizeClass {
                allocator: address(size_class.local_free_list()),
                cell_size: size_class.cell_size(),
                inline_capacity: u32::from(*inline_capacity),
            });
        }
    }

    let array_template = Array::create(vm, realm, 0, None).must();
    let array_shape = array_template.shape();
    let storage_size_classes = value_storage::size_classes(heap)
        .iter()
        .zip(SIZE_CLASS_CAPACITIES)
        .map(|(size_class, capacity)| StorageSizeClass {
            allocator: address(size_class.local_free_list()),
            cell_size: size_class.cell_size(),
            capacity,
        })
        .collect::<Vec<_>>();
    let array = ArrayAllocationInfo {
        allocator: address(heap.local_free_list_of(Array::CLASS)),
        cell_size: size(size_of::<Array>()),
        template: cell_words(array_template, size_of::<Array>()),
        shape: CellId(array_shape.as_ptr() as u64),
        storage_size_classes: if storage_size_classes.iter().all(|size_class| size_class.allocator != 0) {
            storage_size_classes
        } else {
            Vec::new()
        },
        storage_template: value_storage::cell_header_template().to_vec(),
        storage_values_offset: offset(VALUES_OFFSET),
        storage_capacity_offset: offset(value_storage::CAPACITY_OFFSET),
        slow_path: libjs_jit_allocate_array as *const () as u64,
    };

    let rope_template = rope_template(vm);
    let rope = RopeAllocationInfo {
        allocator: address(heap.local_free_list_of(RopeString::CLASS)),
        cell_size: size(size_of::<RopeString>()),
        template: rope_template,
        lhs_offset: offset(ROPE_STRING_LHS_OFFSET),
        rhs_offset: offset(ROPE_STRING_RHS_OFFSET),
        length_offset: offset(core::mem::offset_of!(PrimitiveString, length_in_utf16_code_units)),
        min_length: ROPE_STRING_MIN_LENGTH,
    };

    let function = FunctionAllocationInfo {
        allocator: address(heap.local_free_list_of(EcmascriptFunctionObject::CLASS)),
        cell_size: size(size_of::<EcmascriptFunctionObject>()),
        slow_path: libjs_jit_clone_function as *const () as u64,
    };

    AllocationInfos {
        object,
        array,
        rope,
        function,
        cells: vec![header_of(array_shape)],
    }
}

fn header_of<T>(cell: Gc<T>) -> Gc<CellHeader> {
    // SAFETY: Every cell starts with a cell header.
    unsafe { Gc::from_non_null(cell.as_non_null().cast()) }
}

/// The bytes of a new rope string, with null halves.
fn rope_template(vm: &Vm) -> Vec<u64> {
    let half = PrimitiveString::create_from_utf8(vm, "ropes are made of two halves");
    let rope = PrimitiveString::create_from_concatenation(vm, half, half).must();
    assert!(class_of(rope).is_subclass_of(RopeString::CLASS));
    let mut words = cell_words(rope, size_of::<RopeString>());
    words[ROPE_STRING_LHS_OFFSET / 8] = 0;
    words[ROPE_STRING_RHS_OFFSET / 8] = 0;
    words
}

/// The size class of function environments with room for `binding_count` binding values in their cells, as
/// `FunctionEnvironmentFreeLists` numbers them: 0 for none, and the inline binding size class plus one otherwise.
fn function_environment_size_class(binding_count: usize) -> Option<usize> {
    match inline_binding_size_class(binding_count) {
        Some(size_class) => Some(size_class + 1),
        None if binding_count == 0 => Some(0),
        None => None,
    }
}

/// The local free list function environments of a size class (see `function_environment_size_class()`) come from,
/// and the size of their cells, if the heap gives the address of the list.
fn function_environment_size_class_allocator(
    vm: &Vm,
    size_class: usize,
) -> Option<(NonNull<*mut core::ffi::c_void>, u32)> {
    let heap = vm.heap();
    if size_class == 0 {
        return Some((
            heap.local_free_list_of(FunctionEnvironment::CLASS)?,
            size(size_of::<FunctionEnvironment>()),
        ));
    }
    let size_class = heap.size_classes(FunctionEnvironment::CLASS, &FUNCTION_ENVIRONMENT_CELL_SIZES)[size_class - 1];
    Some((size_class.local_free_list()?, size_class.cell_size()))
}

/// The local free list function environments with room for `binding_count` binding values in their cells come from,
/// and the size of their cells, if they have room for that many and the heap gives the address of the list.
fn function_environment_allocator(vm: &Vm, binding_count: usize) -> Option<(NonNull<*mut core::ffi::c_void>, u32)> {
    function_environment_size_class_allocator(vm, function_environment_size_class(binding_count)?)
}

/// The number of entries of `FunctionEnvironmentFreeLists`, a power of two above the number of size classes.
pub const FUNCTION_ENVIRONMENT_FREE_LIST_COUNT: usize = 16;
const _: () = assert!(FUNCTION_ENVIRONMENT_CELL_SIZES.len() < FUNCTION_ENVIRONMENT_FREE_LIST_COUNT);

/// A list that is always empty, for the size classes whose free lists JIT code does not pop. It is never written: JIT
/// code only pops lists that are not empty.
static EMPTY_FREE_LIST: u64 = 0;

/// The addresses of the local free lists of the function environment size classes (see
/// `function_environment_size_class()`), which call stubs pop the environments of calls from, by the size class of
/// the callee's `CallEnvironmentTemplate` masked into the table. Size classes without an address and the entries past
/// the size classes have a list that is always empty, which sends the stub to the runtime.
pub struct FunctionEnvironmentFreeLists(Box<[core::cell::Cell<u64>; FUNCTION_ENVIRONMENT_FREE_LIST_COUNT]>);

impl Default for FunctionEnvironmentFreeLists {
    fn default() -> Self {
        let empty = core::ptr::from_ref(&EMPTY_FREE_LIST) as u64;
        Self(Box::new(core::array::from_fn(|_| core::cell::Cell::new(empty))))
    }
}

impl FunctionEnvironmentFreeLists {
    /// The address of the table, after filling it in from the VM's heap.
    pub fn address(&self, vm: &Vm) -> u64 {
        for size_class in 0..=FUNCTION_ENVIRONMENT_CELL_SIZES.len() {
            if let Some((allocator, _)) = function_environment_size_class_allocator(vm, size_class) {
                self.0[size_class].set(allocator.as_ptr() as u64);
            }
        }
        self.0.as_ptr() as u64
    }
}

/// Gives the calls of `function` the template of their function environments (see `CallEnvironmentTemplate`) that
/// call stubs allocate them with, made from `environment`, which the runtime just made and bound `this` in for a call
/// of it, if they have none yet and JIT code can allocate them itself.
pub fn make_call_environment_template(vm: &Vm, function: Gc<EcmascriptFunctionObject>, environment: Gc<Environment>) {
    let shared_data = function.shared_data();
    // SAFETY: Nothing else refers to the template while this looks at whether there is one.
    if unsafe { &*shared_data.call_environment_template.as_ptr() }.is_some() {
        return;
    }
    let Some(environment) = environment.downcast::<FunctionEnvironment>() else {
        return;
    };
    let binding_count = shared_data.function_environment_bindings_count();
    let Some(size_class) = function_environment_size_class(binding_count) else {
        return;
    };
    let Some((_, cell_size)) = function_environment_size_class_allocator(vm, size_class) else {
        return;
    };
    let has_inline_binding_values = binding_count != 0;
    if !environment.rare_data.get().is_null()
        || environment.binding_values_are_inline.get() != has_inline_binding_values
        || environment.binding_values.size() != 0
        || (has_inline_binding_values && environment.shape.get().is_none())
    {
        return;
    }
    let words = cell_words(environment, size_of::<FunctionEnvironment>());
    let mut template = CallEnvironmentTemplate {
        header: CellHeader::for_class(CallEnvironmentTemplate::CLASS),
        size_class: size_class as u64,
        cell_size: u64::from(cell_size),
        binding_values_offset: if has_inline_binding_values {
            size_of::<FunctionEnvironment>() as u64
        } else {
            0
        },
        binds_this: u64::from(function.uses_this() && function.this_mode() != ThisMode::Lexical),
        words: [0; FUNCTION_ENVIRONMENT_WORDS],
    };
    template.words.copy_from_slice(&words);
    for offset in [
        core::mem::offset_of!(DeclarativeEnvironment, binding_values.data),
        core::mem::offset_of!(Environment, outer),
        FUNCTION_ENVIRONMENT_FUNCTION_OBJECT_OFFSET,
        FUNCTION_ENVIRONMENT_THIS_VALUE_OFFSET,
    ] {
        template.words[offset / 8] = 0;
    }
    shared_data
        .call_environment_template
        .set(Some(vm.heap().allocate(template)));
}

/// The template of the function environments that calls of `function` get (see `EnvironmentTemplateSnapshot`), if the
/// function needs one and JIT code can allocate it itself: when the environment has its final shape from the start and
/// room for its binding values in its cell, and the heap gives the address of the local free list it comes from. This
/// allocates a sample, so it may collect garbage. The template refers to the function, its shape and its outer
/// environment, which `cell` gets.
pub fn function_environment_template(
    vm: &Vm,
    function: Gc<EcmascriptFunctionObject>,
    mut cell: impl FnMut(Gc<CellHeader>) -> CellId,
) -> Option<EnvironmentTemplateSnapshot> {
    if !function.function_environment_needed() {
        return None;
    }
    let shared_data = function.shared_data();
    let binding_count = shared_data.function_environment_bindings_count();
    let (allocator, cell_size) = function_environment_allocator(vm, binding_count)?;
    let sample = function
        .inline_call_environment(vm, None)?
        .downcast::<FunctionEnvironment>()?;
    // NB: Calls that bind `this` store it in the environment, as resolve_and_bind_this() does.
    let binds_this = function.uses_this() && function.this_mode() != ThisMode::Lexical;
    if binds_this {
        sample.bind_this_value(vm, Value::UNDEFINED).must();
    }
    if !sample.rare_data.get().is_null() || sample.binding_values_are_inline.get() != (binding_count != 0) {
        return None;
    }
    let mut cells = Vec::new();
    if let Some(shape) = sample.shape.get() {
        cells.push(cell(header_of(shape)));
    } else if binding_count != 0 {
        return None;
    }
    if let Some(outer) = sample.outer.get() {
        cells.push(cell(header_of(outer)));
    }
    cells.push(cell(header_of(function)));
    Some(EnvironmentTemplateSnapshot {
        allocator: allocator.as_ptr() as u64,
        cell_size,
        words: cell_words(sample, size_of::<FunctionEnvironment>()),
        inline_binding_values: binding_count != 0,
        binding_values_offset: offset(core::mem::offset_of!(DeclarativeEnvironment, binding_values.data)),
        binds_this,
        this_value_offset: offset(FUNCTION_ENVIRONMENT_THIS_VALUE_OFFSET),
        cells,
    })
}

/// The templates of the lexical environments the `CreateLexicalEnvironment` instructions of `executable` create (see
/// `LexicalEnvironmentTemplateSnapshot`), indexed by environment shape cache, for the caches that have their final
/// shape. This allocates samples, so it may collect garbage. The templates refer to their shapes, which `cell` gets.
pub fn lexical_environment_templates(
    vm: &Vm,
    executable: Gc<Executable>,
    realm: Gc<Realm>,
    mut cell: impl FnMut(Gc<CellHeader>) -> CellId,
) -> Vec<Option<LexicalEnvironmentTemplateSnapshot>> {
    let heap = vm.heap();
    (0..executable.environment_shape_cache_count())
        .map(|index| {
            let cache = executable.environment_shape_cache(index);
            let shape = cache.shape();
            let binding_count = shape.map_or(0, |shape| shape.size());
            let (allocator, cell_size) = match inline_binding_size_class(binding_count) {
                Some(size_class) => {
                    let size_class = heap
                        .size_classes(DeclarativeEnvironment::CLASS, &DECLARATIVE_ENVIRONMENT_CELL_SIZES)[size_class];
                    (size_class.local_free_list()?, size_class.cell_size())
                }
                None if binding_count == 0 => (
                    heap.local_free_list_of(DeclarativeEnvironment::CLASS)?,
                    size(size_of::<DeclarativeEnvironment>()),
                ),
                None => return None,
            };
            let sample = create_lexical_environment(
                vm,
                realm.global_environment().upcast(),
                cache,
                u32::try_from(binding_count).ok()?,
                false,
            );
            if !sample.rare_data.get().is_null() || sample.binding_values_are_inline.get() != (binding_count != 0) {
                return None;
            }
            Some(LexicalEnvironmentTemplateSnapshot {
                allocator: allocator.as_ptr() as u64,
                cell_size,
                words: cell_words(sample, size_of::<DeclarativeEnvironment>()),
                binding_count: u32::try_from(binding_count).ok()?,
                inline_binding_values: binding_count != 0,
                binding_values_offset: offset(core::mem::offset_of!(DeclarativeEnvironment, binding_values.data)),
                outer_offset: offset(core::mem::offset_of!(Environment, outer)),
                shape: shape.map(|shape| cell(header_of(shape))),
            })
        })
        .collect()
}

/// `Environment* libjs_jit_create_lexical_environment(VM*, Environment* parent, Executable*, u32 shape_cache, u32
/// capacity)`: what CreateLexicalEnvironment without a catch environment creates, for JIT code.
///
/// # Safety
///
/// JIT code calls this with its VM, a live parent environment and the executable of its frame.
pub unsafe extern "C" fn libjs_jit_create_lexical_environment(
    vm: *const Vm,
    parent: *mut Environment,
    executable: *mut Executable,
    shape_cache: u32,
    capacity: u32,
) -> *mut DeclarativeEnvironment {
    // SAFETY: JIT code passes its VM and live cells.
    let (vm, parent, executable) = unsafe {
        (
            &*vm,
            Gc::from_non_null(NonNull::new_unchecked(parent)),
            Gc::from_non_null(NonNull::new_unchecked(executable)),
        )
    };
    create_lexical_environment(
        vm,
        parent,
        executable.environment_shape_cache(shape_cache),
        capacity,
        false,
    )
    .as_ptr()
}

/// The closures JIT code of `executable`, whose frames run `function`, creates with each `NewFunction` (by shared
/// function data index) by copying a sample (see `FunctionAllocationInfo`), or None for those it cannot. This
/// allocates the samples, so it may collect garbage.
pub fn closure_templates(
    vm: &Vm,
    executable: Gc<Executable>,
    function: Gc<EcmascriptFunctionObject>,
) -> Vec<Option<(Gc<EcmascriptFunctionObject>, Vec<u64>)>> {
    (0..executable.shared_function_data_count())
        .map(|index| {
            let shared_data = executable.shared_function_data(u32::try_from(index).ok()?);
            closure_template(vm, function, shared_data)
        })
        .collect()
}

fn closure_template(
    vm: &Vm,
    function: Gc<EcmascriptFunctionObject>,
    shared_data: Gc<SharedFunctionInstanceData>,
) -> Option<(Gc<EcmascriptFunctionObject>, Vec<u64>)> {
    // NB: Only the closures of plain functions are copies of each other apart from their environments.
    if shared_data.kind() != FunctionKind::Normal || shared_data.is_class_constructor() {
        return None;
    }
    let realm = function.realm()?;
    let sample = EcmascriptFunctionObject::create_from_function_data_with_prototype(
        vm,
        realm,
        shared_data,
        None,
        None,
        realm.function_prototype(),
    );
    // NB: The closures of the function's frames belong to its script or module, the active one in them.
    sample.script_or_module.set(function.script_or_module.get());
    if !sample.can_be_copied() {
        return None;
    }
    let words = cell_words(sample, size_of::<EcmascriptFunctionObject>());
    Some((sample, words))
}

/// The closure templates as the snapshot carries them, with their samples as cells the code embeds.
pub fn closure_template_snapshots(
    templates: Vec<Option<(Gc<EcmascriptFunctionObject>, Vec<u64>)>>,
    mut cell: impl FnMut(Gc<EcmascriptFunctionObject>) -> CellId,
) -> Vec<Option<ClosureTemplateSnapshot>> {
    templates
        .into_iter()
        .map(|template| {
            template.map(|(sample, words)| ClosureTemplateSnapshot {
                sample: cell(sample),
                words,
                inline_executable: None,
            })
        })
        .collect()
}

/// `Object* libjs_jit_allocate_object(VM*, Shape*, u32 reserve)`: a plain object of the shape, every named property
/// undefined, with room for `reserve` properties inline if a size class has room for them.
///
/// # Safety
///
/// JIT code calls this with its VM and a live shape.
pub unsafe extern "C" fn libjs_jit_allocate_object(vm: *const Vm, shape: *mut Shape, reserve: u32) -> *mut Object {
    // SAFETY: JIT code passes its VM and a shape it embeds.
    let (vm, shape) = unsafe { (&*vm, Gc::from_non_null(NonNull::new_unchecked(shape))) };
    Object::create_with_premade_shape_and_reserve(vm, shape, reserve).as_ptr()
}

/// `Array* libjs_jit_allocate_array(VM*, u32 count)`: an array of the running realm with `count` packed undefined
/// elements.
///
/// # Safety
///
/// JIT code calls this with its VM.
pub unsafe extern "C" fn libjs_jit_allocate_array(vm: *const Vm, count: u32) -> *mut Array {
    // SAFETY: JIT code passes its VM.
    let vm = unsafe { &*vm };
    let realm = vm.current_realm().expect("JIT code runs in a realm");
    let array = Array::create(vm, realm, 0, None).must();
    array.set_indexed_property_elements_to_undefined(count);
    array.as_ptr()
}

/// `ECMAScriptFunctionObject* libjs_jit_clone_function(VM*, ECMAScriptFunctionObject* sample, Environment*,
/// PrivateEnvironment*)`: a closure like the sample of a closure template, with the given environments.
///
/// # Safety
///
/// JIT code calls this with its VM, a sample it embeds and live environments (or null private environment).
pub unsafe extern "C" fn libjs_jit_clone_function(
    vm: *const Vm,
    sample: *mut EcmascriptFunctionObject,
    environment: *mut Environment,
    private_environment: *mut PrivateEnvironment,
) -> *mut EcmascriptFunctionObject {
    // SAFETY: JIT code passes its VM and live cells.
    let (vm, sample) = unsafe { (&*vm, Gc::from_non_null(NonNull::new_unchecked(sample))) };
    let realm = sample.realm().expect("an ECMAScript function has a realm");
    let closure = EcmascriptFunctionObject::create_from_function_data_with_prototype(
        vm,
        realm,
        sample.shared_data.get(),
        // SAFETY: JIT code passes live environments.
        NonNull::new(environment).map(|environment| unsafe { Gc::from_non_null(environment) }),
        // SAFETY: As above.
        NonNull::new(private_environment).map(|environment| unsafe { Gc::from_non_null(environment) }),
        realm.function_prototype(),
    );
    closure.script_or_module.set(sample.script_or_module.get());
    closure.as_ptr()
}
