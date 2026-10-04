/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The tables through which an embedder defines objects whose internal methods it implements itself. They are plain C
// with a fixed layout so that an engine other than LibJS's C++ one could read the same structures, which makes the
// layout part of the ABI.
//
// An embedder describes each class of host object with a JSHostClass and a hook table of the kind's struct. Both are
// static constant data with exactly one definition, because the engine identifies a class by the address of its
// JSHostClass. A null hook means the engine's ordinary behavior for that internal method. Tables are flattened: a null
// hook never falls back to a hook of the parent class, which only matters to is_host_instance_of().
//
// Hooks run on the thread that owns the engine and take no VM argument except where JavaScript calls need one, as a
// process has one VM. Engine objects and values cross as raw pointers and NaN-boxed values, which stay alive for the
// duration of a hook through conservative stack scanning, like any engine value on the native stack.

#ifdef __cplusplus
extern "C" {
#endif

enum {
    JS_HOST_ABI_VERSION = 1,
};

// Layout of every host object of kind JS_HOST_CLASS_OBJECT. Direct getter functions read the wrappable slot without
// checking what it holds, so it only ever holds the embedder's wrapped implementation object. Any other per-object state
// belongs in the host data slot.
enum {
    JS_HOST_OBJECT_HOST_CLASS_OFFSET = 72,
    JS_HOST_OBJECT_WRAPPABLE_OFFSET = 80,
    JS_HOST_OBJECT_HOST_DATA_OFFSET = 88,
    JS_HOST_OBJECT_SIZE = 96,
};

typedef uint64_t JSValue;

// A borrowed property key. Its bits are those of the engine's own property key, which the hook must not keep after it
// returns.
typedef struct JSPropertyKey {
    uintptr_t bits;
} JSPropertyKey;

enum {
    JS_COMPLETION_NORMAL = 0,
    JS_COMPLETION_THROW = 1,
};

// A normal completion carries its result in the payload: a JSValue, a bool as 0 or 1, or a pointer. A throw completion
// carries the thrown JSValue.
typedef struct JSCompletion {
    uint64_t payload;
    uint8_t variant;
} JSCompletion;

typedef struct JSObject JSObject;
typedef struct JSVM JSVM;
typedef struct JSModule JSModule;
typedef struct JSPromiseCapability JSPromiseCapability;

// Opaque inline cache records that a [[Get]] or [[Set]] hook passes on, untouched, to the engine operation it delegates
// to. Either pointer may be null.
typedef struct JSGetCacheMetadata JSGetCacheMetadata;
typedef struct JSSetCacheMetadata JSSetCacheMetadata;

enum {
    JS_PROPERTY_LOOKUP_PHASE_OWN_PROPERTY = 0,
    JS_PROPERTY_LOOKUP_PHASE_PROTOTYPE_CHAIN = 1,
};

enum {
    JS_PD_HAS_VALUE = 1 << 0,
    JS_PD_HAS_GET = 1 << 1,
    JS_PD_HAS_SET = 1 << 2,
    JS_PD_HAS_WRITABLE = 1 << 3,
    JS_PD_WRITABLE = 1 << 4,
    JS_PD_HAS_ENUMERABLE = 1 << 5,
    JS_PD_ENUMERABLE = 1 << 6,
    JS_PD_HAS_CONFIGURABLE = 1 << 7,
    JS_PD_CONFIGURABLE = 1 << 8,
    // The property's slot in the object's storage. Inline caches are filled from it, so a hook that delegates to the
    // engine must hand it back unchanged.
    JS_PD_HAS_PROPERTY_OFFSET = 1 << 9,
    // Whether there is a descriptor at all, where the engine passes an optional one.
    JS_PD_PRESENT = 1 << 10,
};

// A property descriptor. get and set are meaningful only with JS_PD_HAS_GET and JS_PD_HAS_SET, and null stands for an
// undefined getter or setter.
typedef struct JSPropertyDescriptor {
    JSValue value;
    JSObject* get;
    JSObject* set;
    uint32_t property_offset;
    uint16_t flags;
} JSPropertyDescriptor;

typedef struct JSValueSink {
    void* context;
    void (*append)(void* context, JSValue);
} JSValueSink;

typedef struct JSStringSink {
    void* context;
    void (*append)(void* context, uint16_t const* code_units, size_t length_in_code_units);
} JSStringSink;

enum {
    JS_HOST_CLASS_OBJECT = 1,
    JS_HOST_CLASS_FUNCTION = 2,
    JS_HOST_CLASS_ARRAY = 3,
    JS_HOST_CLASS_MODULE = 4,
};

// Engine flags of a host class.
enum {
    // For every kind but JS_HOST_CLASS_MODULE, the engine copies these into each object it creates.
    JS_HOST_CLASS_IS_PLATFORM_OBJECT = 1 << 0,
    JS_HOST_CLASS_REQUIRES_SLOW_ADD_OWN_PROPERTY = 1 << 1,
    JS_HOST_CLASS_MAY_INTERFERE_WITH_INDEXED_PROPERTY_ACCESS = 1 << 2,
    JS_HOST_CLASS_IS_HTMLDDA = 1 << 3,
    JS_HOST_CLASS_IS_GLOBAL_OBJECT = 1 << 4,

    // Only for JS_HOST_CLASS_OBJECT.
    //
    // JS_HOST_CLASS_IMMUTABLE_PROTOTYPE makes the ordinary [[SetPrototypeOf]] that of an immutable prototype exotic
    // object.
    //
    // Inline caches remember that a key is missing from an object, or found further up its prototype chain, and answer
    // from that without calling hooks while the shapes involved stay the same. Whether the engine may do so depends only
    // on JS_HOST_CLASS_NOT_CACHEABLE_FOR_PROPERTY_ABSENCE and the is_cacheable_for_inherited_property hook, never on
    // which other hooks a class has. A class whose hooks can answer for keys missing from its shape must set the flag,
    // and one whose hooks can start shadowing inherited properties must have that hook return false.
    //
    // Enumeration (for-in, Object.keys, Object.assign, object spread, JSON.stringify) has fast paths that read keys,
    // attributes and values straight from the shape and storage. The engine never takes them for an object whose class
    // has a get_prototype_of, get_own_property, has_property, get or own_property_keys hook, nor for one whose class sets
    // JS_HOST_CLASS_NOT_ELIGIBLE_FOR_OWN_PROPERTY_ENUMERATION_FAST_PATH.
    JS_HOST_CLASS_IMMUTABLE_PROTOTYPE = 1 << 5,
    JS_HOST_CLASS_NOT_CACHEABLE_FOR_PROPERTY_ABSENCE = 1 << 6,
    JS_HOST_CLASS_NOT_ELIGIBLE_FOR_OWN_PROPERTY_ENUMERATION_FAST_PATH = 1 << 7,

    // Only for JS_HOST_CLASS_FUNCTION, which must then have a construct hook.
    JS_HOST_CLASS_HAS_CONSTRUCTOR = 1 << 8,

    // For every kind. Each host class otherwise gets a cell allocator of its own, so that objects of different classes
    // never share heap blocks. A class with this flag allocates its objects from its parent's allocator instead, which
    // suits many classes with few objects each. The parent must be of the same kind.
    JS_HOST_CLASS_SHARES_ALLOCATOR_WITH_PARENT = 1 << 9,
};

// The essential internal methods of kind JS_HOST_CLASS_OBJECT, plus engine queries. A completion's payload is the
// result of the internal method, or unused for get_own_property and own_property_keys, which write to their out
// parameter.
//
// The engine passes get_own_property a zeroed descriptor. For a present property the hook sets JS_PD_PRESENT and the
// JS_PD_HAS_* bits of the fields it fills in, and for an absent one it leaves the descriptor zeroed.
//
// define_own_property receives a descriptor with JS_PD_PRESENT set and may update it in place, as the engine reports a
// new property's offset through it. The engine reads it back only while JS_PD_PRESENT is still set.
// precomputed_get_own_property is null unless the caller has already run [[GetOwnProperty]], whose result it then
// holds, with JS_PD_PRESENT clear for an absent property.
typedef struct JSHostObjectHooks {
    JSCompletion (*get_prototype_of)(JSObject*);
    JSCompletion (*set_prototype_of)(JSObject*, JSObject* prototype);
    JSCompletion (*is_extensible)(JSObject*);
    JSCompletion (*prevent_extensions)(JSObject*);
    JSCompletion (*get_own_property)(JSObject*, JSPropertyKey, JSPropertyDescriptor* out);
    JSCompletion (*define_own_property)(JSObject*, JSPropertyKey, JSPropertyDescriptor* descriptor, JSPropertyDescriptor const* precomputed_get_own_property);
    JSCompletion (*has_property)(JSObject*, JSPropertyKey);
    JSCompletion (*get)(JSObject*, JSPropertyKey, JSValue receiver, JSGetCacheMetadata*, uint8_t phase);
    JSCompletion (*set)(JSObject*, JSPropertyKey, JSValue value, JSValue receiver, JSSetCacheMetadata*, uint8_t phase);
    JSCompletion (*delete_property)(JSObject*, JSPropertyKey);
    JSCompletion (*own_property_keys)(JSObject*, JSValueSink* keys);
    bool (*is_cacheable_for_inherited_property)(JSObject*);
    void* (*error_data)(JSObject*);
    // Runs during garbage collection and must not allocate.
    void (*finalize)(JSObject*);
} JSHostObjectHooks;

// The [[Call]] and [[Construct]] behavior of kind JS_HOST_CLASS_FUNCTION, which the engine runs in the function's own
// execution context, so arguments and the this value come from the VM. call is required.
typedef struct JSHostFunctionHooks {
    JSCompletion (*call)(JSObject* function, JSVM*);
    JSCompletion (*construct)(JSObject* function, JSVM*, JSObject* new_target);
    void (*finalize)(JSObject* function);
} JSHostFunctionHooks;

// The internal methods that kind JS_HOST_CLASS_ARRAY may replace. The rest are those of an Array exotic object.
typedef struct JSHostArrayHooks {
    JSCompletion (*set)(JSObject*, JSPropertyKey, JSValue value, JSValue receiver, JSSetCacheMetadata*, uint8_t phase);
    JSCompletion (*delete_property)(JSObject*, JSPropertyKey);
} JSHostArrayHooks;

enum {
    JS_RESOLVED_BINDING_BINDING_NAME = 0,
    JS_RESOLVED_BINDING_NAMESPACE = 1,
    JS_RESOLVED_BINDING_AMBIGUOUS = 2,
    JS_RESOLVED_BINDING_NULL = 3,
};

// The engine sets type to JS_RESOLVED_BINDING_NULL and provides the binding_name sink. A hook resolving to a binding
// sets type and module and appends the binding name once.
typedef struct JSResolvedBinding {
    JSModule* module;
    JSStringSink binding_name;
    uint8_t type;
} JSResolvedBinding;

// The abstract methods of a Cyclic Module Record that kind JS_HOST_CLASS_MODULE implements; all four are required. A host
// module has no star exports, so the engine passes no export star set or resolve set.
typedef struct JSHostModuleHooks {
    void (*get_exported_names)(JSModule*, JSStringSink* names);
    void (*resolve_export)(JSModule*, uint16_t const* export_name, size_t export_name_length, JSResolvedBinding* out);
    JSCompletion (*initialize_environment)(JSModule*);
    JSCompletion (*execute_module)(JSModule*, JSPromiseCapability* capability_or_null);
} JSHostModuleHooks;

typedef struct JSHostClass {
    uint16_t abi_version;
    uint8_t kind;
    uint8_t reserved;
    uint32_t flags;
    char const* name;
    size_t name_length;
    struct JSHostClass const* parent;
    // The hook struct of the class's kind.
    void const* hooks;
    // Embedder data that the engine never reads.
    void const* user_data;
} JSHostClass;

#ifdef __cplusplus
}
#endif
