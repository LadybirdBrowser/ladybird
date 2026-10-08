/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Host objects of kind JS_HOST_CLASS_OBJECT.
//!
//! The exported functions of this file run on the thread that owns the VM, and trust their arguments, as those of
//! embedding/object.rs do: `vm` is the embedder's VM, objects and realms are live cells of its heap, host classes are
//! tables that live as long as the process, and the embedder's cells are null or live GC cells of the same heap.
#![allow(
    clippy::missing_safety_doc,
    reason = "the module documentation states the contract every exported function shares"
)]

use core::ffi::c_void;
use core::ops::Deref;
use core::ptr::NonNull;

use crate::embedding::abi_types::{
    JSRealm, cell_from_abi, completion_from_abi, object_into_abi, optional_cell_from_abi, optional_object_into_abi,
    vm_from_abi,
};
use crate::embedding::error::error_data_from_host_hook;
use crate::embedding::hooks::lend_property_key_to_abi;
use crate::embedding::host::class_table::{
    bool_completion_from_hook, completion_without_result_from_hook, copy_host_class_flags_into_object,
    get_cache_metadata_into_abi, host_class_from_abi, host_class_into_abi, lend_object_to_hook, lookup_phase_into_abi,
    optional_object_completion_from_hook, set_cache_metadata_into_abi,
};
use crate::embedding::host::host_array::as_host_array;
use crate::embedding::host::host_function::as_host_function;
use crate::embedding::host::registry::runtime_class_and_allocator_of_host_class;
use crate::embedding::object::{property_descriptor_from_abi, property_descriptor_to_abi};
use crate::gc::class::{Class, Finalize, define_cell};
use crate::gc::class_id::ClassId;
use crate::gc::foreign::ForeignCellSlot;
use crate::gc::root::MarkedVec;
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::host_class::{
    JS_HOST_CLASS_IMMUTABLE_PROTOTYPE, JS_HOST_CLASS_NOT_CACHEABLE_FOR_PROPERTY_ABSENCE,
    JS_HOST_CLASS_NOT_ELIGIBLE_FOR_OWN_PROPERTY_ENUMERATION_FAST_PATH, JS_HOST_CLASS_OBJECT, JS_PD_CONFIGURABLE,
    JS_PD_ENUMERABLE, JS_PD_HAS_CONFIGURABLE, JS_PD_HAS_ENUMERABLE, JS_PD_HAS_VALUE, JS_PD_HAS_WRITABLE, JS_PD_PRESENT,
    JS_PD_WRITABLE, JSHostClass, JSHostObjectHooks, JSObject, JSPropertyDescriptor, JSVM, JSValue, JSValueSink,
};
pub use crate::layout::host_object::HostObject;
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error_data::ErrorData;
use crate::runtime::object::{
    CacheableGetPropertyMetadata, CacheableSetPropertyMetadata, MayInterfereWithIndexedPropertyAccess,
    ORDINARY_OBJECT_METHODS, Object, ObjectMethods, PropertyLookupPhase, allocate_object_in,
};
use crate::runtime::property_descriptor::PropertyDescriptor;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;

// The class that the class of every host class table of this kind extends. Its own internal methods are ordinary,
// and no object has it as its class.
define_cell!(HostObject, Object, extends: [Object], methods: ORDINARY_OBJECT_METHODS, finalize: finalize);

// SAFETY: Visits the object and the embedder's two cells, which are all the cells a host object reaches.
unsafe impl Trace for HostObject {
    fn trace(&self, visitor: &mut Visitor) {
        self.base.trace(visitor);
        self.wrappable.trace(visitor);
        self.host_data.trace(visitor);
    }
}

impl Finalize for HostObject {
    fn finalize(&self) {
        if let Some(finalize) = self.host_class.host_object_hooks().finalize {
            // SAFETY: The hook takes a dying object of its class, which stays intact while it runs.
            unsafe { finalize(lend_object_to_hook(&self.base)) };
        }
    }
}

