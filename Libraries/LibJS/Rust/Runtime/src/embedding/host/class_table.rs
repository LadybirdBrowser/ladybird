/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Reading and validating JSHostClass tables, and the completions their hooks return.
//!
//! A table is static constant data with exactly one definition, which lives as long as the process, so the runtime
//! keeps `&'static` references to tables and their hook structs, and identifies a class by the address of its table.

use crate::embedding::abi_types::{completion_from_abi, optional_cell_from_abi};
use crate::layout::cell::Gc;
use crate::layout::host_class::{
    JS_HOST_ABI_VERSION, JS_HOST_CLASS_ARRAY, JS_HOST_CLASS_FUNCTION, JS_HOST_CLASS_HAS_CONSTRUCTOR,
    JS_HOST_CLASS_IMMUTABLE_PROTOTYPE, JS_HOST_CLASS_IS_GLOBAL_OBJECT, JS_HOST_CLASS_IS_HTMLDDA,
    JS_HOST_CLASS_IS_PLATFORM_OBJECT, JS_HOST_CLASS_MAY_INTERFERE_WITH_INDEXED_PROPERTY_ACCESS, JS_HOST_CLASS_MODULE,
    JS_HOST_CLASS_NOT_CACHEABLE_FOR_PROPERTY_ABSENCE,
    JS_HOST_CLASS_NOT_ELIGIBLE_FOR_OWN_PROPERTY_ENUMERATION_FAST_PATH, JS_HOST_CLASS_OBJECT,
    JS_HOST_CLASS_REQUIRES_SLOW_ADD_OWN_PROPERTY, JS_HOST_CLASS_SHARES_ALLOCATOR_WITH_PARENT,
    JS_PROPERTY_LOOKUP_PHASE_OWN_PROPERTY, JS_PROPERTY_LOOKUP_PHASE_PROTOTYPE_CHAIN, JSCompletion, JSGetCacheMetadata,
    JSHostArrayHooks, JSHostClass, JSHostFunctionHooks, JSHostObjectHooks, JSObject, JSSetCacheMetadata,
};
use crate::layout::object::Object;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::object::{CacheableGetPropertyMetadata, CacheableSetPropertyMetadata, PropertyLookupPhase};

/// The engine flags that every kind of host object copies into the objects of its class.
const FLAGS_COPIED_INTO_OBJECTS: u32 = JS_HOST_CLASS_IS_PLATFORM_OBJECT
    | JS_HOST_CLASS_REQUIRES_SLOW_ADD_OWN_PROPERTY
    | JS_HOST_CLASS_MAY_INTERFERE_WITH_INDEXED_PROPERTY_ACCESS
    | JS_HOST_CLASS_IS_HTMLDDA
    | JS_HOST_CLASS_IS_GLOBAL_OBJECT;

const FLAGS_OF_HOST_OBJECTS_ONLY: u32 = JS_HOST_CLASS_IMMUTABLE_PROTOTYPE
    | JS_HOST_CLASS_NOT_CACHEABLE_FOR_PROPERTY_ABSENCE
    | JS_HOST_CLASS_NOT_ELIGIBLE_FOR_OWN_PROPERTY_ENUMERATION_FAST_PATH;

static HOST_OBJECT_HOOKS_THAT_ARE_ALL_ORDINARY: JSHostObjectHooks = JSHostObjectHooks {
    get_prototype_of: None,
    set_prototype_of: None,
    is_extensible: None,
    prevent_extensions: None,
    get_own_property: None,
    define_own_property: None,
    has_property: None,
    get: None,
    set: None,
    delete_property: None,
    own_property_keys: None,
    is_cacheable_for_inherited_property: None,
    error_data: None,
    finalize: None,
};

static HOST_ARRAY_HOOKS_THAT_ARE_ALL_ORDINARY: JSHostArrayHooks = JSHostArrayHooks {
    set: None,
    delete_property: None,
};

/// # Safety
///
/// `table` must point to a JSHostClass that lives as long as the process, as LibJS/HostObjectABI.h requires of every
/// table, along with its name, parent and hooks.
pub unsafe fn host_class_from_abi(table: *const JSHostClass) -> &'static JSHostClass {
    // SAFETY: The caller passes a table that lives as long as the process.
    unsafe { table.as_ref() }.expect("the embedder passes a host class")
}

pub fn host_class_into_abi(table: &'static JSHostClass) -> *const JSHostClass {
    core::ptr::from_ref(table)
}

impl JSHostClass {
    /// The address that identifies the class.
    pub fn identity(&'static self) -> usize {
        core::ptr::from_ref(self).addr()
    }

