/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the generated bindings of an interface do with its wrappers, its interface object and its interface prototype
//! object, https://webidl.spec.whatwg.org/#es-interfaces
//!
//! An object of an interface implemented in Rust is a cell that holds the implementation, a `Ref<T>`, which is what
//! implementations hold and pass each other. A wrapper of such an object is a platform object of the interface's
//! wrapper class, which LibWeb's C++ bindings define. Its host data is the cell, and its wrappable slot stays empty,
//! so the C++ bindings treat it like a platform object without a wrappable.
//!
//! An object has one wrapper per wrapper world, as an implementation object of the C++ bindings has
//! (Documentation/WrapperArchitecture.md), and wrap() finds or creates it.

use core::cell::Cell;
use core::ffi::c_void;
use core::ops::Deref;
use core::ptr::NonNull;

use ak::Utf16FlyString;
use libjs_rust::api::{
    Attribute, DeferGc, EmbedderCell, EmbedderClass, EmbedderValue, ErrorKind, ErrorType, ExternalMemorySize,
    FunctionObject, Gc, HostObject, JSHostClass, Object, PrimitiveString, PropertyAttributes, PropertyKey,
    RawNativeFunction, RawNativeFunctionPointer, Realm, Throw, ThrowCompletionOr, Trace, Value, Vm, get_function_realm,
    host_data_of, is_host_instance_of,
};

use crate::bindings::CallContext;
use crate::bindings::realm_data::wrapper_world_of;
use crate::bindings::services::{
    web_bindings_ensure_web_constructor, web_bindings_ensure_web_prototype, web_bindings_realm_is_window,
    web_bindings_rejected_promise, web_bindings_rust_wrapper_for, web_bindings_set_rust_wrapper,
    web_bindings_with_temporary_execution_context,
};

/// What an implementation can tell the bindings about its objects, as a C++ implementation object does by overriding
/// the virtual functions of Web::Bindings::Wrappable.
pub(crate) trait Wrappable {
    /// The realm of the relevant global object of the object, which the wrapper of the object in the main world is
    /// created in, as Wrappable::relevant_global_impl() decides for a C++ implementation object. Without one, the
    /// wrapper is created in the realm that first asks for it.
    fn relevant_realm(&self, _vm: &Vm) -> Option<Gc<Realm>> {
        None
    }

    /// Whether the object owns memory outside the heap, which LibGC then asks external_memory_size() for while it
    /// sweeps and counts towards the bytes that make it collect, as Cell::external_memory_size() of a C++
    /// implementation object.
    const OWNS_EXTERNAL_MEMORY: bool = false;

    /// How much memory outside the heap the object owns, which LibGC asks for if OWNS_EXTERNAL_MEMORY is true.
    fn external_memory_size(&self) -> usize {
        0
    }

    /// Whether LibGC finalizes the object before it destroys it, which it does through finalize(), as it does a C++
    /// implementation object that overrides Cell::finalize().
    const FINALIZES: bool = false;

    /// What the object does when LibGC finalizes it, while every cell that dies in the collection is still intact.
    /// It must not allocate.
    fn finalize(_object: Ref<Self>)
    where
        Self: Trace + Sized,
    {
    }
}

/// The value of the cell of an object of an interface: its implementation, and its wrapper in the main world.
///
/// The cell keeps that wrapper alive, and the wrapper keeps the cell alive through its host data, so the two live and
/// die together: the object has the same wrapper in the main world for as long as it lives. A C++ implementation
/// object holds its wrapper weakly until script makes the wrapper observable, which the wrapper of an interface
/// implemented in Rust has no hooks to tell.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Wrapped<T: Trace> {
    main_world_wrapper: Cell<Option<Gc<HostObject>>>,
    implementation: T,
}

impl<T: Trace> Deref for Wrapped<T> {
    type Target = T;

    fn deref(&self) -> &T {
        &self.implementation
    }
}

/// An object of the interface that `T` implements.
pub(crate) type Ref<T> = Gc<EmbedderCell<Wrapped<T>>>;

/// What the implementation of an interface that inherits from another implements, for the implementation of that
/// interface: where an object of the interface keeps what it is as an object of the inherited interface. The
/// attributes and operations of the inherited interface run on that.
pub(crate) trait Inherits<Parent> {
    fn parent(&self) -> &Parent;
}

