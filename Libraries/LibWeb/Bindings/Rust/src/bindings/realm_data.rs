/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the interfaces implemented in Rust keep for a realm: the objects that a specification has a global object
//! hold, which every object of an interface in the realm of that global returns.

use core::cell::Cell;

use libjs_rust::api::{EmbedderCell, EmbedderClass, EmbedderValue, FunctionObject, Gc, Realm, Trace, Vm};

#[derive(Default, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct RealmData {
    /// https://streams.spec.whatwg.org/#count-queuing-strategy-size-function
    pub(crate) count_queuing_strategy_size_function: Cell<Option<Gc<FunctionObject>>>,
    /// https://streams.spec.whatwg.org/#byte-length-queuing-strategy-size-function
    pub(crate) byte_length_queuing_strategy_size_function: Cell<Option<Gc<FunctionObject>>>,
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
    let data = REALM_DATA_CLASS.allocate(vm, RealmData::default());
    realm.set_embedder_data(data);
    data
}
