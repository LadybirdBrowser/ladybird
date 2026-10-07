/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Host modules, of kind JS_HOST_CLASS_MODULE: Cyclic Module Records whose abstract methods an embedder implements,
//! as LibWeb does for WebAssembly module records.
//!
//! The exported functions run on the thread that owns the VM and trust their arguments, as object.rs states: `vm` is
//! the embedder's VM, modules, realms and environments are live cells of its heap, a host class is static data that
//! outlives the VM, and host-defined and host data cells are null or live cells of the VM's heap.

#![allow(
    clippy::missing_safety_doc,
    reason = "the module documentation states the contract every exported function shares"
)]

use core::ffi::c_void;
use core::ops::Deref;

use ak::Utf16FlyString;
use libjs_runtime_macros::Trace;

use crate::embedding::abi_types::{
    JSRealm, JSUtf16View, cell_from_abi, cell_into_abi, completion_from_abi, optional_cell_from_abi, vm_from_abi,
};
use crate::embedding::environment::JSEnvironment;
use crate::embedding::hooks::{JSModuleRequest, module_request_from_abi};
use crate::embedding::host::class_table::host_class_from_abi;
use crate::embedding::host::registry::runtime_class_and_allocator_of_host_class;
use crate::embedding::module::promise_capability_into_abi;
use crate::embedding::realm::host_defined_slot_of;
use crate::gc::class::{Class, define_cell};
use crate::gc::foreign::ForeignCellSlot;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::host_class::{
    JS_HOST_CLASS_MODULE, JS_RESOLVED_BINDING_AMBIGUOUS, JS_RESOLVED_BINDING_BINDING_NAME,
    JS_RESOLVED_BINDING_NAMESPACE, JS_RESOLVED_BINDING_NULL, JSHostClass, JSHostModuleHooks, JSModule,
    JSResolvedBinding, JSStringSink, JSVM,
};
use crate::layout::realm::Realm;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::cyclic_module::{CYCLIC_MODULE_METHODS, CyclicModule};
use crate::runtime::module::{Module, ModuleMethods, ResolvedBinding, ResolvedBindingType};
use crate::runtime::module_environment::ModuleEnvironment;
use crate::runtime::module_request::ModuleRequest;
use crate::runtime::promise_capability::PromiseCapability;

/// A Cyclic Module Record whose abstract methods come from its host class, with a C++ GC cell for any state of its
/// own. It never has top-level await.
#[repr(C)]
#[derive(Trace)]
pub struct HostModule {
    base: CyclicModule,
    #[gc(untraced)]
    host_class: &'static JSHostClass,
    host_data: ForeignCellSlot,
}

define_cell!(HostModule, Other, extends: [CyclicModule, Module]);

impl Deref for HostModule {
    type Target = CyclicModule;

    fn deref(&self) -> &CyclicModule {
        &self.base
    }
}

pub const HOST_MODULE_METHODS: ModuleMethods = ModuleMethods {
    get_exported_names: |module, _, _| as_host_module(module).get_exported_names(),
    resolve_export: |module, _, export_name, _| as_host_module(module).resolve_export(export_name),
    initialize_environment: |module, _| as_host_module(module).initialize_environment(),
    execute_module: |module, _, capability| as_host_module(module).execute_module(capability),
    ..CYCLIC_MODULE_METHODS
};

/// The host module a method of a host module was called on.
fn as_host_module(module: &Module) -> &HostModule {
    module
        .downcast_ref::<HostModule>()
        .expect("only host modules have the methods of host modules")
}

/// The hooks of a host class of kind JS_HOST_CLASS_MODULE, all four of which it must have.
fn module_hooks_of(host_class: &'static JSHostClass) -> &'static JSHostModuleHooks {
    // SAFETY: The hooks of a module class are a JSHostModuleHooks, static like the class itself.
    let hooks =
        unsafe { host_class.hooks.cast::<JSHostModuleHooks>().as_ref() }.expect("a host module class has hooks");
    assert!(
        hooks.get_exported_names.is_some()
            && hooks.resolve_export.is_some()
            && hooks.initialize_environment.is_some()
            && hooks.execute_module.is_some(),
        "a host module class has all four hooks"
    );
    hooks
}

/// The class of the module records of `table`, named after it, which the registry derives from `parent`: the class of
/// the table's parent, or HostModule's for a table without a parent module class.
pub fn derive_host_module_class(table: &'static JSHostClass, parent: &'static Class) -> &'static Class {
    Class::derive_runtime_without_object_methods(parent, table.class_name())
}

unsafe extern "C" fn append_exported_name(names: *mut c_void, code_units: *const u16, length_in_code_units: usize) {
    // SAFETY: The sink's context is the list that get_exported_names() collects, and the embedder passes that many code
    //         units.
    let (names, code_units) = unsafe {
        (
            &mut *names.cast::<Vec<Utf16FlyString>>(),
            code_units_of(code_units, length_in_code_units),
        )
    };
    names.push(Utf16FlyString::from_utf16(code_units));
}