impl Deref for HostObject {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

/// The internal methods of the objects of a host class table: those of an ordinary object, with each method that the
/// table has a hook for calling the hook, as modified by the table's flags.
fn host_object_methods(table: &'static JSHostClass) -> ObjectMethods {
    let hooks = table.host_object_hooks();
    let mut methods = ObjectMethods {
        error_data: no_error_data,
        ..ORDINARY_OBJECT_METHODS
    };
    if hooks.get_prototype_of.is_some() {
        methods.internal_get_prototype_of = Some(get_prototype_of_through_hook);
    }
    if hooks.set_prototype_of.is_some() {
        methods.internal_set_prototype_of = set_prototype_of_through_hook;
    } else if table.has_flag(JS_HOST_CLASS_IMMUTABLE_PROTOTYPE) {
        methods.internal_set_prototype_of = Object::set_immutable_prototype;
    }
    if hooks.is_extensible.is_some() {
        methods.internal_is_extensible = is_extensible_through_hook;
    }
    if hooks.prevent_extensions.is_some() {
        methods.internal_prevent_extensions = prevent_extensions_through_hook;
    }
    if hooks.get_own_property.is_some() {
        methods.internal_get_own_property = get_own_property_through_hook;
    }
    if hooks.define_own_property.is_some() {
        methods.internal_define_own_property = define_own_property_through_hook;
    }
    if hooks.has_property.is_some() {
        methods.internal_has_property = has_property_through_hook;
    }
    if hooks.get.is_some() {
        methods.internal_get = get_through_hook;
    }
    if hooks.set.is_some() {
        methods.internal_set = set_through_hook;
    }
    if hooks.delete_property.is_some() {
        methods.internal_delete = delete_through_hook;
    }
    if hooks.own_property_keys.is_some() {
        methods.internal_own_property_keys = own_property_keys_through_hook;
    }
    if table.has_flag(JS_HOST_CLASS_NOT_CACHEABLE_FOR_PROPERTY_ABSENCE) {
        methods.is_cacheable_for_property_absence = |_| false;
    }
    if hooks.is_cacheable_for_inherited_property.is_some() {
        methods.is_cacheable_for_inherited_property = is_cacheable_for_inherited_property_through_hook;
    }
    // The fast paths read keys, attributes and values straight from the shape and storage and follow the shape's
    // prototype, so they would bypass these hooks.
    let hooks_answer_instead_of_the_shape = hooks.get_prototype_of.is_some()
        || hooks.get_own_property.is_some()
        || hooks.has_property.is_some()
        || hooks.get.is_some()
        || hooks.own_property_keys.is_some();
    if hooks_answer_instead_of_the_shape
        || table.has_flag(JS_HOST_CLASS_NOT_ELIGIBLE_FOR_OWN_PROPERTY_ENUMERATION_FAST_PATH)
    {
        methods.eligible_for_own_property_enumeration_fast_path = |_| false;
    }
    if hooks.error_data.is_some() {
        methods.error_data = error_data_through_hook;
    }
    methods
}

/// The class of the objects of `table`, which extends `parent`.
pub fn derive_host_object_class(table: &'static JSHostClass, parent: &'static Class) -> &'static Class {
    Class::derive_runtime(
        parent,
        table.class_name(),
        Box::leak(Box::new(host_object_methods(table))),
    )
}

fn hooks_of(object: &Object) -> &'static JSHostObjectHooks {
    as_host_object(object).host_class.host_object_hooks()
}

fn no_error_data(_: &Object) -> Option<&ErrorData> {
    None
}

// The internal methods of a class whose table has a hook for them. The runtime only installs each for a table with
// that hook, holds no borrow of its state across the call, and lends the hook the object and key for its duration.

fn get_prototype_of_through_hook(object: &Object, _vm: &Vm) -> ThrowCompletionOr<Option<Gc<Object>>> {
    let hook = hooks_of(object)
        .get_prototype_of
        .expect("the class has a [[GetPrototypeOf]] hook");
    // SAFETY: The hook takes an object of its class, and completes with null or a live object.
    unsafe { optional_object_completion_from_hook(hook(lend_object_to_hook(object))) }
}