/// An implementation of an interface, as the generated bindings of the interface describe it.
pub(crate) trait Interface: Wrappable + Trace + Sized + 'static {
    /// The class of the cells of the objects of the interface.
    const CELL_CLASS: &'static EmbedderClass<Wrapped<Self>>;
    /// Web::Bindings::InterfaceName of the interface.
    const INTERFACE_NAME: u16;
    /// The name that errors call the interface.
    const NAME: &'static str;

    /// The class of the wrappers of the objects of the interface.
    fn wrapper_class() -> &'static JSHostClass;
}

impl<T: Interface> EmbedderValue for Wrapped<T> {
    const CLASS: &'static EmbedderClass<Self> = T::CELL_CLASS;
}

impl<T: Wrappable + Trace> ExternalMemorySize for Wrapped<T> {
    fn external_memory_size(&self) -> usize {
        self.implementation.external_memory_size()
    }
}

/// The class of the cells of the objects of an interface, which the generated bindings of the interface define once.
pub(crate) const fn cell_class<T: Wrappable + Trace + 'static>(name: &'static str) -> EmbedderClass<Wrapped<T>> {
    let class = if T::OWNS_EXTERNAL_MEMORY {
        EmbedderClass::new_with_external_memory_size(name)
    } else {
        EmbedderClass::new(name)
    };
    if T::FINALIZES {
        class.with_finalize(finalize_object::<T>)
    } else {
        class
    }
}

/// The finalize entry of the class of the cells of an interface whose implementation finalizes its objects.
///
/// # Safety
///
/// Only LibGC calls it, with a dead cell of the class.
unsafe extern "C" fn finalize_object<T: Wrappable + Trace + 'static>(cell: *mut c_void) {
    let cell = NonNull::new(cell.cast::<EmbedderCell<Wrapped<T>>>()).expect("LibGC passes a cell");
    // SAFETY: LibGC passes a dead cell of the class, which stays intact while it finalizes.
    T::finalize(unsafe { Gc::from_non_null(cell) });
}

/// Without the generated bindings, which a build of the crate on its own leaves out, nothing creates or wraps an
/// object, but the implementations still name each other's objects. This stands in for the bindings of every
/// interface so that they compile.
#[cfg(not(web_idl_bindings))]
impl<T: Wrappable + Trace + Sized + 'static> Interface for T {
    const CELL_CLASS: &'static EmbedderClass<Wrapped<Self>> = &EmbedderClass::new("");
    const INTERFACE_NAME: u16 = 0;
    const NAME: &'static str = "";

    fn wrapper_class() -> &'static JSHostClass {
        unreachable!("an interface has no wrappers without its bindings")
    }
}

/// A new object of the interface, built as Heap::allocate() builds a cell.
pub(crate) fn allocate<T: Interface>(vm: &Vm, build: impl FnOnce(&DeferGc<'_>) -> T) -> Ref<T> {
    T::CELL_CLASS.allocate(vm, |defer_gc| Wrapped {
        main_world_wrapper: Cell::new(None),
        implementation: build(defer_gc),
    })
}

/// What the constructor steps or the transfer-receiving steps of an interface create: the implementation of the new
/// object, which the bindings allocate, or the new object itself, for steps that hand the object to something else
/// before they finish, as the constructor of ReadableStream gives it to its controller.
pub(crate) trait ConstructedObject<T: Interface> {
    fn into_object(self, vm: &Vm) -> Ref<T>;
}

impl<T: Interface> ConstructedObject<T> for T {
    fn into_object(self, vm: &Vm) -> Ref<T> {
        allocate(vm, |_| self)
    }
}

impl<T: Interface> ConstructedObject<T> for Ref<T> {
    fn into_object(self, _: &Vm) -> Ref<T> {
        self
    }
}

/// The object an attribute or an operation of an interface runs on, if it is a wrapper of the interface or of an
/// interface that inherits from it, with the object's realm. A nullish this value stands for the global object of
/// the current realm.
pub(crate) fn this_object(
    vm: &Vm,
    wrapper_class: &'static JSHostClass,
    interface_name: &str,
) -> ThrowCompletionOr<(Gc<Object>, Gc<Realm>)> {
    let mut this_value = vm.this_value();
    if this_value.is_nullish() {
        let current_realm = vm.current_realm().expect("a native function runs in a realm");
        this_value = Value::from_object(current_realm.global_object());
    }
    if this_value.is_object() {
        let object = this_value.as_object();
        if is_host_instance_of(&object, wrapper_class) {
            return Ok((object, object.shape().realm()));
        }
    }
    vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&interface_name])
}