unsafe extern "C" fn set_binding_name(binding_name: *mut c_void, code_units: *const u16, length_in_code_units: usize) {
    // SAFETY: The sink's context is the binding name that resolve_export() waits for, and the embedder passes that many
    //         code units.
    let (binding_name, code_units) = unsafe {
        (
            &mut *binding_name.cast::<Option<Utf16FlyString>>(),
            code_units_of(code_units, length_in_code_units),
        )
    };
    assert!(binding_name.is_none(), "a resolved binding has one name");
    *binding_name = Some(Utf16FlyString::from_utf16(code_units));
}

/// # Safety
///
/// Unless `length` is 0, `code_units` must point to that many code units, which stay unchanged for `'a`.
unsafe fn code_units_of<'a>(code_units: *const u16, length: usize) -> &'a [u16] {
    if length == 0 {
        return &[];
    }
    // SAFETY: The caller passes that many code units.
    unsafe { core::slice::from_raw_parts(code_units, length) }
}

fn resolved_binding_type_from_abi(binding_type: u8) -> ResolvedBindingType {
    match binding_type {
        JS_RESOLVED_BINDING_BINDING_NAME => ResolvedBindingType::BindingName,
        JS_RESOLVED_BINDING_NAMESPACE => ResolvedBindingType::Namespace,
        JS_RESOLVED_BINDING_AMBIGUOUS => ResolvedBindingType::Ambiguous,
        JS_RESOLVED_BINDING_NULL => ResolvedBindingType::Null,
        binding_type => panic!("{binding_type} is not a type of resolved binding"),
    }
}

impl HostModule {
    /// A host module of `host_class`, a Cyclic Module Record that requests `requested_modules`. `host_defined` is its
    /// [[HostDefined]], and `host_data` the embedder's cell for its own state.
    pub fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        host_class: &'static JSHostClass,
        filename: String,
        requested_modules: Vec<ModuleRequest>,
        host_defined: ForeignCellSlot,
        host_data: ForeignCellSlot,
    ) -> Gc<HostModule> {
        let (class, allocator) = runtime_class_and_allocator_of_host_class(vm, host_class, JS_HOST_CLASS_MODULE);
        module_hooks_of(host_class);
        vm.heap().allocate_in(
            allocator,
            HostModule {
                base: CyclicModule::new(class, realm, filename, false, requested_modules, host_defined),
                host_class,
                host_data,
            },
        )
    }

    pub fn host_class(&self) -> &'static JSHostClass {
        self.host_class
    }

    pub fn host_data(&self) -> &ForeignCellSlot {
        &self.host_data
    }

    fn hooks(&self) -> &'static JSHostModuleHooks {
        // SAFETY: create() checked that the hooks of the class are those of a module class.
        unsafe { &*self.host_class.hooks.cast::<JSHostModuleHooks>() }
    }

    fn as_abi(&self) -> *mut JSModule {
        core::ptr::from_ref(self).cast_mut().cast()
    }

    fn get_exported_names(&self) -> Vec<Utf16FlyString> {
        let mut exported_names: Vec<Utf16FlyString> = Vec::new();
        let mut names = JSStringSink {
            context: (&raw mut exported_names).cast(),
            append: Some(append_exported_name),
        };
        let get_exported_names = self
            .hooks()
            .get_exported_names
            .expect("a host module class has all four hooks");
        // SAFETY: The hook takes the module and a sink that outlives the call.
        unsafe { get_exported_names(self.as_abi(), &raw mut names) };
        exported_names
    }

    fn resolve_export(&self, export_name: &Utf16FlyString) -> ResolvedBinding {
        let mut binding_name: Option<Utf16FlyString> = None;
        let mut resolved_binding = JSResolvedBinding {
            module: core::ptr::null_mut(),
            binding_name: JSStringSink {
                context: (&raw mut binding_name).cast(),
                append: Some(set_binding_name),
            },
            r#type: JS_RESOLVED_BINDING_NULL,
        };
        let export_name_code_units = export_name.to_utf16();
        let resolve_export = self
            .hooks()
            .resolve_export
            .expect("a host module class has all four hooks");
        // SAFETY: The hook takes the module, the code units of the name and an out record, all of which outlive the
        //         call.
        unsafe {
            resolve_export(
                self.as_abi(),
                export_name_code_units.as_ptr(),
                export_name_code_units.len(),
                &raw mut resolved_binding,
            );
        };
        let binding_type = resolved_binding_type_from_abi(resolved_binding.r#type);
        ResolvedBinding {
            binding_type,
            // SAFETY: A hook resolving to a binding names a live module.
            module: unsafe { optional_cell_from_abi::<JSModule>(resolved_binding.module) },
            export_name: if binding_type == ResolvedBindingType::BindingName {
                binding_name.expect("a hook resolving to a binding appends its name")
            } else {
                Utf16FlyString::default()
            },
        }
    }

    fn initialize_environment(&self) -> ThrowCompletionOr<()> {
        let initialize_environment = self
            .hooks()
            .initialize_environment
            .expect("a host module class has all four hooks");
        // SAFETY: The hook takes the module.
        completion_from_abi(unsafe { initialize_environment(self.as_abi()) }).map(|_| ())
    }

    fn execute_module(&self, capability: Option<Gc<PromiseCapability>>) -> ThrowCompletionOr<()> {
        let execute_module = self
            .hooks()
            .execute_module
            .expect("a host module class has all four hooks");
        let capability = capability.map_or(core::ptr::null_mut(), promise_capability_into_abi);
        // SAFETY: The hook takes the module and its capability, if it has one.
        completion_from_abi(unsafe { execute_module(self.as_abi(), capability) }).map(|_| ())
    }
}