fn set_prototype_of_through_hook(object: &Object, _vm: &Vm, prototype: Option<Gc<Object>>) -> ThrowCompletionOr<bool> {
    let hook = hooks_of(object)
        .set_prototype_of
        .expect("the class has a [[SetPrototypeOf]] hook");
    // SAFETY: The hook takes an object of its class and null or a live object.
    bool_completion_from_hook(unsafe { hook(lend_object_to_hook(object), optional_object_into_abi(prototype)) })
}

fn is_extensible_through_hook(object: &Object, _vm: &Vm) -> ThrowCompletionOr<bool> {
    let hook = hooks_of(object)
        .is_extensible
        .expect("the class has an [[IsExtensible]] hook");
    // SAFETY: The hook takes an object of its class.
    bool_completion_from_hook(unsafe { hook(lend_object_to_hook(object)) })
}

fn prevent_extensions_through_hook(object: &Object, _vm: &Vm) -> ThrowCompletionOr<bool> {
    let hook = hooks_of(object)
        .prevent_extensions
        .expect("the class has a [[PreventExtensions]] hook");
    // SAFETY: The hook takes an object of its class.
    bool_completion_from_hook(unsafe { hook(lend_object_to_hook(object)) })
}

fn get_own_property_through_hook(
    object: &Object,
    _vm: &Vm,
    key: &PropertyKey,
) -> ThrowCompletionOr<Option<PropertyDescriptor>> {
    let hook = hooks_of(object)
        .get_own_property
        .expect("the class has a [[GetOwnProperty]] hook");
    let mut descriptor = property_descriptor_to_abi(None);
    // SAFETY: The hook takes an object of its class, a key, and a zeroed descriptor to fill in.
    completion_without_result_from_hook(unsafe {
        hook(
            lend_object_to_hook(object),
            lend_property_key_to_abi(key),
            &raw mut descriptor,
        )
    })?;
    // SAFETY: The getter and setter of a descriptor that a hook fills in are null or live functions.
    Ok(unsafe { property_descriptor_from_hook(&descriptor) })
}

/// The descriptor that a [[GetOwnProperty]] hook filled in. A data property with all of its attributes, such as an
/// indexed or named property of a collection, skips the general conversion, since reads of such properties spend a
/// measurable share of their time in it.
///
/// # Safety
///
/// The getter and setter of the descriptor must be null or live functions.
unsafe fn property_descriptor_from_hook(descriptor: &JSPropertyDescriptor) -> Option<PropertyDescriptor> {
    const COMPLETE_DATA_DESCRIPTOR_FLAGS: u16 =
        JS_PD_PRESENT | JS_PD_HAS_VALUE | JS_PD_HAS_WRITABLE | JS_PD_HAS_ENUMERABLE | JS_PD_HAS_CONFIGURABLE;
    const ATTRIBUTE_FLAGS: u16 = JS_PD_WRITABLE | JS_PD_ENUMERABLE | JS_PD_CONFIGURABLE;
    if descriptor.flags & !ATTRIBUTE_FLAGS == COMPLETE_DATA_DESCRIPTOR_FLAGS {
        return Some(PropertyDescriptor {
            value: Some(Value(descriptor.value)),
            writable: Some(descriptor.flags & JS_PD_WRITABLE != 0),
            enumerable: Some(descriptor.flags & JS_PD_ENUMERABLE != 0),
            configurable: Some(descriptor.flags & JS_PD_CONFIGURABLE != 0),
            ..Default::default()
        });
    }
    // SAFETY: The caller guarantees that the getter and setter are null or live functions.
    unsafe { property_descriptor_from_abi(descriptor) }
}

