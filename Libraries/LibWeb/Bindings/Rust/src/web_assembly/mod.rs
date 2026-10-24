/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The WebAssembly JavaScript Interface, https://webassembly.github.io/spec/js-api/, its Web API,
//! https://webassembly.github.io/spec/web-api/, and its ESM integration,
//! https://webassembly.github.io/esm-integration/js-api/, on the engine of Libraries/LibWasm/Engine.
//!
//! The engine's entities (functions, tables, memories, globals, tags and instances) and its exceptions and GC objects
//! are cells of the agent's heap. The store keeps none of them alive: each lives while an object that stands for it,
//! or another entity, refers to it. The engine has each cell keep the object that stands for it alive in turn
//! (`set_host_object()`), which makes that object the cell's entry in the agent's object caches: the object stays the
//! same one for as long as both live, and both go away together.

pub(crate) mod exception;
pub(crate) mod global;
pub(crate) mod instance;
pub(crate) mod memory;
pub(crate) mod module;
pub(crate) mod module_record;
pub(crate) mod table;
pub(crate) mod tag;
#[allow(
    clippy::module_inception,
    reason = "the module of an IDL file is named after it, WebAssembly/WebAssembly.idl"
)]
pub(crate) mod web_assembly;

mod builtins;
mod errors;
mod exported_function;
mod gc_object;
mod values;

use core::cell::{Cell, OnceCell, RefCell, UnsafeCell};
use core::ffi::{c_char, c_void};
use core::ptr::NonNull;
use std::collections::HashMap;
use std::rc::Rc;
use std::sync::Arc;

use libjs_rust::api::{
    EmbedderCell, EmbedderClass, EmbedderValue, ErrorKind, ErrorType, Gc, GcWeak, Realm, Throw, ThrowCompletionOr,
    Trace, Value, Visitor, Vm, is_host_instance_of,
};
use libwasm_engine::Module as CoreModule;
use libwasm_engine::runtime::entity::{Entity, Handle};
use libwasm_engine::runtime::heap::{CellTypeInfo, GcInterface, GcLayout, PrimitiveStorageLayout};
use libwasm_engine::runtime::{InstantiationError, Store, TagAddress, Trap, Value as WasmValue};
use libwasm_engine::types::FuncType;

use crate::bindings::conversions::BufferSource;
use crate::bindings::interface::{Interface, Ref, Wrapped, implementation_of};
use crate::web_assembly::errors::{WebAssemblyError, throw_webassembly_error};
use crate::web_assembly::exception::Exception;
use crate::web_assembly::memory::{Memory, refresh_the_memory_buffer};
use crate::web_assembly::values::{EXTERNREF, ExternValueBox, to_js_value};

// https://webassembly.github.io/spec/js-api/#page-size
pub(crate) const PAGE_SIZE: u64 = 65536;

unsafe extern "C" {
    // The functions of Libraries/LibGC/CAPI.h that the engine uses, with the types of its GcInterface.
    fn gc_get_layout(layout: *mut GcLayout);
    fn gc_allocator_create(type_info: *const CellTypeInfo, name: *const c_char, name_length: usize) -> *mut c_void;
    fn gc_heap_allocate_cell(heap: *mut c_void, allocator: *mut c_void, must_mark: *mut bool) -> *mut c_void;
    fn gc_heap_defer_gc(heap: *mut c_void);
    fn gc_heap_undefer_gc(heap: *mut c_void);
    fn gc_heap_collect_garbage(heap: *mut c_void, collection_type: i32, print_report: bool);
    fn gc_heap_did_allocate_external_memory(heap: *mut c_void, size: usize);
    fn gc_root_create(cell: *mut c_void) -> *mut c_void;
    fn gc_root_destroy(root: *mut c_void);
    pub(crate) fn gc_visitor_visit_cell(visitor: *mut c_void, cell: *mut c_void);
    fn gc_visitor_visit_possible_values(visitor: *mut c_void, data: *const u8, size: usize);
    fn gc_primitive_storage_reserve(
        size: usize,
        capacity: usize,
        zero_fill: bool,
        guard_size: usize,
        out_handle: *mut u64,
        out_layout: *mut PrimitiveStorageLayout,
    ) -> bool;
    fn gc_primitive_storage_resize(
        handle: u64,
        new_size: usize,
        zero_fill: bool,
        out_layout: *mut PrimitiveStorageLayout,
    ) -> bool;
    fn gc_primitive_storage_free(handle: u64);
    fn gc_primitive_storage_data(handle: u64) -> *mut u8;
}