/// The object that a wrapper of exactly the interface wraps.
pub(crate) fn implementation_of<T: Interface>(wrapper: Gc<Object>) -> Ref<T> {
    implementation_if(wrapper).expect("a wrapper holds the cell of its object")
}

/// The object that the wrapper of an interface implemented in Rust wraps, if it is an object of exactly this
/// interface and not of one that inherits from it.
pub(crate) fn implementation_if<T: Interface>(wrapper: Gc<Object>) -> Option<Ref<T>> {
    let host_data = host_data_of(&wrapper).expect("a wrapper holds the cell of its object");
    // SAFETY: The wrapper is a host object of the wrapper class of an interface implemented in Rust, whose host data
    //         the bindings set to the cell of an object of such an interface.
    unsafe { T::CELL_CLASS.downcast(host_data) }
}

/// An object of the interface converted from a JS value, https://webidl.spec.whatwg.org/#js-interface
pub(crate) fn to_interface<T: Interface>(vm: &Vm, value: Value) -> ThrowCompletionOr<Ref<T>> {
    // 1. If V implements I, then return the IDL interface type value that represents a reference to that platform
    //    object.
    if value.is_object() {
        let object = value.as_object();
        if is_host_instance_of(&object, T::wrapper_class()) {
            return Ok(implementation_of(object));
        }
    }
    // 2. Throw a TypeError.
    vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&T::NAME])
}

/// The wrapper of the object in the wrapper world of the realm of the call context, which the first call for that
/// world creates, as Web::Bindings::wrap() does for a C++ implementation object.
pub(crate) fn wrap<T: Interface>(cx: &CallContext, object: Ref<T>) -> Gc<Object> {
    let vm = cx.vm();
    let realm = cx.realm();
    let main_world_wrapper = object.main_world_wrapper.get();
    if let Some(wrapper) = main_world_wrapper
        && wrapper.shape().realm() == realm
    {
        return wrapper.upcast();
    }

    let wrapper_world = wrapper_world_of(vm, realm);
    if wrapper_world.is_main_world() {
        if let Some(wrapper) = main_world_wrapper {
            // NB: An agent has one main world, which the realms of the agent share, and objects do not cross agents.
            assert!(
                wrapper_world_of(vm, wrapper.shape().realm()) == wrapper_world,
                "an object has a wrapper in the main world of another agent"
            );
            return wrapper.upcast();
        }
        let wrapper_realm = object
            .relevant_realm(vm)
            .filter(|relevant_realm| wrapper_world_of(vm, *relevant_realm) == wrapper_world)
            .unwrap_or(realm);
        let wrapper = create_wrapper(
            vm,
            wrapper_realm,
            interface_prototype_object(wrapper_realm, T::INTERFACE_NAME),
            object,
        );
        object.main_world_wrapper.set(Some(wrapper));
        return wrapper.upcast();
    }

    // SAFETY: The realm and the cell are live.
    let wrapper = unsafe { web_bindings_rust_wrapper_for(realm.as_ptr().cast(), object.as_ptr().cast()) };
    if let Some(wrapper) = NonNull::new(wrapper.cast::<Object>()) {
        // SAFETY: LibWeb returns a live wrapper.
        return unsafe { Gc::from_non_null(wrapper) };
    }
    let wrapper = create_wrapper(vm, realm, interface_prototype_object(realm, T::INTERFACE_NAME), object);
    register_wrapper_outside_main_world(realm, object, wrapper);
    wrapper.upcast()
}

/// The wrapper that a construction creates for the new object of the interface, with the prototype that the new
/// target of the construction decides.
pub(crate) fn construct_wrapper<T: Interface>(
    vm: &Vm,
    realm: Gc<Realm>,
    prototype: Gc<Object>,
    object: impl ConstructedObject<T>,
) -> Gc<HostObject> {
    let object = object.into_object(vm);
    assert!(
        object.main_world_wrapper.get().is_none(),
        "the constructor steps create no wrapper of the new object"
    );
    let wrapper = create_wrapper(vm, realm, prototype, object);
    if wrapper_world_of(vm, realm).is_main_world() {
        object.main_world_wrapper.set(Some(wrapper));
    } else {
        register_wrapper_outside_main_world(realm, object, wrapper);
    }
    wrapper
}