fn define_own_property_through_hook(
    object: &Object,
    _vm: &Vm,
    key: &PropertyKey,
    descriptor: &mut PropertyDescriptor,
    precomputed_get_own_property: Option<&Option<PropertyDescriptor>>,
) -> ThrowCompletionOr<bool> {
    let hook = hooks_of(object)
        .define_own_property
        .expect("the class has a [[DefineOwnProperty]] hook");
    let mut abi_descriptor = property_descriptor_to_abi(Some(descriptor));
    let abi_precomputed_get_own_property =
        precomputed_get_own_property.map(|precomputed| property_descriptor_to_abi(precomputed.as_ref()));
    // SAFETY: The hook takes an object of its class, a key, a present descriptor it may update, and null or the
    //         result of a [[GetOwnProperty]] the caller already ran.
    let completion = unsafe {
        hook(
            lend_object_to_hook(object),
            lend_property_key_to_abi(key),
            &raw mut abi_descriptor,
            abi_precomputed_get_own_property
                .as_ref()
                .map_or(core::ptr::null(), core::ptr::from_ref),
        )
    };
    // SAFETY: The getter and setter of a descriptor that a hook writes back are null or live functions.
    if let Some(descriptor_written_back_by_hook) = unsafe { property_descriptor_from_abi(&abi_descriptor) } {
        *descriptor = descriptor_written_back_by_hook;
    }
    bool_completion_from_hook(completion)
}

fn has_property_through_hook(object: &Object, _vm: &Vm, key: &PropertyKey) -> ThrowCompletionOr<bool> {
    let hook = hooks_of(object)
        .has_property
        .expect("the class has a [[HasProperty]] hook");
    // SAFETY: The hook takes an object of its class and a key.
    bool_completion_from_hook(unsafe { hook(lend_object_to_hook(object), lend_property_key_to_abi(key)) })
}

fn get_through_hook(
    object: &Object,
    _vm: &Vm,
    key: &PropertyKey,
    receiver: Value,
    cacheable_metadata: Option<&mut CacheableGetPropertyMetadata>,
    phase: PropertyLookupPhase,
) -> ThrowCompletionOr<Value> {
    let hook = hooks_of(object).get.expect("the class has a [[Get]] hook");
    // SAFETY: The hook takes an object of its class, a key, and the caller's cache metadata and phase, which it
    //         passes on untouched, if at all, to the engine operation it delegates to.
    completion_from_abi(unsafe {
        hook(
            lend_object_to_hook(object),
            lend_property_key_to_abi(key),
            receiver.0,
            get_cache_metadata_into_abi(cacheable_metadata),
            lookup_phase_into_abi(phase),
        )
    })
}

fn set_through_hook(
    object: &Object,
    _vm: &Vm,
    key: &PropertyKey,
    value: Value,
    receiver: Value,
    cacheable_metadata: Option<&mut CacheableSetPropertyMetadata>,
    phase: PropertyLookupPhase,
) -> ThrowCompletionOr<bool> {
    let hook = hooks_of(object).set.expect("the class has a [[Set]] hook");
    // SAFETY: As for [[Get]].
    bool_completion_from_hook(unsafe {
        hook(
            lend_object_to_hook(object),
            lend_property_key_to_abi(key),
            value.0,
            receiver.0,
            set_cache_metadata_into_abi(cacheable_metadata),
            lookup_phase_into_abi(phase),
        )
    })
}

fn delete_through_hook(object: &Object, _vm: &Vm, key: &PropertyKey) -> ThrowCompletionOr<bool> {
    let hook = hooks_of(object)
        .delete_property
        .expect("the class has a [[Delete]] hook");
    // SAFETY: The hook takes an object of its class and a key.
    bool_completion_from_hook(unsafe { hook(lend_object_to_hook(object), lend_property_key_to_abi(key)) })
}

