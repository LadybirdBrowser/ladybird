/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Tag, https://webassembly.github.io/spec/js-api/#tags

use libjs_rust::api::{Gc, Realm, Trace, Value, Vm};
use libwasm_engine::runtime::{TagAddress, TagInstance};
use libwasm_engine::types::FuncType;

use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::realm_data::realm_data;
use crate::bindings::{CallContext, WebIdlResult};
use crate::web_assembly::global::ValueType;
use crate::web_assembly::{Agent, EntityRef, host_object, implementation_of_value, set_host_object};

/// https://webassembly.github.io/spec/js-api/#dictdef-tagtype
pub(crate) struct TagType {
    pub(crate) parameters: Vec<ValueType>,
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Tag {
    // [[Address]]
    address: EntityRef<TagInstance>,
}

impl Wrappable for Tag {}

impl Tag {
    // https://webassembly.github.io/spec/js-api/#dom-tag-tag
    pub(crate) fn construct(cx: &mut CallContext, r#type: TagType) -> WebIdlResult<Ref<Self>> {
        // 1. Let parameters be type["parameters"].
        // 2. Let wasmParameters be «».
        // 3. For each type of parameters,
        //     1. Append ToValueType(type) to wasmParameters.
        let parameters = r#type
            .parameters
            .iter()
            .map(|parameter| parameter.to_value_type())
            .collect();

        // 4. Let store be the surrounding agent's associated store.
        // 5. Let (store, tagAddress) be tag_alloc(store, « wasmParameters » → « »).
        // 6. Set the surrounding agent's associated store to store.
        let tag = Agent::the(cx.vm()).store().allocate_tag(FuncType {
            params: parameters,
            results: Vec::new(),
        });

        // 7. Initialize this from tagAddress.
        Ok(Self::initialize(cx.vm(), tag))
    }

    // https://webassembly.github.io/spec/js-api/#initialize-a-tag-object
    fn initialize(vm: &Vm, tag: TagAddress) -> Ref<Self> {
        // 1. Let map be the surrounding agent's associated tag object cache.
        // 3. Set tag.[[Address]] to tagAddress.
        let object = allocate(vm, |_| Self {
            address: EntityRef(tag),
        });

        // 4. Set map[tagAddress] to tag.
        set_host_object(tag.cell(), object);
        object
    }

    /// The Tag object of the tag in the realm, https://webassembly.github.io/spec/js-api/#create-a-tag-object
    pub(crate) fn from_address(vm: &Vm, realm: Gc<Realm>, tag: TagAddress) -> Ref<Self> {
        // NB: The JavaScript exception tag lives as long as the agent, while a Tag object belongs to the realm it is
        //     first wrapped in, so every realm has a Tag object of its own for it.
        if tag == Agent::the(vm).js_tag() {
            let data = realm_data(vm, realm);
            if let Some(object) = data.webassembly_js_tag.get() {
                return object;
            }
            let object = allocate(vm, |_| Self {
                address: EntityRef(tag),
            });
            data.webassembly_js_tag.set(Some(object));
            return object;
        }

        // 1. Let map be the surrounding agent's associated tag object cache.
        // 2. If map[tagAddress] exists,
        //     1. Return map[tagAddress].
        if let Some(object) = host_object(tag.cell()) {
            return object;
        }

        // 3. Let tag be a new Tag.
        // 4. Initialize tag from tagAddress.
        // 5. Return tag.
        Self::initialize(vm, tag)
    }

    pub(crate) fn from_value(value: Value) -> Option<Ref<Self>> {
        implementation_of_value(value)
    }

    pub(crate) fn address(&self) -> TagAddress {
        self.address.0
    }
}