fn gc_interface(heap: *mut c_void) -> GcInterface {
    GcInterface {
        heap,
        gc_get_layout,
        gc_allocator_create,
        gc_heap_allocate_cell,
        gc_heap_defer_gc,
        gc_heap_undefer_gc,
        gc_heap_collect_garbage,
        gc_heap_did_allocate_external_memory,
        gc_root_create,
        gc_root_destroy,
        gc_visitor_visit_cell,
        gc_visitor_visit_possible_values,
        gc_primitive_storage_reserve,
        gc_primitive_storage_resize,
        gc_primitive_storage_free,
        gc_primitive_storage_data,
    }
}

/// An entity of the store, which keeps its cell alive where it is traced.
pub(crate) struct EntityRef<T>(pub(crate) Handle<T>);

impl<T> Clone for EntityRef<T> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<T> Copy for EntityRef<T> {}

// SAFETY: The handle is the address of the entity's cell, which this visits.
unsafe impl<T: Entity + 'static> Trace for EntityRef<T> {
    fn trace(&self, visitor: &mut Visitor) {
        // SAFETY: The visitor is live while it traces, and the cell is a cell of its heap.
        unsafe { gc_visitor_visit_cell(visitor.as_raw().cast(), self.0.cell().cast()) };
    }
}

/// An exception, a GC object, or another reference that is the address of a cell, which keeps the cell alive where it
/// is traced.
#[derive(Clone, Copy)]
pub(crate) struct CellReference(pub(crate) u64);

// SAFETY: The reference is the address of its cell, which this visits.
unsafe impl Trace for CellReference {
    fn trace(&self, visitor: &mut Visitor) {
        // SAFETY: The visitor is live while it traces, and the reference is the address of a cell of its heap.
        unsafe { gc_visitor_visit_cell(visitor.as_raw().cast(), self.0 as *mut c_void) };
    }
}

/// Values of the store, which may be references that keep cells alive where they are traced, as a conservative scan
/// of the values would. They live in a cell of their own, which a Gc in a local keeps alive. As allocating the cell can
/// collect garbage, it is allocated empty, before there are values for it to keep alive.
pub(crate) struct WasmValues(pub(crate) RefCell<Vec<WasmValue>>);

static WASM_VALUES_CLASS: EmbedderClass<WasmValues> = EmbedderClass::new("WasmValues");

impl EmbedderValue for WasmValues {
    const CLASS: &'static EmbedderClass<Self> = &WASM_VALUES_CLASS;
}

impl WasmValues {
    pub(crate) fn allocate(vm: &Vm) -> Gc<EmbedderCell<WasmValues>> {
        WASM_VALUES_CLASS.allocate(vm, |_| WasmValues(RefCell::default()))
    }
}

// SAFETY: Every cell the values refer to is at an address in their bits, which this visits as a possible value. Only
//         the edges of a cell may be visited so, which is why the values live in one.
unsafe impl Trace for WasmValues {
    fn trace(&self, visitor: &mut Visitor) {
        for value in self.0.borrow().iter() {
            let (slots, count) = value.to_slots();
            // SAFETY: The visitor is live while it traces, and the slots are plain data.
            unsafe {
                gc_visitor_visit_possible_values(
                    visitor.as_raw().cast(),
                    slots.as_ptr().cast(),
                    count * size_of::<u64>(),
                );
            };
        }
    }
}

/// The surrounding agent's associated store, and the agent's caches of the objects that are not the host objects of
/// cells: the boxes of the host value cache, https://webassembly.github.io/spec/js-api/#host-value-cache
pub(crate) struct Agent {
    vm: NonNull<Vm>,
    // NB: The store must not move once it is attached to the heap.
    store: Box<UnsafeCell<Store>>,
    // https://webassembly.github.io/spec/js-api/#javascript-exception-tag
    js_tag: TagAddress,
    // NB: The root of the tag's cell, which lives as long as the agent.
    _js_tag_root: NonNull<c_void>,
    // NB: The module whose types are the funcTypes of the builtins, https://webassembly.github.io/spec/js-api/#builtins
    builtin_types_module: OnceCell<CompiledModule>,
    // NB: An entry whose box is gone is stale. Stale entries go once there are as many of them as live ones.
    extern_value_boxes: RefCell<HashMap<u64, GcWeak<EmbedderCell<ExternValueBox>>>>,
    live_extern_value_boxes_after_pruning: Cell<usize>,
}

thread_local! {
    static AGENT: Cell<Option<&'static Agent>> = const { Cell::new(None) };
}

impl Agent {
    /// The agent of the current thread, which the first use on it creates. Each thread is one agent with one VM and one
    /// heap, which the agent outlives.
    pub(crate) fn the(vm: &Vm) -> &'static Agent {
        if let Some(agent) = AGENT.get() {
            return agent;
        }
        let store_cell = Box::new(UnsafeCell::new(Store::with_heap(Some(gc_interface(
            vm.heap().raw().cast(),
        )))));
        // SAFETY: Nothing else has the store yet.
        let store = unsafe { &mut *store_cell.get() };
        store.attach_to_heap();