/// Appends a key that an [[OwnPropertyKeys]] hook hands its sink to the list that is the sink's context.
unsafe extern "C" fn append_key_to_list(keys: *mut c_void, key: JSValue) {
    // SAFETY: The sink's context is the rooted list of keys of the [[OwnPropertyKeys]] call that made the sink.
    let keys = unsafe { &*keys.cast::<MarkedVec<'_, Value>>() };
    keys.push(Value(key));
}

fn own_property_keys_through_hook<'vm>(object: &Object, vm: &'vm Vm) -> ThrowCompletionOr<MarkedVec<'vm, Value>> {
    let hook = hooks_of(object)
        .own_property_keys
        .expect("the class has an [[OwnPropertyKeys]] hook");
    let keys = MarkedVec::new(vm);
    let mut sink = JSValueSink {
        context: core::ptr::from_ref(&keys).cast_mut().cast(),
        append: Some(append_key_to_list),
    };
    // SAFETY: The hook takes an object of its class and a sink, whose list roots the keys appended to it.
    completion_without_result_from_hook(unsafe { hook(lend_object_to_hook(object), &raw mut sink) })?;
    Ok(keys)
}

fn is_cacheable_for_inherited_property_through_hook(object: &Object) -> bool {
    let hook = hooks_of(object)
        .is_cacheable_for_inherited_property
        .expect("the class has an is_cacheable_for_inherited_property hook");
    // SAFETY: The hook takes an object of its class.
    unsafe { hook(lend_object_to_hook(object)) }
}

fn error_data_through_hook(object: &Object) -> Option<&ErrorData> {
    let hook = hooks_of(object).error_data.expect("the class has an error_data hook");
    // SAFETY: The hook takes an object of its class, and returns null or error data that lives as long as the object,
    //         in a cell the object keeps alive.
    unsafe { error_data_from_host_hook(object, hook(lend_object_to_hook(object))) }
}

impl HostObject {
    /// HostObject::create(): a host object of the class `table` describes, with the realm's empty object shape
    /// transitioned to `prototype`, holding the embedder's `wrappable` and `host_data` cells.
    ///
    /// # Safety
    ///
    /// `table` must be of kind JS_HOST_CLASS_OBJECT, and `wrappable` and `host_data` must be absent or live cells of
    /// the VM's heap.
    pub unsafe fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        table: &'static JSHostClass,
        prototype: Option<Gc<Object>>,
        wrappable: Option<NonNull<c_void>>,
        host_data: Option<NonNull<c_void>>,
    ) -> Gc<HostObject> {
        let (class, allocator) = runtime_class_and_allocator_of_host_class(vm, table, JS_HOST_CLASS_OBJECT);
        let base = Object::new_with_realm_and_prototype(
            vm,
            class,
            realm,
            prototype,
            MayInterfereWithIndexedPropertyAccess::No,
        );
        copy_host_class_flags_into_object(table, &base);
        let host_object = HostObject {
            base,
            host_class: table,
            wrappable: ForeignCellSlot::empty(),
            host_data: ForeignCellSlot::empty(),
        };
        // SAFETY: The caller passes absent or live cells, which the slots then keep alive.
        unsafe {
            host_object.wrappable.set(wrappable);
            host_object.host_data.set(host_data);
        }
        let host_object = allocate_object_in(vm, allocator, host_object);
        host_object.initialize(vm, realm);
        host_object
    }
}

/// The host object an internal method of a host object class was called on.
pub(crate) fn as_host_object(object: &Object) -> &HostObject {
    debug_assert!(object.is::<HostObject>());
    // SAFETY: The object is a HostObject, which starts with its Object.
    unsafe { &*core::ptr::from_ref(object).cast::<HostObject>() }
}

/// host_class_of(): the host class of an object of any host kind, or none for any other object.
pub fn host_class_of(object: &Object) -> Option<&'static JSHostClass> {
    match object.class().id {
        ClassId::HostObject => Some(as_host_object(object).host_class),
        ClassId::HostFunction => Some(as_host_function(object).host_class),
        ClassId::HostArray => Some(as_host_array(object).host_class),
        _ => None,
    }
}

/// is_host_instance_of(): whether the object's host class is `table` or derives from it through JSHostClass::parent.
pub fn is_host_instance_of(object: &Object, table: &'static JSHostClass) -> bool {
    host_class_of(object).is_some_and(|host_class| host_class.is_or_derives_from(table))
}

