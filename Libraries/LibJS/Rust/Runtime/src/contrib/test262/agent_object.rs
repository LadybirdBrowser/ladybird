/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::Utf16FlyString;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;

#[repr(C)]
#[derive(Trace)]
pub struct AgentObject {
    base: Object,
}

define_object_class!(AgentObject, extends: [Object], methods: {
    initialize: AgentObject::initialize,
    ..ORDINARY_OBJECT_METHODS
});

impl AgentObject {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<AgentObject> {
        realm.create_object(
            vm,
            AgentObject {
                base: Object::new_without_prototype(vm, Self::CLASS, realm, MayInterfereWithIndexedPropertyAccess::No),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &PropertyKey::from(Utf16FlyString::from_utf8("monotonicNow")),
            raw_native!(AgentObject::monotonic_now),
            0,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &PropertyKey::from(Utf16FlyString::from_utf8("sleep")),
            raw_native!(AgentObject::sleep),
            1,
            attr,
            None,
        );
        // TODO: broadcast
        // TODO: getReport
        // TODO: start
    }

    #[allow(clippy::unnecessary_wraps, reason = "native functions return a completion")]
    fn monotonic_now(_vm: &Vm) -> ThrowCompletionOr<Value> {
        let milliseconds = monotonic_time_now_in_milliseconds();
        #[allow(
            clippy::cast_precision_loss,
            reason = "C++ converts the milliseconds to a double as well"
        )]
        Ok(Value::from_f64(milliseconds as f64))
    }

    fn sleep(vm: &Vm) -> ThrowCompletionOr<Value> {
        let milliseconds = vm.argument(0).to_i32(vm)?;
        // NB: Core::System::sleep_ms() takes a u32, so a negative count of milliseconds wraps around like in C++.
        let milliseconds = milliseconds.cast_unsigned();
        // SAFETY: usleep has no preconditions; like C++, a failure is ignored.
        unsafe { libc::usleep(milliseconds.wrapping_mul(1000)) };
        Ok(Value::UNDEFINED)
    }
}

/// MonotonicTime::now().milliseconds(): CLOCK_MONOTONIC, with the milliseconds rounded up as AK's
/// Duration::to_milliseconds() rounds them.
fn monotonic_time_now_in_milliseconds() -> i64 {
    let mut time = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    // SAFETY: The timespec is valid for writing.
    unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &raw mut time) };
    let seconds: i64 = time.tv_sec;
    let nanoseconds: i64 = time.tv_nsec;
    let mut milliseconds = seconds * 1000 + nanoseconds / 1_000_000;
    if nanoseconds % 1_000_000 != 0 {
        milliseconds += 1;
    }
    milliseconds
}