fn register_wrapper_outside_main_world<T: Interface>(realm: Gc<Realm>, object: Ref<T>, wrapper: Gc<HostObject>) {
    // SAFETY: The realm, the cell and the wrapper are live, and the wrapper is in the realm.
    unsafe {
        web_bindings_set_rust_wrapper(realm.as_ptr().cast(), object.as_ptr().cast(), wrapper.as_ptr().cast());
    }
}

/// A new wrapper of the object in the realm, whose host data is the cell of the object.
fn create_wrapper<T: Interface>(vm: &Vm, realm: Gc<Realm>, prototype: Gc<Object>, object: Ref<T>) -> Gc<HostObject> {
    // SAFETY: The wrapper class's hooks expect no wrappable, and the host data is a live cell.
    unsafe {
        HostObject::create(
            vm,
            realm,
            T::wrapper_class(),
            Some(prototype),
            None,
            Some(object.as_non_null().cast()),
        )
    }
}

/// The cell of an object of the interface, as LibWeb's C++ code holds it: a pointer to a class that derives from
/// Web::Bindings::RustInterfaceCell.
pub(crate) fn cell_into_abi<T: Interface>(object: Ref<T>) -> *mut c_void {
    object.as_ptr().cast()
}

/// The object of the interface whose cell LibWeb's C++ code holds.
///
/// # Safety
///
/// `cell` must be a live cell of an object of an interface implemented in Rust.
pub(crate) unsafe fn cell_from_abi<T: Interface>(cell: *mut c_void) -> Ref<T> {
    let cell = NonNull::new(cell).expect("C++ passes the cell of an object");
    // SAFETY: The caller passes a live cell of an embedder class.
    unsafe { T::CELL_CLASS.downcast(cell) }.expect("C++ passes the cell of an object of the interface")
}

/// The object of the interface whose cell LibWeb's C++ code holds, if the cell is that of an object of exactly the
/// interface: C++ names an object of an interface that others inherit from by that interface, whichever it is.
///
/// # Safety
///
/// `cell` must be a live cell of an object of an interface implemented in Rust.
pub(crate) unsafe fn cell_if_abi<T: Interface>(cell: *mut c_void) -> Option<Ref<T>> {
    let cell = NonNull::new(cell).expect("C++ passes the cell of an object");
    // SAFETY: The caller passes a live cell of an embedder class.
    unsafe { T::CELL_CLASS.downcast(cell) }
}

/// Whether the global object of the realm is a Window, which the members that only a Window has, [Exposed=Window] on
/// an interface exposed elsewhere as well, are defined for.
pub(crate) fn realm_is_window(realm: Gc<Realm>) -> bool {
    // SAFETY: The realm is live.
    unsafe { web_bindings_realm_is_window(realm.as_ptr().cast()) }
}

/// Runs the steps while an execution context of the realm is on the stack, for steps that can run script but that do not
/// run on behalf of script, as an HTML::TemporaryExecutionContext does for C++.
pub(crate) fn with_temporary_execution_context<F: FnOnce() -> R, R>(realm: Gc<Realm>, steps: F) -> R {
    run_with_temporary_execution_context(realm, false, steps)
}

/// Runs the steps as with_temporary_execution_context() does, with the realm prepared to run callbacks as well, as an
/// HTML::TemporaryExecutionContext with CallbacksEnabled::Yes does for C++.
pub(crate) fn with_temporary_execution_context_and_callbacks<F: FnOnce() -> R, R>(realm: Gc<Realm>, steps: F) -> R {
    run_with_temporary_execution_context(realm, true, steps)
}

fn run_with_temporary_execution_context<F: FnOnce() -> R, R>(realm: Gc<Realm>, callbacks_enabled: bool, steps: F) -> R {
    unsafe extern "C" fn run_steps<F: FnOnce() -> R, R>(context: *mut c_void) {
        // SAFETY: The context is the steps and the slot for their result, which outlive the call.
        let (steps, result) = unsafe { &mut *context.cast::<(Option<F>, Option<R>)>() };
        *result = Some(steps.take().expect("LibWeb runs the steps once")());
    }

    let mut context = (Some(steps), None);
    // SAFETY: The realm is live, and the context outlives the call, which runs the steps once.
    unsafe {
        web_bindings_with_temporary_execution_context(
            realm.as_ptr().cast(),
            callbacks_enabled,
            (&raw mut context).cast(),
            run_steps::<F, R>,
        );
    }
    context.1.expect("LibWeb runs the steps")
}

