/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::{Cell, OnceCell, RefCell};
use core::ffi::c_void;
use core::ptr::NonNull;

use ak::{Utf16FlyString, Utf16String};
use libjs_runtime_macros::Trace;

use super::interpreter_stack::InterpreterStackMemory;
use crate::build_configuration::VM_STACK_SPACE_LIMIT;
use crate::gc::capi::{self, GCVisitor};
use crate::gc::heap::Heap;
use crate::gc::root::RootSet;
use crate::gc::visitor::{Trace, Visitor};
use crate::layout::cell::Gc;
use crate::layout::execution_context::ExecutionContext;
use crate::layout::function_object::NativeFunctionTableEntry;
use crate::layout::vm::{InterpreterStack, VmHead};
use crate::runtime::common_property_names::CommonPropertyNames;
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::symbol::{self, Symbol, enumerate_well_known_symbols};

pub const STRING_TO_ATOM_CACHE_SIZE: usize = 2;
pub const FLY_STRING_CACHE_SIZE: usize = 1024;
pub const NUMERIC_STRING_CACHE_SIZE: usize = 1000;
pub const LARGE_NUMERIC_STRING_CACHE_SIZE: usize = 1024;
const SINGLE_ASCII_CHARACTER_STRING_COUNT: usize = 128;

/// Remembers the property key of a string that is not stored as a fly string.
#[derive(Default)]
pub struct StringToAtomCacheEntry {
    pub string: Option<Gc<PrimitiveString>>,
    pub atom: Option<Utf16FlyString>,
}

#[derive(Trace)]
pub struct NumericStringCacheEntry {
    pub number: Cell<u64>,
    pub string: Cell<Option<Gc<PrimitiveString>>>,
}

#[derive(Trace)]
#[allow(non_snake_case)]
pub struct CachedStrings {
    pub number: Gc<PrimitiveString>,
    pub undefined: Gc<PrimitiveString>,
    pub object: Gc<PrimitiveString>,
    pub string: Gc<PrimitiveString>,
    pub symbol: Gc<PrimitiveString>,
    pub boolean: Gc<PrimitiveString>,
    pub bigint: Gc<PrimitiveString>,
    pub function: Gc<PrimitiveString>,
    pub object_Object: Gc<PrimitiveString>,
}

macro_rules! define_well_known_symbols {
    ($(($symbol_name:ident, $snake_name:ident),)*) => {
        #[derive(Trace)]
        pub struct WellKnownSymbols {
            $(pub $snake_name: Gc<Symbol>,)*
        }

        impl WellKnownSymbols {
            fn create(vm: &Vm) -> Self {
                Self {
                    $($snake_name: Symbol::create(
                        vm,
                        Some(Utf16String::from_utf8(concat!("Symbol.", stringify!($symbol_name)))),
                        symbol::Kind::Unique,
                    ),)*
                }
            }
        }
    };
}

enumerate_well_known_symbols!(define_well_known_symbols);

/// The virtual machine the interpreter runs on. The interpreter reads and writes the head directly.
#[repr(C)]
pub struct Vm {
    pub head: VmHead,
    heap: OnceCell<Heap>,
    _interpreter_stack_memory: InterpreterStackMemory,
    execution_context_stack: RefCell<Vec<NonNull<ExecutionContext>>>,
    /// The context that was running when each context on the stack was pushed.
    previous_running_execution_contexts: RefCell<Vec<*mut ExecutionContext>>,
    native_function_table: RefCell<Vec<NativeFunctionTableEntry>>,
    roots: RootSet,

    pub names: CommonPropertyNames,
    /// The strings in these two caches are weak: the sweep callback drops the ones that die.
    string_to_atom_cache: RefCell<[StringToAtomCacheEntry; STRING_TO_ATOM_CACHE_SIZE]>,
    fly_string_cache: Box<[Cell<Option<Gc<PrimitiveString>>>; FLY_STRING_CACHE_SIZE]>,
    numeric_string_cache: Box<[Cell<Option<Gc<PrimitiveString>>>; NUMERIC_STRING_CACHE_SIZE]>,
    large_numeric_string_cache: Box<[NumericStringCacheEntry; LARGE_NUMERIC_STRING_CACHE_SIZE]>,
    empty_string: OnceCell<Gc<PrimitiveString>>,
    cached_strings: OnceCell<CachedStrings>,
    single_ascii_character_strings: OnceCell<[Gc<PrimitiveString>; SINGLE_ASCII_CHARACTER_STRING_COUNT]>,
    well_known_symbols: OnceCell<WellKnownSymbols>,
}

const _: () = assert!(core::mem::offset_of!(Vm, head) == 0);