        // NB: The store is the agent's, whose [[CanBlock]] decides whether memory.atomic.wait may block.
        store.can_block = libjs_rust::api::agent_can_suspend(vm);

        let js_tag = store.allocate_tag(FuncType {
            params: vec![EXTERNREF],
            results: Vec::new(),
        });
        // NB: The tag lives as long as the agent, which roots its cell.
        // SAFETY: The cell is a live cell of the heap.
        let js_tag_root = NonNull::new(unsafe { gc_root_create(js_tag.cell().cast()) }).expect("a root is allocated");

        // https://webassembly.github.io/spec/js-api/#memory-objects
        // Whenever memory.grow is executed, refresh the memory buffer of the memory's Memory object, if it has one.
        store.memory_grow_callback = Some(Rc::new(|memory| {
            let agent = AGENT.get().expect("a store's callbacks run on the thread of its agent");
            if let Some(object) = host_object::<EmbedderCell<Wrapped<Memory>>>(memory.cell()) {
                refresh_the_memory_buffer(agent.vm(), object);
            }
        }));

        let agent: &'static Agent = Box::leak(Box::new(Agent {
            vm: NonNull::from(vm),
            store: store_cell,
            js_tag,
            _js_tag_root: js_tag_root,
            builtin_types_module: OnceCell::new(),
            extern_value_boxes: RefCell::default(),
            live_extern_value_boxes_after_pruning: Cell::new(0),
        }));
        AGENT.set(Some(agent));
        agent
    }

    /// The agent of the current thread, which must have one.
    pub(crate) fn current() -> &'static Agent {
        AGENT.get().expect("the thread has an agent")
    }

    pub(crate) fn vm(&self) -> &'static Vm {
        // SAFETY: The VM of the thread outlives its agent.
        unsafe { self.vm.as_ref() }
    }

    /// The store, which a call into it can be on the stack below. It belongs to the thread of the agent, and is reached
    /// the way the engine's C interface reaches it, through a pointer, for as long as any one use lasts: a caller keeps
    /// the reference no longer than that, as the JavaScript and WebAssembly that run in between reach the store through
    /// the agent as well.
    #[allow(
        clippy::mut_from_ref,
        reason = "the store is the agent's, as an embedder of the engine holds it"
    )]
    pub(crate) fn store(&self) -> &mut Store {
        // SAFETY: The agent lives as long as the thread, which is the only one that uses its store.
        unsafe { &mut *self.store.get() }
    }

    pub(crate) fn js_tag(&self) -> TagAddress {
        self.js_tag
    }

    pub(crate) fn builtin_types_module(&self) -> &CompiledModule {
        self.builtin_types_module.get_or_init(|| {
            Arc::new(
                libwasm_engine::compile(&builtins::BUILTIN_TYPES_MODULE_BYTES)
                    .expect("the funcTypes of the builtins are a valid module"),
            )
        })
    }

    pub(crate) fn extern_value_box(&self, value: Value) -> Option<Gc<EmbedderCell<ExternValueBox>>> {
        self.extern_value_boxes
            .borrow()
            .get(&value.to_raw())
            .and_then(GcWeak::get)
    }

    pub(crate) fn set_extern_value_box(&self, value: Value, value_box: Gc<EmbedderCell<ExternValueBox>>) {
        let mut boxes = self.extern_value_boxes.borrow_mut();
        if boxes.len() >= 2 * self.live_extern_value_boxes_after_pruning.get() + 16 {
            boxes.retain(|_, weak| weak.get().is_some());
            self.live_extern_value_boxes_after_pruning.set(boxes.len());
        }
        boxes.insert(value.to_raw(), GcWeak::new(self.vm().heap(), value_box));
    }
}

/// The object that the cell keeps alive as the one that stands for it, if it has one (`set_host_object()`).
pub(crate) fn host_object<T>(cell: *mut u8) -> Option<Gc<T>> {
    // SAFETY: The cell is a live cell of the runtime.
    let object = unsafe { libwasm_engine::runtime::heap::host_object(cell.cast()) };
    // SAFETY: Each kind of cell keeps one kind of object, which its cache stores as a cell of that type.
    NonNull::new(object.cast::<T>()).map(|object| unsafe { Gc::from_non_null(object) })
}

/// Has the cell keep the object alive as the one that stands for it, for as long as the cell lives.
pub(crate) fn set_host_object<T>(cell: *mut u8, object: Gc<T>) {
    // SAFETY: The cell is a live cell of the runtime, and the object a cell of the same heap.
    unsafe { libwasm_engine::runtime::heap::set_host_object(cell, object.as_ptr().cast()) };
}