/// What an operation that returns a promise type returns in place of throwing: a promise rejected with what its
/// steps threw, https://webidl.spec.whatwg.org/#dfn-create-operation-function
pub(crate) fn rejected_promise(vm: &Vm, throw: Throw) -> Value {
    let current_realm = vm.current_realm().expect("a native function runs in a realm");
    // SAFETY: The realm is live, and the values are live while the native function runs.
    let promise = unsafe {
        web_bindings_rejected_promise(
            current_realm.as_ptr().cast(),
            vm.this_value().to_raw(),
            throw.value().to_raw(),
        )
    };
    let promise = NonNull::new(promise.cast::<Object>()).expect("LibWeb returns the promise");
    // SAFETY: LibWeb returns a live promise.
    Value::from_object(unsafe { Gc::from_non_null(promise) })
}

/// The interface prototype object of the interface in the realm.
pub(crate) fn interface_prototype_object(realm: Gc<Realm>, interface_name: u16) -> Gc<Object> {
    // SAFETY: The realm is live, and the interface is implemented in Rust.
    let prototype = unsafe { web_bindings_ensure_web_prototype(realm.as_ptr().cast(), interface_name) };
    let prototype = NonNull::new(prototype.cast::<Object>()).expect("the realm has the interface prototype object");
    // SAFETY: LibWeb returns the live prototype.
    unsafe { Gc::from_non_null(prototype) }
}

/// The prototype of an object that newTarget constructs, steps 3.2 and 3.3 of internally creating a new object
/// implementing the interface, https://webidl.spec.whatwg.org/#internally-create-a-new-object-implementing-the-interface
pub(crate) fn prototype_from_new_target(
    vm: &Vm,
    new_target: Gc<Object>,
    interface_name: u16,
) -> ThrowCompletionOr<Gc<Object>> {
    // 3.2. Let prototype be ? Get(newTarget, "prototype").
    let prototype = new_target.get(vm, &vm.names.prototype)?;

    // 3.3. If Type(prototype) is not Object, then:
    if prototype.is_object() {
        return Ok(prototype.as_object());
    }
    // 1. Let targetRealm be ? GetFunctionRealm(newTarget).
    let new_target = new_target
        .downcast::<FunctionObject>()
        .expect("the new target of a construction is a constructor");
    let target_realm = get_function_realm(vm, new_target)?;
    // 2. Set prototype to the interface prototype object for interface in targetRealm.
    Ok(interface_prototype_object(target_realm, interface_name))
}

/// Unwraps the raw arguments that LibWeb's C++ bindings pass the entry points that initialize the interface object and
/// the interface prototype object, which run before any binding of the interface. The C++ bindings pass the runtime's
/// VM and cells, as JS::HostABI::vm_to_abi() and object_to_abi() do.
///
/// # Safety
///
/// The pointers must be the live VM, a live realm and a live object.
pub(crate) unsafe fn entry_point_arguments<'vm>(
    vm: *mut c_void,
    realm: *mut c_void,
    object: *mut c_void,
) -> (&'vm Vm, Gc<Realm>, Gc<Object>) {
    let realm = NonNull::new(realm.cast::<Realm>()).expect("C++ passes a realm");
    let object = NonNull::new(object.cast::<Object>()).expect("C++ passes an object");
    // SAFETY: The caller guarantees that the pointers are live.
    unsafe { (&*vm.cast::<Vm>(), Gc::from_non_null(realm), Gc::from_non_null(object)) }
}

/// Unwraps the VM and the realm that LibWeb's C++ bindings pass an entry point.
///
/// # Safety
///
/// The pointers must be the live VM and a live realm.
pub(crate) unsafe fn realm_arguments<'vm>(vm: *mut c_void, realm: *mut c_void) -> (&'vm Vm, Gc<Realm>) {
    let realm = NonNull::new(realm.cast::<Realm>()).expect("C++ passes a realm");
    // SAFETY: The caller guarantees that the pointers are live.
    unsafe { (&*vm.cast::<Vm>(), Gc::from_non_null(realm)) }
}