/// The host class of the module, if it is a host module.
pub fn host_class_of(module: &Module) -> Option<&'static JSHostClass> {
    module.downcast_ref::<HostModule>().map(HostModule::host_class)
}

/// # Safety
///
/// `module` must be a live host module.
unsafe fn host_module_from_abi<'a>(module: *mut JSModule) -> &'a HostModule {
    // SAFETY: The caller passes a live module, which stays alive while the embedder holds it.
    let module = unsafe { &*cell_from_abi::<JSModule>(module).as_ptr() };
    as_host_module(module)
}

/// HostModule::create(): a host module of `host_class`, a JS_HOST_CLASS_MODULE class with all four hooks, in `realm`,
/// named `filename`, which module loading resolves its imports against. It copies the `requested_module_count`
/// module requests of `requested_modules`, its [[RequestedModules]]. `host_defined` is its [[HostDefined]] and
/// `host_data` the embedder's cell for its own state, each a cell of the VM's heap or null, which the module keeps
/// alive. Each host class gets a cell allocator of its own unless it has JS_HOST_CLASS_SHARES_ALLOCATOR_WITH_PARENT.
/// Returns an unrooted module. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_host_module_create(
    vm: *mut JSVM,
    realm: *mut JSRealm,
    host_class: *const JSHostClass,
    filename: JSUtf16View,
    requested_modules: *const *const JSModuleRequest,
    requested_module_count: usize,
    host_defined: *mut c_void,
    host_data: *mut c_void,
) -> *mut JSModule {
    // SAFETY: See the module documentation.
    let (vm, realm, host_class, filename) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi(realm),
            host_class_from_abi(host_class),
            filename.as_view().to_utf8(),
        )
    };
    let requested_modules = if requested_module_count == 0 {
        Vec::new()
    } else {
        // SAFETY: The embedder passes that many module requests.
        unsafe { core::slice::from_raw_parts(requested_modules, requested_module_count) }
            .iter()
            // SAFETY: Each is a module request that outlives the call.
            .map(|&request| unsafe { module_request_from_abi(request) }.clone())
            .collect()
    };
    // SAFETY: The cells are null or live cells of the VM's heap.
    let (host_defined, host_data) = unsafe { (host_defined_slot_of(host_defined), host_defined_slot_of(host_data)) };
    let module = HostModule::create(
        vm,
        realm,
        host_class,
        filename,
        requested_modules,
        host_defined,
        host_data,
    );
    cell_into_abi(module.upcast::<Module>())
}

/// The module's host class, or null for a module record that the runtime implements. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_host_module_host_class(module: *mut JSModule) -> *const JSHostClass {
    // SAFETY: See the module documentation.
    let module = unsafe { cell_from_abi::<JSModule>(module) };
    host_class_of(&module).map_or(core::ptr::null(), core::ptr::from_ref)
}

/// The cell the embedder keeps the host module's own state in, or null if it has none or is not a host module. Main
/// thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_host_module_host_data(module: *mut JSModule) -> *mut c_void {
    // SAFETY: See the module documentation.
    let module = unsafe { cell_from_abi::<JSModule>(module) };
    module
        .downcast_ref::<HostModule>()
        .map_or(core::ptr::null_mut(), |module| module.host_data().as_ptr())
}

/// Sets the host module's [[Environment]] to `environment`, a module environment, as its initialize_environment hook
/// does. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_host_module_set_environment(module: *mut JSModule, environment: *mut JSEnvironment) {
    // SAFETY: See the module documentation.
    let (module, environment) = unsafe {
        (
            host_module_from_abi(module),
            cell_from_abi::<JSEnvironment>(environment),
        )
    };
    let environment = environment
        .downcast::<ModuleEnvironment>()
        .expect("the environment of a module is a module environment");
    module.set_environment(environment);
}