impl Vm {
    pub fn create() -> Box<Vm> {
        // SAFETY: Initializes the region cell pointers are relative to, and the cage typed array data lives in.
        let (heap_region_base, primitive_storage_cage_base) =
            unsafe { (capi::gc_heap_region_base(), capi::gc_primitive_storage_cage_base()) };
        let interpreter_stack_memory = InterpreterStackMemory::allocate();
        let native_function_table = Vec::new();
        let vm = Box::new(Vm {
            head: VmHead {
                running_execution_context: Cell::new(core::ptr::null_mut()),
                interpreter_stack: interpreter_stack_memory.initial_state(),
                stack_base: Cell::new(0),
                execution_generation: Cell::new(0),
                primitive_storage_cage_base: Cell::new(primitive_storage_cage_base),
                heap_region_base: Cell::new(heap_region_base),
                native_function_table_data: Cell::new(Vec::<NativeFunctionTableEntry>::as_ptr(&native_function_table)),
                debugger: Cell::new(core::ptr::null_mut()),
            },
            heap: OnceCell::new(),
            _interpreter_stack_memory: interpreter_stack_memory,
            execution_context_stack: RefCell::new(Vec::new()),
            previous_running_execution_contexts: RefCell::new(Vec::new()),
            native_function_table: RefCell::new(native_function_table),
            roots: RootSet::default(),
            names: CommonPropertyNames::new(),
            string_to_atom_cache: RefCell::default(),
            fly_string_cache: Box::new([const { Cell::new(None) }; FLY_STRING_CACHE_SIZE]),
            numeric_string_cache: Box::new([const { Cell::new(None) }; NUMERIC_STRING_CACHE_SIZE]),
            large_numeric_string_cache: Box::new(
                [const {
                    NumericStringCacheEntry {
                        number: Cell::new(0),
                        string: Cell::new(None),
                    }
                }; LARGE_NUMERIC_STRING_CACHE_SIZE],
            ),
            empty_string: OnceCell::new(),
            cached_strings: OnceCell::new(),
            single_ascii_character_strings: OnceCell::new(),
            well_known_symbols: OnceCell::new(),
        });
        let context = core::ptr::from_ref::<Vm>(&vm).cast_mut().cast();
        // SAFETY: The VM is boxed, so its address is stable, and it destroys the heap before anything else.
        let heap = unsafe { Heap::new(gather_roots, context) };
        // SAFETY: As above.
        unsafe { heap.register_sweep_callback(sweep, context) };
        vm.head.stack_base.set(heap.stack_bounds().0);
        if vm.heap.set(heap).is_err() {
            unreachable!("the heap is created once");
        }
        vm.allocate_preallocated_strings_and_symbols();
        vm
    }

    fn allocate_preallocated_strings_and_symbols(&self) {
        let allocate_string = |string: &str| {
            self.heap()
                .allocate(PrimitiveString::new(Utf16String::from_utf8(string)))
        };

        let _ = self.empty_string.set(allocate_string(""));

        let _ = self.cached_strings.set(CachedStrings {
            number: allocate_string("number"),
            undefined: allocate_string("undefined"),
            object: allocate_string("object"),
            string: allocate_string("string"),
            symbol: allocate_string("symbol"),
            boolean: allocate_string("boolean"),
            bigint: allocate_string("bigint"),
            function: allocate_string("function"),
            object_Object: allocate_string("[object Object]"),
        });

        let _ = self
            .single_ascii_character_strings
            .set(core::array::from_fn(|character| {
                let character = [character as u8];
                allocate_string(core::str::from_utf8(&character).expect("ASCII is UTF-8"))
            }));

        let _ = self.well_known_symbols.set(WellKnownSymbols::create(self));
    }

    pub fn heap(&self) -> &Heap {
        self.heap.get().expect("the VM has a heap")
    }

    pub fn interpreter_stack(&self) -> &InterpreterStack {
        &self.head.interpreter_stack
    }

    pub fn running_execution_context(&self) -> Option<NonNull<ExecutionContext>> {
        NonNull::new(self.head.running_execution_context.get())
    }

    /// Whether so little of the native stack is left that running more JavaScript could overflow it.
    pub fn did_reach_stack_space_limit(&self) -> bool {
        let marker = 0u8;
        let current = core::ptr::from_ref(&marker) as usize;
        current.saturating_sub(self.head.stack_base.get()) < VM_STACK_SPACE_LIMIT as usize
    }

    pub fn push_execution_context(&self, context: NonNull<ExecutionContext>) {
        // SAFETY: The caller passes a live context.
        let context_ref = unsafe { context.as_ref() };
        context_ref.caller_frame.set(core::ptr::null_mut());
        context_ref.caller_return_pc.set(0);
        context_ref.caller_dst_raw.set(0);
        context_ref.caller_is_construct.set(false);
        self.execution_context_stack.borrow_mut().push(context);
        self.previous_running_execution_contexts
            .borrow_mut()
            .push(self.head.running_execution_context.get());
        self.head.running_execution_context.set(context.as_ptr());
    }

    pub fn pop_execution_context(&self) -> NonNull<ExecutionContext> {
        let context = self
            .execution_context_stack
            .borrow_mut()
            .pop()
            .expect("a context to pop");
        let previous = self
            .previous_running_execution_contexts
            .borrow_mut()
            .pop()
            .expect("each pushed context records its predecessor");
        self.head.running_execution_context.set(previous);
        context
    }