/// Unwraps the raw arguments that LibWeb's C++ bindings pass the entry point that constructs an object.
///
/// # Safety
///
/// The pointers must be the live VM and the live new target of a construction.
pub(crate) unsafe fn construct_arguments<'vm>(vm: *mut c_void, new_target: *mut c_void) -> (&'vm Vm, Gc<Object>) {
    let new_target = NonNull::new(new_target.cast::<Object>()).expect("a construction has a new target");
    // SAFETY: The caller guarantees that the pointers are live.
    unsafe { (&*vm.cast::<Vm>(), Gc::from_non_null(new_target)) }
}

/// Gives the interface prototype object its [[Prototype]]: the interface prototype object of the interface that the
/// interface inherits from, or %Object.prototype%, https://webidl.spec.whatwg.org/#interface-prototype-object
pub(crate) fn set_interface_prototype_object_prototype(
    vm: &Vm,
    realm: Gc<Realm>,
    prototype: Gc<Object>,
    inherited_interface_name: Option<u16>,
) {
    let parent = match inherited_interface_name {
        Some(inherited_interface_name) => interface_prototype_object(realm, inherited_interface_name),
        None => realm.intrinsics().object_prototype(vm),
    };
    prototype.set_prototype(vm, Some(parent));
}

/// Defines a regular attribute on the interface prototype object, https://webidl.spec.whatwg.org/#es-attributes
pub(crate) fn define_attribute(
    vm: &Vm,
    realm: Gc<Realm>,
    prototype: Gc<Object>,
    name: &str,
    getter: RawNativeFunctionPointer,
    setter: RawNativeFunctionPointer,
) {
    prototype.define_native_accessor(
        vm,
        realm,
        &PropertyKey::from_utf8(name),
        getter,
        setter,
        PropertyAttributes::new(Attribute::ENUMERABLE | Attribute::CONFIGURABLE),
    );
}

/// Defines a regular attribute that is read only and [CachedAccessor] on the interface prototype object: its getter
/// returns the same value for as long as the object lives, so the engine keeps the value on the object after the first
/// get and does not call the getter again, as LibWeb's C++ bindings define such an attribute.
pub(crate) fn define_cached_attribute(
    vm: &Vm,
    realm: Gc<Realm>,
    prototype: Gc<Object>,
    name: &str,
    getter: RawNativeFunctionPointer,
) {
    let property_key = PropertyKey::from_utf8(name);
    let getter = RawNativeFunction::create(vm, getter, 0, &property_key, Some(realm), Some("get"), None);
    prototype.define_direct_cached_accessor(
        vm,
        &property_key,
        Some(getter.upcast()),
        None,
        PropertyAttributes::new(Attribute::ENUMERABLE | Attribute::CONFIGURABLE),
        true,
    );
}

/// Defines a regular operation on the interface prototype object, https://webidl.spec.whatwg.org/#es-operations
pub(crate) fn define_operation(
    vm: &Vm,
    realm: Gc<Realm>,
    prototype: Gc<Object>,
    name: &str,
    behaviour: RawNativeFunctionPointer,
    length: i32,
) {
    prototype.define_native_function(
        vm,
        realm,
        &PropertyKey::from_utf8(name),
        behaviour,
        length,
        PropertyAttributes::new(Attribute::WRITABLE | Attribute::ENUMERABLE | Attribute::CONFIGURABLE),
        None,
    );
}

/// Defines the properties of a value iterator on the interface prototype object of an interface that supports indexed
/// properties, which are those of %Array.prototype%, https://webidl.spec.whatwg.org/#define-the-iteration-methods
pub(crate) fn define_value_iterator_properties(vm: &Vm, realm: Gc<Realm>, prototype: Gc<Object>) {
    let array_prototype = realm.intrinsics().array_prototype(vm);
    let names = &vm.names;
    prototype.define_direct_property(
        vm,
        &PropertyKey::from(vm.well_known_symbols().iterator),
        array_prototype.get_without_side_effects(vm, &names.values),
        PropertyAttributes::new(Attribute::CONFIGURABLE | Attribute::WRITABLE),
    );
    for name in [&names.entries, &names.keys, &names.values, &names.forEach] {
        prototype.define_direct_property(
            vm,
            name,
            array_prototype.get_without_side_effects(vm, name),
            PropertyAttributes::new(Attribute::WRITABLE | Attribute::ENUMERABLE | Attribute::CONFIGURABLE),
        );
    }
}