    pub fn has_flag(&self, flag: u32) -> bool {
        self.flags & flag != 0
    }

    pub fn class_name(&'static self) -> &'static str {
        if self.name_length == 0 {
            return "";
        }
        assert!(!self.name.is_null(), "a host class with a name points to it");
        // SAFETY: The name of a table is static data of the given length, like the table itself.
        let name = unsafe { core::slice::from_raw_parts(self.name.cast::<u8>(), self.name_length) };
        core::str::from_utf8(name).expect("the name of a host class is UTF-8")
    }

    pub fn parent_class(&'static self) -> Option<&'static JSHostClass> {
        // SAFETY: The parent of a table is a table, which lives as long as the process.
        unsafe { self.parent.as_ref() }
    }

    /// Whether the class is `ancestor` or derives from it through JSHostClass::parent, as is_host_instance_of() asks.
    pub fn is_or_derives_from(&'static self, ancestor: &'static JSHostClass) -> bool {
        let mut class = Some(self);
        while let Some(current) = class {
            if core::ptr::eq(current, ancestor) {
                return true;
            }
            class = current.parent_class();
        }
        false
    }

    /// The class whose allocator the cells of this class come from: the nearest one, starting with this class, that
    /// does not share its parent's.
    pub fn allocating_class(&'static self) -> &'static JSHostClass {
        let mut class = self;
        while class.has_flag(JS_HOST_CLASS_SHARES_ALLOCATOR_WITH_PARENT) {
            class = class
                .parent_class()
                .filter(|parent| parent.kind == class.kind)
                .expect("a host class that shares its parent's allocator has a parent of its kind");
        }
        class
    }

    /// The hooks of a class of kind JS_HOST_CLASS_OBJECT. A table without a hook struct has only ordinary ones.
    pub fn host_object_hooks(&'static self) -> &'static JSHostObjectHooks {
        debug_assert!(self.kind == JS_HOST_CLASS_OBJECT);
        // SAFETY: The hooks of a table of this kind are a JSHostObjectHooks, which lives as long as the table.
        unsafe { self.hooks.cast::<JSHostObjectHooks>().as_ref() }.unwrap_or(&HOST_OBJECT_HOOKS_THAT_ARE_ALL_ORDINARY)
    }

    /// The hooks of a class of kind JS_HOST_CLASS_FUNCTION, which always has them.
    pub fn host_function_hooks(&'static self) -> &'static JSHostFunctionHooks {
        debug_assert!(self.kind == JS_HOST_CLASS_FUNCTION);
        // SAFETY: The hooks of a table of this kind are a JSHostFunctionHooks, which lives as long as the table.
        unsafe { self.hooks.cast::<JSHostFunctionHooks>().as_ref() }.expect("a host function class has hooks")
    }

    /// The hooks of a class of kind JS_HOST_CLASS_ARRAY. A table without a hook struct has only those of an Array.
    pub fn host_array_hooks(&'static self) -> &'static JSHostArrayHooks {
        debug_assert!(self.kind == JS_HOST_CLASS_ARRAY);
        // SAFETY: The hooks of a table of this kind are a JSHostArrayHooks, which lives as long as the table.
        unsafe { self.hooks.cast::<JSHostArrayHooks>().as_ref() }.unwrap_or(&HOST_ARRAY_HOOKS_THAT_ARE_ALL_ORDINARY)
    }

    /// Checks what LibJS/HostObjectABI.h requires of a table of `kind`, which the engine then relies on: its version
    /// and kind, flags that apply to the kind, a parent of the same kind to share an allocator with, and the hooks the
    /// kind cannot do without.
    pub fn validate(&'static self, kind: u8) {
        assert!(
            self.abi_version == JS_HOST_ABI_VERSION,
            "the host class {} is of ABI version {}, not {JS_HOST_ABI_VERSION}",
            self.class_name(),
            self.abi_version
        );
        assert!(
            self.kind == kind,
            "the host class {} is of kind {}, not {kind}",
            self.class_name(),
            self.kind
        );
        let flags_of_the_kind = match kind {
            JS_HOST_CLASS_OBJECT => FLAGS_COPIED_INTO_OBJECTS | FLAGS_OF_HOST_OBJECTS_ONLY,
            JS_HOST_CLASS_FUNCTION => FLAGS_COPIED_INTO_OBJECTS | JS_HOST_CLASS_HAS_CONSTRUCTOR,
            JS_HOST_CLASS_ARRAY => FLAGS_COPIED_INTO_OBJECTS,
            JS_HOST_CLASS_MODULE => 0,
            kind => panic!("{kind} is not a kind of host class"),
        };
        assert!(
            self.flags & !(flags_of_the_kind | JS_HOST_CLASS_SHARES_ALLOCATOR_WITH_PARENT) == 0,
            "the host class {} has flags {:#x} that its kind does not have",
            self.class_name(),
            self.flags
        );
        self.allocating_class();
        if kind == JS_HOST_CLASS_FUNCTION {
            let hooks = self.host_function_hooks();
            assert!(hooks.call.is_some(), "a host function class has a call hook");
            assert!(
                !self.has_flag(JS_HOST_CLASS_HAS_CONSTRUCTOR) || hooks.construct.is_some(),
                "a host function class with a constructor has a construct hook"
            );
        }
    }
}

/// Gives a host object the flags of its class that the engine keeps in every object.
pub fn copy_host_class_flags_into_object(table: &JSHostClass, object: &Object) {
    if table.has_flag(JS_HOST_CLASS_IS_PLATFORM_OBJECT) {
        object.set_is_platform_object();
    }
    if table.has_flag(JS_HOST_CLASS_REQUIRES_SLOW_ADD_OWN_PROPERTY) {
        object.set_requires_slow_add_own_property();
    }
    if table.has_flag(JS_HOST_CLASS_MAY_INTERFERE_WITH_INDEXED_PROPERTY_ACCESS) {
        object.set_may_interfere_with_indexed_property_access();
    }
    if table.has_flag(JS_HOST_CLASS_IS_HTMLDDA) {
        object.set_is_htmldda();
    }
    if table.has_flag(JS_HOST_CLASS_IS_GLOBAL_OBJECT) {
        object.set_global_object_flag();
    }
}

/// The inline cache metadata of a [[Get]], which a hook receives untouched, to pass on to the engine.
pub fn get_cache_metadata_into_abi(metadata: Option<&mut CacheableGetPropertyMetadata>) -> *mut JSGetCacheMetadata {
    metadata.map_or(core::ptr::null_mut(), |metadata| core::ptr::from_mut(metadata).cast())
}

/// The inline cache metadata of a [[Set]], which a hook receives untouched, to pass on to the engine.
pub fn set_cache_metadata_into_abi(metadata: Option<&mut CacheableSetPropertyMetadata>) -> *mut JSSetCacheMetadata {
    metadata.map_or(core::ptr::null_mut(), |metadata| core::ptr::from_mut(metadata).cast())
}

pub fn lookup_phase_into_abi(phase: PropertyLookupPhase) -> u8 {
    match phase {
        PropertyLookupPhase::OwnProperty => JS_PROPERTY_LOOKUP_PHASE_OWN_PROPERTY,
        PropertyLookupPhase::PrototypeChain => JS_PROPERTY_LOOKUP_PHASE_PROTOTYPE_CHAIN,
    }
}

/// The object a hook receives, which the runtime lends it for the duration of the call.
pub fn lend_object_to_hook(object: &Object) -> *mut JSObject {
    core::ptr::from_ref(object).cast_mut().cast()
}

/// The completion of a hook whose normal completion has no result.
pub fn completion_without_result_from_hook(completion: JSCompletion) -> ThrowCompletionOr<()> {
    completion_from_abi(completion).map(|_| ())
}

/// The completion of a hook whose normal completion carries a bool, as 0 or 1.
pub fn bool_completion_from_hook(completion: JSCompletion) -> ThrowCompletionOr<bool> {
    completion_from_abi(completion).map(|payload| payload.0 != 0)
}

/// The completion of a hook whose normal completion carries an object or null.
///
/// # Safety
///
/// A normal completion must carry null or the address of a live object.
pub unsafe fn optional_object_completion_from_hook(completion: JSCompletion) -> ThrowCompletionOr<Option<Gc<Object>>> {
    completion_from_abi(completion).map(|payload| {
        let object = core::ptr::with_exposed_provenance_mut::<JSObject>(payload.0 as usize);
        // SAFETY: The caller guarantees that the payload is null or the address of a live object.
        unsafe { optional_cell_from_abi(object) }
    })
}

/// The completion of a hook whose normal completion carries an object.
///
/// # Safety
///
/// A normal completion must carry the address of a live object.
pub unsafe fn object_completion_from_hook(completion: JSCompletion) -> ThrowCompletionOr<Gc<Object>> {
    // SAFETY: The caller guarantees that the payload is the address of a live object.
    unsafe { optional_object_completion_from_hook(completion) }
        .map(|object| object.expect("the hook completes with an object"))
}