    /// Calls `callback` on every live execution context, from the running one down, following both the frames the
    /// interpreter links through caller_frame and the contexts pushed onto the execution context stack.
    pub fn for_each_execution_context_top_to_bottom(&self, mut callback: impl FnMut(&ExecutionContext)) {
        let stack = self.execution_context_stack.borrow();
        let previous_running = self.previous_running_execution_contexts.borrow();
        let mut stack_index = stack.len();
        let mut context = self.head.running_execution_context.get();
        if context.is_null() {
            for context in stack.iter().rev() {
                // SAFETY: Contexts on the stack are live.
                callback(unsafe { context.as_ref() });
            }
            return;
        }
        while !context.is_null() {
            // SAFETY: Every context reachable from the running one is live.
            let context_ref = unsafe { &*context };
            callback(context_ref);
            if stack_index > 0 && core::ptr::eq(context, stack[stack_index - 1].as_ptr()) {
                context = previous_running[stack_index - 1];
                stack_index -= 1;
                continue;
            }
            context = context_ref.caller_frame.get();
        }
    }

    pub fn roots(&self) -> &RootSet {
        &self.roots
    }

    fn gather_roots(&self, visitor: &mut Visitor) {
        self.for_each_execution_context_top_to_bottom(|context| context.trace(visitor));
        self.roots.trace(visitor);
        self.empty_string.trace(visitor);
        self.single_ascii_character_strings.trace(visitor);
        self.numeric_string_cache.trace(visitor);
        self.large_numeric_string_cache.trace(visitor);
        self.cached_strings.trace(visitor);
        self.well_known_symbols.trace(visitor);
    }

    /// Drops the weak cache entries of strings that died in this collection, as JS::PrimitiveString::finalize does.
    fn remove_dead_strings_from_weak_caches(&self) {
        let is_dead = |string: Gc<PrimitiveString>| !string.header.mark.get();
        for cache_slot in self.fly_string_cache.iter() {
            if cache_slot.get().is_some_and(is_dead) {
                cache_slot.set(None);
            }
        }
        for entry in self.string_to_atom_cache.borrow_mut().iter_mut() {
            if entry.string.is_some_and(is_dead) {
                *entry = StringToAtomCacheEntry::default();
            }
        }
    }

    pub fn string_to_atom_cache(&self) -> &RefCell<[StringToAtomCacheEntry; STRING_TO_ATOM_CACHE_SIZE]> {
        &self.string_to_atom_cache
    }

    pub fn fly_string_cache(&self) -> &[Cell<Option<Gc<PrimitiveString>>>; FLY_STRING_CACHE_SIZE] {
        &self.fly_string_cache
    }

    pub fn numeric_string_cache(&self) -> &[Cell<Option<Gc<PrimitiveString>>>; NUMERIC_STRING_CACHE_SIZE] {
        &self.numeric_string_cache
    }

    pub fn large_numeric_string_cache(&self) -> &[NumericStringCacheEntry; LARGE_NUMERIC_STRING_CACHE_SIZE] {
        &self.large_numeric_string_cache
    }

    pub fn empty_string(&self) -> Gc<PrimitiveString> {
        *self.empty_string.get().expect("the VM allocates the empty string")
    }

    pub fn single_ascii_character_string(&self, character: u8) -> Gc<PrimitiveString> {
        assert!(character < 0x80);
        self.single_ascii_character_strings
            .get()
            .expect("the VM allocates the single ASCII character strings")[usize::from(character)]
    }

    pub fn cached_strings(&self) -> &CachedStrings {
        self.cached_strings.get().expect("the VM allocates the cached strings")
    }

    pub fn well_known_symbols(&self) -> &WellKnownSymbols {
        self.well_known_symbols
            .get()
            .expect("the VM creates the well-known symbols")
    }

    pub fn register_native_function(&self, entry: NativeFunctionTableEntry) -> u32 {
        let mut table = self.native_function_table.borrow_mut();
        table.push(entry);
        self.head.native_function_table_data.set(table.as_ptr());
        u32::try_from(table.len() - 1).expect("native function index fits in u32")
    }
}

impl Drop for Vm {
    fn drop(&mut self) {
        // The heap's final collection destroys every cell, so it has to happen while the rest of the VM exists.
        drop(self.heap.take());
    }
}

unsafe extern "C" fn gather_roots(context: *mut c_void, visitor: *mut GCVisitor) {
    // SAFETY: The heap was created with the VM as its context, and LibGC passes a live visitor.
    let (vm, mut visitor) = unsafe { (&*context.cast::<Vm>(), Visitor::from_raw(visitor)) };
    vm.gather_roots(&mut visitor);
}

unsafe extern "C" fn sweep(context: *mut c_void) {
    // SAFETY: The sweep callback was registered with the VM as its context.
    let vm = unsafe { &*context.cast::<Vm>() };
    vm.remove_dead_strings_from_weak_caches();
}