/// Records a regular attribute or operation that the interface does not implement on the interface prototype object,
/// so that a read of it is reported, without the property being there for script.
pub(crate) fn define_unimplemented_property(vm: &Vm, prototype: Gc<Object>, name: &str) {
    prototype.define_unimplemented_property(vm, Utf16FlyString::from_utf8(name));
}

/// Defines the constants of an interface on its interface object or its interface prototype object,
/// https://webidl.spec.whatwg.org/#define-the-constants
pub(crate) fn define_constants(vm: &Vm, target: Gc<Object>, constants: &[(&str, f64)]) {
    // 1. For each constant const that is a member of definition:
    for (identifier, value) in constants {
        // 2. Let value be the result of converting const's IDL value to a JavaScript value.
        // 3. Let desc be the PropertyDescriptor{[[Writable]]: false, [[Enumerable]]: true, [[Configurable]]: false,
        //    [[Value]]: value}.
        // 4. Let id be const's identifier.
        // 5. Perform ! DefinePropertyOrThrow(target, id, desc).
        target.define_direct_property(
            vm,
            &PropertyKey::from_utf8(identifier),
            Value::from_f64(*value),
            PropertyAttributes::new(Attribute::ENUMERABLE),
        );
    }
}

/// Defines @@toStringTag on the interface prototype object, https://webidl.spec.whatwg.org/#es-interface-prototype-object
pub(crate) fn define_to_string_tag(vm: &Vm, prototype: Gc<Object>, name: &str) {
    prototype.define_direct_property(
        vm,
        &PropertyKey::from(vm.well_known_symbols().to_string_tag),
        ascii_string(vm, name),
        PropertyAttributes::new(Attribute::CONFIGURABLE),
    );
}

/// Gives the interface object its [[Prototype]], if the interface inherits from another: the interface object of that
/// interface. Then defines the "length", "name" and "prototype" properties of the interface object,
/// https://webidl.spec.whatwg.org/#interface-object
pub(crate) fn define_interface_object_properties(
    vm: &Vm,
    realm: Gc<Realm>,
    constructor: Gc<Object>,
    name: &str,
    length: i32,
    interface_name: u16,
    inherited_interface_name: Option<u16>,
) {
    if let Some(inherited_interface_name) = inherited_interface_name {
        // SAFETY: The realm is live, and the interface is implemented in Rust.
        let parent = unsafe { web_bindings_ensure_web_constructor(realm.as_ptr().cast(), inherited_interface_name) };
        let parent = NonNull::new(parent.cast::<Object>()).expect("the realm has the interface object");
        // SAFETY: LibWeb returns the live interface object.
        constructor.set_prototype(vm, Some(unsafe { Gc::from_non_null(parent) }));
    }
    constructor.define_direct_property(
        vm,
        &vm.names.length,
        Value::from_f64(f64::from(length)),
        PropertyAttributes::new(Attribute::CONFIGURABLE),
    );
    constructor.define_direct_property(
        vm,
        &vm.names.name,
        ascii_string(vm, name),
        PropertyAttributes::new(Attribute::CONFIGURABLE),
    );
    let prototype = interface_prototype_object(realm, interface_name);
    constructor.define_direct_property(
        vm,
        &vm.names.prototype,
        Value::from_object(prototype),
        PropertyAttributes::new(0),
    );
}

fn ascii_string(vm: &Vm, string: &str) -> Value {
    Value::from_string(PrimitiveString::create_from_ascii(vm, string.as_bytes()))
}

/// A new object for a dictionary value, https://webidl.spec.whatwg.org/#js-dictionary
pub(crate) fn create_dictionary_object(cx: &CallContext) -> Gc<Object> {
    // 1. Let O be OrdinaryObjectCreate(%Object.prototype%).
    let object_prototype = cx.realm().intrinsics().object_prototype(cx.vm());
    Object::create(cx.vm(), cx.realm(), Some(object_prototype))
}

/// Stores a member of a dictionary value in its object, step 3.2.3 of https://webidl.spec.whatwg.org/#js-dictionary
pub(crate) fn set_dictionary_member(
    cx: &CallContext,
    object: Gc<Object>,
    key: &PropertyKey,
    value: Value,
) -> ThrowCompletionOr<()> {
    // 3. Perform ! CreateDataPropertyOrThrow(O, key, value).
    object.create_data_property(cx.vm(), key, value, None, None)?;
    Ok(())
}