/// The slot of an object of any host kind that holds the embedder's companion cell.
fn host_data_slot_of(object: &Object) -> Option<&ForeignCellSlot> {
    match object.class().id {
        ClassId::HostObject => Some(&as_host_object(object).host_data),
        ClassId::HostFunction => Some(&as_host_function(object).host_data),
        ClassId::HostArray => Some(&as_host_array(object).host_data),
        _ => None,
    }
}

/// host_data_of(): the companion cell of a host object of any kind, or none.
pub fn host_data_of(object: &Object) -> Option<NonNull<c_void>> {
    host_data_slot_of(object).and_then(ForeignCellSlot::get)
}

/// Replaces the companion cell of a host object of any kind.
///
/// # Safety
///
/// `host_data` must be absent or a live cell of the VM's heap.
pub unsafe fn set_host_data(object: &Object, host_data: Option<NonNull<c_void>>) {
    let slot = host_data_slot_of(object).expect("only host objects have host data");
    // SAFETY: The caller passes an absent or live cell, which the slot then keeps alive.
    unsafe { slot.set(host_data) };
}

// The embedding ABI of host objects, and of what every kind of host object shares

/// HostObject::create(): a host object of `host_class`, a table of kind JS_HOST_CLASS_OBJECT, made from the realm's
/// empty object shape with `prototype_or_null` as its [[Prototype]]. `wrappable_or_null` is the embedder's
/// implementation object, which direct getter functions read at JS_HOST_OBJECT_WRAPPABLE_OFFSET, and
/// `host_data_or_null` a cell with the rest of its per-object state; the object keeps both alive. Returns an unrooted
/// object. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_host_object_create(
    vm: *mut JSVM,
    realm: *mut JSRealm,
    host_class: *const JSHostClass,
    prototype_or_null: *mut JSObject,
    wrappable_or_null: *mut c_void,
    host_data_or_null: *mut c_void,
) -> *mut JSObject {
    // SAFETY: See the module documentation.
    unsafe {
        object_into_abi(HostObject::create(
            vm_from_abi(vm),
            cell_from_abi::<JSRealm>(realm),
            host_class_from_abi(host_class),
            optional_cell_from_abi::<JSObject>(prototype_or_null),
            NonNull::new(wrappable_or_null),
            NonNull::new(host_data_or_null),
        ))
    }
}

/// host_class_of(): the host class of an object of any host kind, or null for any other object. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_host_object_host_class_of(object: *mut JSObject) -> *const JSHostClass {
    // SAFETY: See the module documentation.
    let object = unsafe { cell_from_abi::<JSObject>(object) };
    host_class_of(&object).map_or(core::ptr::null(), host_class_into_abi)
}

/// is_host_instance_of(): whether the host class of the object is `host_class` or derives from it through
/// JSHostClass::parent, false for an object of no host kind. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_host_object_is_host_instance_of(
    object: *mut JSObject,
    host_class: *const JSHostClass,
) -> bool {
    // SAFETY: See the module documentation.
    let (object, host_class) = unsafe { (cell_from_abi::<JSObject>(object), host_class_from_abi(host_class)) };
    is_host_instance_of(&object, host_class)
}

/// host_data_of(): the companion cell of a host object of any kind, or null, also for an object of no host kind.
/// Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_host_object_host_data_of(object: *mut JSObject) -> *mut c_void {
    // SAFETY: See the module documentation.
    let object = unsafe { cell_from_abi::<JSObject>(object) };
    host_data_of(&object).map_or(core::ptr::null_mut(), NonNull::as_ptr)
}

/// set_host_data(): replaces the companion cell of a host object of any kind with `host_data_or_null`, which the object
/// then keeps alive. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_host_object_set_host_data(host_object: *mut JSObject, host_data_or_null: *mut c_void) {
    // SAFETY: See the module documentation.
    unsafe { set_host_data(&cell_from_abi::<JSObject>(host_object), NonNull::new(host_data_or_null)) };
}