/// The object of the interface that the value is the wrapper of, if it is one.
pub(crate) fn implementation_of_value<T: Interface>(value: Value) -> Option<Ref<T>> {
    if !value.is_object() || !is_host_instance_of(&value.as_object(), T::wrapper_class()) {
        return None;
    }
    Some(implementation_of::<T>(value.as_object()))
}

/// A copy of the bytes held by a buffer, which the algorithms compile: their stableBytes.
pub(crate) fn stable_bytes_of(bytes: BufferSource) -> Vec<u8> {
    // SAFETY: Copying the bytes neither runs script nor allocates a cell.
    unsafe { bytes.with_bytes(<[u8]>::to_vec) }
}

/// What a compiled WebAssembly module is to its objects: the module, which threads can share.
pub(crate) type CompiledModule = Arc<CoreModule>;

/// https://webassembly.github.io/spec/js-api/#compile-a-webassembly-module
/// https://webassembly.github.io/content-security-policy/js-api/#compile-a-webassembly-module
pub(crate) fn compile_a_webassembly_module(
    vm: &Vm,
    realm: Gc<Realm>,
    bytes: &[u8],
) -> ThrowCompletionOr<CompiledModule> {
    host_ensure_can_compile_wasm_bytes(realm)?;

    match libwasm_engine::compile(bytes) {
        Ok(module) => Ok(Arc::new(module)),
        Err(error) => Err(throw_webassembly_error(
            vm,
            realm,
            WebAssemblyError::Compile,
            &error.to_string(),
        )),
    }
}

unsafe extern "C" {
    /// EnsureCSPDoesNotBlockWasmByteCompilation(realm), which completes with nothing, or throws a CompileError.
    fn web_bindings_ensure_csp_does_not_block_wasm_byte_compilation(
        realm: *mut c_void,
    ) -> libjs_rust::api::RawNativeFunctionResult;
}

// https://webassembly.github.io/content-security-policy/js-api/#abstract-opdef-hostensurecancompilewasmbytes
pub(crate) fn host_ensure_can_compile_wasm_bytes(realm: Gc<Realm>) -> ThrowCompletionOr<()> {
    // 2. Perform EnsureCSPDoesNotBlockWasmByteCompilation(realm)
    // This algorithm does not return a value, but raises a CompileError exception if the operation cannot complete
    // successfully.
    // SAFETY: The realm is live.
    unsafe { web_bindings_ensure_csp_does_not_block_wasm_byte_compilation(realm.as_ptr().cast()) }
        .into_completion()
        .map(|_| ())
}

/// How instantiating a module or calling a function of the store failed, as the exception it throws,
/// https://webassembly.github.io/spec/js-api/#error-objects
pub(crate) fn throw_of_trap(vm: &Vm, trap: Trap) -> Throw {
    let realm = vm.current_realm().expect("WebAssembly code runs in a realm");
    let agent = Agent::the(vm);
    match trap {
        // https://webassembly.github.io/spec/js-api/#stack-overflow
        // 6.1. Whenever a stack overflow occurs in WebAssembly code, the same class of exception is thrown as for a stack overflow in JavaScript.
        Trap::StackExhausted => {
            thrown(vm.throw_completion(ErrorKind::InternalError, ErrorType::CallStackSizeExceeded, &[]))
        }
        Trap::UncaughtException(exception) => {
            let store = agent.store();
            // NB: An exception with the JavaScript tag carries a JavaScript value that was thrown into WebAssembly
            //     code, which continues as itself.
            if store.exception(exception).tag == agent.js_tag() {
                let payload = store.exception_values(exception)[0];
                return Throw::new(to_js_value(vm, payload, EXTERNREF).unwrap_or_else(|throw| throw.value()));
            }
            // Otherwise, throw the Exception object of the exception.
            Throw::new(Exception::wrapper_of_address(vm, realm, exception))
        }
        // FIXME: Use the convoluted mapping of errors defined in the spec.
        trap => throw_webassembly_error(
            vm,
            realm,
            WebAssemblyError::Runtime,
            &format!("Wasm execution trapped (WIP): {}", trap.message()),
        ),
    }
}

pub(crate) fn throw_of_instantiation_error(vm: &Vm, error: InstantiationError) -> Throw {
    match error {
        InstantiationError::Link(message) => throw_webassembly_error(
            vm,
            vm.current_realm().expect("instantiation runs in a realm"),
            WebAssemblyError::Link,
            &message,
        ),
        InstantiationError::Trap(trap) => throw_of_trap(vm, trap),
    }
}

fn thrown(completion: ThrowCompletionOr<()>) -> Throw {
    match completion {
        Err(throw) => throw,
        Ok(()) => unreachable!("throwing a completion throws"),
    }
}
