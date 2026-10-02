/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::{Cell, OnceCell, RefCell};
use core::ffi::c_void;
use core::ptr::NonNull;
use std::collections::HashMap;

use ak::{Utf16FlyString, Utf16String};
use libjs_runtime_macros::Trace;

use super::interpreter_stack::InterpreterStackMemory;
use crate::build_configuration::VM_STACK_SPACE_LIMIT;
use crate::bytecode::executable::{
    Executable, KeyedPropertyLookupCache, PropertyLookupCache, StaticPropertyLookupCacheSite,
    StaticPropertyLookupCaches,
};
use crate::bytecode::property_access::Strict;
use crate::gc::capi::{self, GCVisitor};
use crate::gc::heap::{Heap, cell_is_dead};
use crate::gc::root::RootSet;
use crate::gc::visitor::{Trace, Visitor};
use crate::layout::cell::Gc;
use crate::layout::environment::Environment;
use crate::layout::execution_context::{ExecutionContext, ScriptOrModule};
use crate::layout::function_object::{FunctionObject, NativeFunctionTableEntry, NativeFunctionType};
use crate::layout::object::Object;
use crate::layout::realm::Realm;
use crate::layout::value::Value;
use crate::layout::vm::{InterpreterStack, VmHead};
use crate::layout_forward::RawNativeFunctionPointer;
use crate::runtime::abstract_operations::get_this_environment;
use crate::runtime::common_property_names::CommonPropertyNames;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::declarative_environment::DeclarativeEnvironment;
use crate::runtime::ecmascript_function_object::as_ecmascript_function_object;
use crate::runtime::environment_coordinate::EnvironmentCoordinate;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_environment::FunctionEnvironment;
use crate::runtime::object::IntrinsicAccessor;
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::reference::{BaseType, Reference};
use crate::runtime::shared_function_instance_data::SharedFunctionInstanceData;
use crate::runtime::symbol::{self, GlobalSymbolRegistry, Symbol, enumerate_well_known_symbols};
use crate::source_range::SourceRange;
use crate::utf16::Utf16View;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EvalMode {
    Direct,
    Indirect,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CompilationType {
    DirectEval,
    IndirectEval,
    Function,
    Timer,
}

/// HostEnsureCanAddPrivateElement, which hosts that are web browsers may override.
pub type HostEnsureCanAddPrivateElement = fn(&Vm, &Object) -> ThrowCompletionOr<()>;

/// HostGetCodeForEval, which hosts may override.
pub type HostGetCodeForEval = fn(&Vm, &Object) -> Option<Gc<PrimitiveString>>;

/// HostEnsureCanCompileStrings ( calleeRealm, parameterStrings, bodyString, codeString, compilationType,
/// parameterArgs, bodyArg ), which hosts may override.
pub type HostEnsureCanCompileStrings = fn(
    &Vm,
    Gc<Realm>,
    &[Utf16String],
    Utf16View<'_>,
    Utf16View<'_>,
    CompilationType,
    &[Value],
    Value,
) -> ThrowCompletionOr<()>;

// 1 HostGetCodeForEval ( argument ), https://tc39.es/proposal-dynamic-code-brand-checks/#sec-hostgetcodeforeval
fn default_host_get_code_for_eval(_: &Vm, _: &Object) -> Option<Gc<PrimitiveString>> {
    // The host-defined abstract operation HostGetCodeForEval takes argument argument (an Object) and returns a
    // String or NO-CODE. It allows host environments to return a String of code from argument to be used by eval,
    // rather than eval returning argument.
    //
    // argument represents the Object to be checked for code.
    //
    // The default implementation of HostGetCodeForEval is to return NO-CODE.
    None
}

// 2 HostEnsureCanCompileStrings ( calleeRealm, parameterStrings, bodyString, codeString, compilationType, parameterArgs, bodyArg ), https://tc39.es/proposal-dynamic-code-brand-checks/#sec-hostensurecancompilestrings
#[allow(clippy::too_many_arguments, reason = "the hook takes the spec's arguments")]
#[allow(clippy::unnecessary_wraps, reason = "the hook's type lets other hosts throw")]
fn default_host_ensure_can_compile_strings(
    _: &Vm,
    _: Gc<Realm>,
    _: &[Utf16String],
    _: Utf16View<'_>,
    _: Utf16View<'_>,
    _: CompilationType,
    _: &[Value],
    _: Value,
) -> ThrowCompletionOr<()> {
    // The host-defined abstract operation HostEnsureCanCompileStrings takes arguments calleeRealm (a Realm Record),
    // parameterStrings (a List of Strings), bodyString (a String), and direct (a Boolean) and returns either a normal
    // completion containing unused or a throw completion.
    //
    // It allows host environments to block certain ECMAScript functions which allow developers to compile strings into ECMAScript code.
    // An implementation of HostEnsureCanCompileStrings must conform to the following requirements:
    //   - If the returned Completion Record is a normal completion, it must be a normal completion containing unused.
    // The default implementation of HostEnsureCanCompileStrings is to return NormalCompletion(unused).
    Ok(())
}

#[allow(clippy::unnecessary_wraps, reason = "the hook's type lets other hosts throw")]
fn default_host_ensure_can_add_private_element(_: &Vm, _: &Object) -> ThrowCompletionOr<()> {
    // The host-defined abstract operation HostEnsureCanAddPrivateElement takes argument O (an Object)
    // and returns either a normal completion containing unused or a throw completion.
    // It allows host environments to prevent the addition of private elements to particular host-defined exotic objects.
    // An implementation of HostEnsureCanAddPrivateElement must conform to the following requirements:
    // - If O is not a host-defined exotic object, this abstract operation must return NormalCompletion(unused) and perform no other steps.
    // - Any two calls of this abstract operation with the same argument must return the same kind of Completion Record.
    // The default implementation of HostEnsureCanAddPrivateElement is to return NormalCompletion(unused).
    Ok(())

    // This abstract operation is only invoked by ECMAScript hosts that are web browsers.
    // NOTE: Since LibJS has no way of knowing whether the current environment is a browser we always
    //       call HostEnsureCanAddPrivateElement when needed.
}

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
    /// The index of each function in the native function table, by its address and type.
    native_function_indices: RefCell<HashMap<(usize, u32), u32>>,
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
    global_symbol_registry: OnceCell<Gc<GlobalSymbolRegistry>>,

    /// The executables whose inline caches the sweep callback prunes. The list is weak: the callback drops the
    /// executables that die.
    executables: RefCell<Vec<Gc<Executable>>>,
    static_property_lookup_caches: StaticPropertyLookupCaches,
    keyed_property_lookup_cache: KeyedPropertyLookupCache,
    host_ensure_can_add_private_element: Cell<HostEnsureCanAddPrivateElement>,
    host_get_code_for_eval: Cell<HostGetCodeForEval>,
    host_ensure_can_compile_strings: Cell<HostEnsureCanCompileStrings>,
    /// The id the next PrivateEnvironment gives its names, the C++ static PrivateEnvironment::s_next_id. It starts
    /// at one such that 0 can be invalid / default initialized.
    next_private_environment_id: Cell<u64>,
    /// The properties defined with Object::define_intrinsic_accessor that have not been read yet, by the address of
    /// their object, the C++ static intrinsic_accessor_map(). The objects are weak: the sweep callback forgets the
    /// ones that die.
    intrinsic_accessors: RefCell<HashMap<usize, HashMap<Utf16FlyString, IntrinsicAccessor>>>,
    /// The realm TypeErrors are created in while a TypeErrorRealmScope is active, at the execution context stack
    /// depth it was created at.
    type_error_realm_override: Cell<Option<Gc<Realm>>>,
    type_error_realm_override_depth: Cell<usize>,
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
            native_function_indices: RefCell::new(HashMap::new()),
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
            global_symbol_registry: OnceCell::new(),
            executables: RefCell::new(Vec::new()),
            static_property_lookup_caches: StaticPropertyLookupCaches::new(),
            keyed_property_lookup_cache: KeyedPropertyLookupCache::new(),
            host_ensure_can_add_private_element: Cell::new(default_host_ensure_can_add_private_element),
            host_get_code_for_eval: Cell::new(default_host_get_code_for_eval),
            host_ensure_can_compile_strings: Cell::new(default_host_ensure_can_compile_strings),
            next_private_environment_id: Cell::new(1),
            intrinsic_accessors: RefCell::new(HashMap::new()),
            type_error_realm_override: Cell::new(None),
            type_error_realm_override_depth: Cell::new(0),
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
        let _ = self.global_symbol_registry.set(GlobalSymbolRegistry::create(self));
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

    /// The Realm of the running execution context.
    pub fn current_realm(&self) -> Option<Gc<Realm>> {
        let context = self.running_execution_context()?;
        // SAFETY: The running execution context is live.
        unsafe { context.as_ref() }.realm.get()
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
        self.global_symbol_registry.trace(visitor);
        self.type_error_realm_override.trace(visitor);
    }

    /// Forgets the intrinsic accessors of objects that died in this collection, as JS::Object::~Object does.
    fn remove_dead_objects_from_intrinsic_accessors(&self) {
        self.intrinsic_accessors.borrow_mut().retain(|&object, _| {
            // SAFETY: The map only holds objects that were live when their accessors were defined, and a dead object
            //         is intact until the sweep that follows this callback.
            let object = unsafe { Gc::from_non_null(NonNull::new_unchecked(object as *mut Object)) };
            !cell_is_dead(object)
        });
    }

    pub fn intrinsic_accessors(&self) -> &RefCell<HashMap<usize, HashMap<Utf16FlyString, IntrinsicAccessor>>> {
        &self.intrinsic_accessors
    }

    /// The realm vm.throw_completion() creates a TypeError in.
    pub(crate) fn type_error_realm(&self) -> Option<Gc<Realm>> {
        if let Some(realm) = self.type_error_realm_override.get()
            && self.execution_context_stack.borrow().len() == self.type_error_realm_override_depth.get()
        {
            return Some(realm);
        }
        self.current_realm()
    }

    /// Has TypeErrors thrown at the current execution context stack depth created in `realm` until the scope ends.
    /// The override only applies at the depth the scope was created at, so callees pushed while it is alive are
    /// unaffected.
    pub fn type_error_realm_scope(&self, realm: Gc<Realm>) -> TypeErrorRealmScope<'_> {
        let scope = TypeErrorRealmScope {
            vm: self,
            previous_realm: self.type_error_realm_override.get(),
            previous_depth: self.type_error_realm_override_depth.get(),
        };
        self.type_error_realm_override.set(Some(realm));
        self.type_error_realm_override_depth
            .set(self.execution_context_stack.borrow().len());
        scope
    }

    /// The frames of every execution context, from the running one down, with where each is in its executable.
    pub fn stack_trace(&self) -> Vec<StackTraceElement> {
        let mut stack_trace = Vec::new();
        self.for_each_execution_context_top_to_bottom(|context| {
            let source_range = context
                .executable
                .get()
                .map(|executable| Executable::from_head(executable).get_source_range(context.program_counter.get()));
            stack_trace.push(StackTraceElement {
                execution_context: NonNull::from(context),
                source_range,
            });
        });
        stack_trace
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

    /// Forgets the cells that died in this collection from the inline caches of executables and static call sites, as
    /// the C++ heap does when it prunes its weak containers.
    fn remove_dead_cells_from_property_lookup_caches(&self) {
        self.executables.borrow_mut().retain(|executable| {
            if cell_is_dead(*executable) {
                return false;
            }
            executable.remove_dead_cells();
            true
        });
        self.static_property_lookup_caches.remove_dead_entries();
        self.keyed_property_lookup_cache.remove_dead_entries();
    }

    pub fn register_executable(&self, executable: Gc<Executable>) {
        self.executables.borrow_mut().push(executable);
    }

    pub fn static_property_lookup_cache(&self, site: StaticPropertyLookupCacheSite) -> &PropertyLookupCache {
        self.static_property_lookup_caches.get(site)
    }

    pub fn keyed_property_lookup_cache(&self) -> &KeyedPropertyLookupCache {
        &self.keyed_property_lookup_cache
    }

    /// The executable of the running execution context.
    pub fn current_executable(&self) -> Gc<Executable> {
        let context = self
            .running_execution_context()
            .expect("there is a running execution context");
        // SAFETY: The running execution context is live.
        let executable = unsafe { context.as_ref() }
            .executable
            .get()
            .expect("the running execution context runs an executable");
        // SAFETY: An Executable starts with its head.
        unsafe { Gc::from_non_null(executable.as_non_null().cast()) }
    }

    pub fn host_ensure_can_add_private_element(&self) -> HostEnsureCanAddPrivateElement {
        self.host_ensure_can_add_private_element.get()
    }

    pub fn set_host_ensure_can_add_private_element(&self, hook: HostEnsureCanAddPrivateElement) {
        self.host_ensure_can_add_private_element.set(hook);
    }

    pub fn host_get_code_for_eval(&self) -> HostGetCodeForEval {
        self.host_get_code_for_eval.get()
    }

    pub fn set_host_get_code_for_eval(&self, hook: HostGetCodeForEval) {
        self.host_get_code_for_eval.set(hook);
    }

    pub fn host_ensure_can_compile_strings(&self) -> HostEnsureCanCompileStrings {
        self.host_ensure_can_compile_strings.get()
    }

    pub fn set_host_ensure_can_compile_strings(&self, hook: HostEnsureCanCompileStrings) {
        self.host_ensure_can_compile_strings.set(hook);
    }

    pub fn next_private_environment_id(&self) -> &Cell<u64> {
        &self.next_private_environment_id
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

    pub fn global_symbol_registry(&self) -> Gc<GlobalSymbolRegistry> {
        *self
            .global_symbol_registry
            .get()
            .expect("the VM creates the global symbol registry")
    }

    pub fn register_native_function(&self, entry: NativeFunctionTableEntry) -> u32 {
        let function = entry.function.expect("a native function has a function pointer");
        let key = (function as usize, entry.function_type as u32);
        if let Some(index) = self.native_function_indices.borrow().get(&key) {
            return *index;
        }

        let mut table = self.native_function_table.borrow_mut();
        assert!(table.len() < u32::MAX as usize);
        let index = u32::try_from(table.len()).expect("native function index fits in u32");
        table.push(entry);
        self.native_function_indices.borrow_mut().insert(key, index);
        self.head.native_function_table_data.set(table.as_ptr());
        index
    }

    pub fn native_function(&self, index: u32, expected_type: NativeFunctionType) -> RawNativeFunctionPointer {
        let table = self.native_function_table.borrow();
        let entry = &table[index as usize];
        assert!(entry.function_type == expected_type);
        assert!(entry.function.is_some());
        entry.function
    }

    /// Pushes `context`, unless so little of the native stack is left that the next call could overflow it.
    pub fn push_execution_context_checking_stack_space(
        &self,
        context: NonNull<ExecutionContext>,
    ) -> ThrowCompletionOr<()> {
        // Ensure we got some stack space left, so the next function call doesn't kill us.
        if self.did_reach_stack_space_limit() {
            return self.throw_completion(ErrorKind::InternalError, ErrorType::CallStackSizeExceeded, &[]);
        }
        self.push_execution_context(context);
        Ok(())
    }

    // 9.4.1 GetActiveScriptOrModule ( ), https://tc39.es/ecma262/#sec-getactivescriptormodule
    pub fn get_active_script_or_module(&self) -> ScriptOrModule {
        // 1. If the execution context stack is empty, return null.
        if self.running_execution_context().is_none() {
            return ScriptOrModule::Empty;
        }

        // 2. Let ec be the topmost execution context on the execution context stack whose ScriptOrModule component is not null.
        let mut script_or_module = ScriptOrModule::Empty;
        self.for_each_execution_context_top_to_bottom(|execution_context| {
            if matches!(script_or_module, ScriptOrModule::Empty) {
                script_or_module = execution_context.script_or_module.get();
            }
        });

        // 3. If no such execution context exists, return null. Otherwise, return ec's ScriptOrModule.
        script_or_module
    }

    fn running_execution_context_ref(&self) -> &ExecutionContext {
        let context = self
            .running_execution_context()
            .expect("there is a running execution context");
        // SAFETY: The running execution context is live while it runs, which outlasts any use of this reference by
        // the code running in it.
        unsafe { context.as_ref() }
    }

    /// The number of arguments the running execution context has slots for.
    pub fn argument_count(&self) -> usize {
        self.running_execution_context_ref().argument_count.get() as usize
    }

    pub fn argument(&self, index: usize) -> Value {
        self.running_execution_context_ref().argument(index)
    }

    pub fn this_value(&self) -> Value {
        let this_value = self.running_execution_context_ref().this_value.get();
        assert!(!this_value.is_empty(), "the running execution context has a this value");
        this_value
    }

    pub fn lexical_environment(&self) -> Option<Gc<Environment>> {
        self.running_execution_context_ref().lexical_environment.get()
    }

    pub fn active_function_object(&self) -> Option<Gc<FunctionObject>> {
        self.running_execution_context_ref().function.get()
    }

    pub fn active_shared_function_data(&self) -> Option<Gc<SharedFunctionInstanceData>> {
        let function = self.active_function_object()?;
        // NB: NativeJavaScriptBackedFunction has shared data as well, once the runtime has it.
        as_ecmascript_function_object(function).map(|function| function.shared_data())
    }
}

impl Vm {
    pub fn execution_context_stack_is_empty(&self) -> bool {
        self.execution_context_stack.borrow().is_empty()
    }

    pub fn variable_environment(&self) -> Option<Gc<Environment>> {
        self.running_execution_context_ref().variable_environment.get()
    }

    /// The Realm of the running execution context, which C++ VM::realm() dereferences.
    fn running_realm(&self) -> Gc<Realm> {
        self.current_realm().expect("the running execution context has a realm")
    }

    pub fn global_object(&self) -> Gc<Object> {
        self.running_realm().global_object()
    }

    pub fn global_declarative_environment(&self) -> Gc<DeclarativeEnvironment> {
        self.running_realm().global_declarative_environment()
    }

    // 9.1.2.1 GetIdentifierReference ( env, name, strict ), https://tc39.es/ecma262/#sec-getidentifierreference
    pub fn get_identifier_reference(
        &self,
        environment: Option<Gc<Environment>>,
        name: Utf16FlyString,
        strict: Strict,
        hops: usize,
    ) -> ThrowCompletionOr<Reference> {
        // 1. If env is the value null, then
        let Some(environment) = environment else {
            // a. Return the Reference Record { [[Base]]: unresolvable, [[ReferencedName]]: name, [[Strict]]: strict, [[ThisValue]]: empty }.
            return Ok(Reference::with_base_type(
                BaseType::Unresolvable,
                PropertyKey::from(name),
                strict,
            ));
        };

        // 2. Let exists be ? env.HasBinding(name).
        let mut index = None;
        let exists = environment.has_binding(self, &name, Some(&mut index))?;

        // Note: This is an optimization for looking up the same reference.
        let environment_coordinate = index.map(|index| EnvironmentCoordinate {
            hops: u32::try_from(hops).expect("the hop count fits in u32"),
            index: u32::try_from(index).expect("the binding index fits in u32"),
        });

        // 3. If exists is true, then
        if exists {
            // a. Return the Reference Record { [[Base]]: env, [[ReferencedName]]: name, [[Strict]]: strict, [[ThisValue]]: empty }.
            return Ok(Reference::with_base_environment(
                environment,
                name,
                strict,
                environment_coordinate,
            ));
        }
        // 4. Else,
        // a. Let outer be env.[[OuterEnv]].
        // b. Return ? GetIdentifierReference(outer, name, strict).
        self.get_identifier_reference(environment.outer_environment(), name, strict, hops + 1)
    }

    // 9.4.2 ResolveBinding ( name [ , env ] ), https://tc39.es/ecma262/#sec-resolvebinding
    pub fn resolve_binding(
        &self,
        name: &Utf16FlyString,
        strict: Strict,
        environment: Option<Gc<Environment>>,
    ) -> ThrowCompletionOr<Reference> {
        // 1. If env is not present or if env is undefined, then
        //     a. Set env to the running execution context's LexicalEnvironment.
        let environment = environment.or_else(|| self.lexical_environment());

        // 2. Assert: env is an Environment Record.
        let environment = environment.expect("ResolveBinding has an environment to resolve in");

        // 3. If the source text matched by the syntactic production that is being evaluated is contained in strict mode code, let strict be true; else let strict be false.
        // NOTE: We take this as a parameter.

        // 4. Return ? GetIdentifierReference(env, name, strict).
        self.get_identifier_reference(Some(environment), name.clone(), strict, 0)

        // NOTE: The spec says:
        //       Note: The result of ResolveBinding is always a Reference Record whose [[ReferencedName]] field is name.
        //       But this is not actually correct as GetIdentifierReference (or really the methods it calls) can throw.
    }

    // 9.4.4 ResolveThisBinding ( ), https://tc39.es/ecma262/#sec-resolvethisbinding
    pub fn resolve_this_binding(&self) -> ThrowCompletionOr<Value> {
        // 1. Let envRec be GetThisEnvironment().
        let environment = get_this_environment(self);

        // 2. Return ? envRec.GetThisBinding().
        environment.get_this_binding(self)
    }

    // 9.4.5 GetNewTarget ( ), https://tc39.es/ecma262/#sec-getnewtarget
    pub fn get_new_target(&self) -> Value {
        // 1. Let envRec be GetThisEnvironment().
        let environment = get_this_environment(self);

        // 2. Assert: envRec has a [[NewTarget]] field.
        // 3. Return envRec.[[NewTarget]].
        environment
            .downcast::<FunctionEnvironment>()
            .expect("the this environment of new.target is a function environment")
            .new_target()
    }

    // 9.4.5 GetGlobalObject ( ), https://tc39.es/ecma262/#sec-getglobalobject
    pub fn get_global_object(&self) -> Gc<Object> {
        // 1. Let currentRealm be the current Realm Record.
        let current_realm = self.current_realm().expect("there is a current realm");

        // 2. Return currentRealm.[[GlobalObject]].
        current_realm.global_object()
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
    vm.remove_dead_cells_from_property_lookup_caches();
    vm.remove_dead_objects_from_intrinsic_accessors();
}

/// VM::TypeErrorRealmScope.
pub struct TypeErrorRealmScope<'vm> {
    vm: &'vm Vm,
    previous_realm: Option<Gc<Realm>>,
    previous_depth: usize,
}

impl Drop for TypeErrorRealmScope<'_> {
    fn drop(&mut self) {
        self.vm.type_error_realm_override.set(self.previous_realm);
        self.vm.type_error_realm_override_depth.set(self.previous_depth);
    }
}

/// An element of VM::stack_trace(): an execution context and where it is in its executable, if it runs one.
pub struct StackTraceElement {
    pub execution_context: NonNull<ExecutionContext>,
    pub source_range: Option<SourceRange>,
}
