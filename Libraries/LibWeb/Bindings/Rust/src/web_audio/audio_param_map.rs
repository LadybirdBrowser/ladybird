/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::Utf16String;
use libjs_rust::api::{Gc, GcRefCell, Map, Trace, Value, Vm};

use super::audio_param::AudioParam;
use crate::bindings::CallContext;
use crate::bindings::conversions::{ToJs, to_dom_string};
use crate::bindings::interface::{Ref, SecurityCheckType, Wrappable, allocate, this_implementation, wrap};
use crate::bindings::maplike::Maplike;

// https://webaudio.github.io/web-audio-api/#AudioParamMap
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct AudioParamMap {
    entries: GcRefCell<Vec<(Utf16String, Ref<AudioParam>)>>,
}

impl Wrappable for AudioParamMap {}
impl AudioParamMap {
    pub(crate) fn values<'vm>(&self, vm: &'vm Vm) -> libjs_rust::api::MarkedVec<'vm, Ref<AudioParam>> {
        let values = libjs_rust::api::MarkedVec::new(vm);
        for (_, value) in &*self.entries.borrow() {
            values.push(*value);
        }
        values
    }

    pub(crate) fn create(vm: &Vm) -> Ref<Self> {
        allocate(vm, |_| Self {
            entries: GcRefCell::new(Vec::new()),
        })
    }

    pub(crate) fn set_entry(&self, vm: &Vm, key: Utf16String, value: Ref<AudioParam>) {
        let mut entries = self.entries.borrow_mut(vm);
        if let Some(entry) = entries.iter_mut().find(|entry| entry.0 == key) {
            entry.1 = value;
        } else {
            entries.push((key, value));
        }
    }
}

impl Maplike for AudioParamMap {
    // https://webidl.spec.whatwg.org/#js-map-has
    fn has(vm: &Vm) -> libjs_rust::api::ThrowCompletionOr<Value> {
        // 1. Let O be the this value, implementation-checked against A with identifier "has" and type "method".
        let (_, implementation, _cx) = this_implementation::<Self>(vm, "has", SecurityCheckType::Method)?;

        // 2. Let map be the map entries of the IDL value that represents a reference to O.
        let map = &implementation.entries;

        // 3. Let keyType be the key type specified in the maplike declaration.
        // 4. Let key be keyArg converted to an IDL value of type keyType.
        let key = to_dom_string(vm, vm.argument(0))?;

        // 5. If key is -0, set key to +0.
        // NB: DOMString conversion cannot produce a numeric -0 key.
        // 6. If map[key] exists, then return true; otherwise return false.
        Ok(Value::from_bool(map.borrow().iter().any(|entry| entry.0 == key)))
    }

    fn map_entries(&self, cx: &CallContext) -> Gc<Map> {
        let map_entries = Map::create(cx.vm(), cx.realm());
        for (key, value) in &*self.entries.borrow() {
            let key = key.clone().to_js(cx).expect("a string converts to a JS value");
            let value = Value::from_object(wrap(cx, *value));
            map_entries.map_set(cx.vm(), key, value);
        }
        map_entries
    }

    fn map_value(&self, cx: &CallContext, key: &Utf16String) -> Option<Value> {
        let entries = self.entries.borrow();
        let entry = entries.iter().find(|entry| &entry.0 == key)?;
        Some(Value::from_object(wrap(cx, entry.1)))
    }
}
