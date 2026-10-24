/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the interfaces implemented in Rust keep for a realm: the objects that a specification has a global object
//! hold, which every object of an interface in the realm of that global returns, and what the bindings ask LibWeb
//! about the realm once.

use core::cell::Cell;
use core::ffi::c_void;
use core::ptr::NonNull;

use libjs_rust::api::{EmbedderCell, EmbedderClass, EmbedderValue, FunctionObject, Gc, Object, Realm, Trace, Vm};

use crate::bindings::interface::Ref;

use crate::bindings::services::web_bindings_wrapper_world;
use crate::web_assembly::tag::Tag;

/// The wrapper world of a realm, Web::Bindings::WrapperWorld, which the realm keeps alive.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) struct WrapperWorld {
    world: NonNull<c_void>,
    is_main_world: bool,
}

impl WrapperWorld {
    pub(crate) fn is_main_world(self) -> bool {
        self.is_main_world
    }
}

#[derive(Default, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct RealmData {
    #[gc(untraced)]
    wrapper_world: Cell<Option<WrapperWorld>>,
    /// https://streams.spec.whatwg.org/#count-queuing-strategy-size-function
    pub(crate) count_queuing_strategy_size_function: Cell<Option<Gc<FunctionObject>>>,
    /// https://streams.spec.whatwg.org/#byte-length-queuing-strategy-size-function
    pub(crate) byte_length_queuing_strategy_size_function: Cell<Option<Gc<FunctionObject>>>,
    /// The NativeError prototypes and constructors of WebAssembly.CompileError, LinkError and RuntimeError,
    /// https://webassembly.github.io/spec/js-api/#error-objects
    pub(crate) webassembly_error_prototypes: [Cell<Option<Gc<Object>>>; 3],
    pub(crate) webassembly_error_constructors: [Cell<Option<Gc<Object>>>; 3],
    /// The Tag object of the JavaScript exception tag, https://webassembly.github.io/spec/js-api/#javascript-exception-tag
    pub(crate) webassembly_js_tag: Cell<Option<Ref<Tag>>>,
}

static REALM_DATA_CLASS: EmbedderClass<RealmData> = EmbedderClass::new("RealmData");

impl EmbedderValue for RealmData {
    const CLASS: &'static EmbedderClass<Self> = &REALM_DATA_CLASS;
}

/// The data of the realm, which the first use in a realm creates.
pub(crate) fn realm_data(vm: &Vm, realm: Gc<Realm>) -> Gc<EmbedderCell<RealmData>> {
    if let Some(data) = realm.embedder_data::<RealmData>() {
        return data;
    }
    let data = REALM_DATA_CLASS.allocate(vm, |_| RealmData::default());
    realm.set_embedder_data(data);
    data
}

/// The wrapper world of the realm, which LibWeb is asked for once per realm.
pub(crate) fn wrapper_world_of(vm: &Vm, realm: Gc<Realm>) -> WrapperWorld {
    let data = realm_data(vm, realm);
    if let Some(wrapper_world) = data.wrapper_world.get() {
        return wrapper_world;
    }
    let mut is_main_world = false;
    // SAFETY: The realm is live, and LibWeb set its host defined data up before any binding runs in it.
    let world = unsafe { web_bindings_wrapper_world(realm.as_ptr().cast(), &raw mut is_main_world) };
    let wrapper_world = WrapperWorld {
        world: NonNull::new(world).expect("a realm of LibWeb has a wrapper world"),
        is_main_world,
    };
    data.wrapper_world.set(Some(wrapper_world));
    wrapper_world
}
