/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use ak::Utf16FlyString;
use libjs_runtime_macros::Trace;

use crate::console::Console;
use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::console_object_prototype::ConsoleObjectPrototype;
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;

#[repr(C)]
#[derive(Trace)]
pub struct ConsoleObject {
    base: Object,
    console: Cell<Option<Gc<Console>>>,
}

define_object_class!(ConsoleObject, extends: [Object], methods: {
    initialize: ConsoleObject::initialize,
    ..ORDINARY_OBJECT_METHODS
});

fn create_console_prototype(vm: &Vm, realm: Gc<Realm>) -> Gc<ConsoleObjectPrototype> {
    ConsoleObjectPrototype::create(vm, realm)
}

/// The console of the current realm's console object, which every console function uses, whatever its this value.
fn console_of_current_realm(vm: &Vm) -> Gc<Console> {
    let realm = vm.current_realm().expect("console functions run in a realm");
    realm.intrinsics().console_object(vm).console()
}

impl ConsoleObject {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<ConsoleObject> {
        let prototype = create_console_prototype(vm, realm);
        realm.create_object(
            vm,
            ConsoleObject {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    prototype.upcast(),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
                console: Cell::new(None),
            },
        )
    }

    pub fn console(&self) -> Gc<Console> {
        self.console.get().expect("the console object has a console")
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        // SAFETY: Only ConsoleObject has these methods.
        let this = unsafe { &*core::ptr::from_ref(object).cast::<ConsoleObject>() };
        let names = &vm.names;
        this.console.set(Some(Console::create(vm, realm)));
        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::ENUMERABLE | Attribute::CONFIGURABLE);
        let define =
            |name: &PropertyKey, function| object.define_native_function(vm, realm, name, function, 0, attr, None);
        define(&names.assert, raw_native!(ConsoleObject::assert_));
        define(&names.clear, raw_native!(ConsoleObject::clear));
        define(&names.debug, raw_native!(ConsoleObject::debug));
        define(&names.error, raw_native!(ConsoleObject::error));
        define(&names.info, raw_native!(ConsoleObject::info));
        define(&names.log, raw_native!(ConsoleObject::log));
        define(&names.table, raw_native!(ConsoleObject::table));
        define(&names.trace, raw_native!(ConsoleObject::trace));
        define(&names.warn, raw_native!(ConsoleObject::warn));
        define(&names.dir, raw_native!(ConsoleObject::dir));
        // NB: C++ defines dirxml with the native function of dir, so ConsoleObject::dirxml is never called.
        define(&names.dirxml, raw_native!(ConsoleObject::dir));
        define(&names.count, raw_native!(ConsoleObject::count));
        define(&names.countReset, raw_native!(ConsoleObject::count_reset));
        define(&names.group, raw_native!(ConsoleObject::group));
        define(&names.groupCollapsed, raw_native!(ConsoleObject::group_collapsed));
        define(&names.groupEnd, raw_native!(ConsoleObject::group_end));
        define(&names.time, raw_native!(ConsoleObject::time));
        define(&names.timeLog, raw_native!(ConsoleObject::time_log));
        define(&names.timeEnd, raw_native!(ConsoleObject::time_end));

        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_fly_string(
                vm,
                &Utf16FlyString::from_utf8("console"),
            )),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 1.1.1. assert(condition, ...data), https://console.spec.whatwg.org/#assert
    fn assert_(vm: &Vm) -> ThrowCompletionOr<Value> {
        console_of_current_realm(vm).assert_(vm)
    }

    // 1.1.2. clear(), https://console.spec.whatwg.org/#clear
    #[allow(clippy::unnecessary_wraps, reason = "native functions return a completion")]
    fn clear(vm: &Vm) -> ThrowCompletionOr<Value> {
        Ok(console_of_current_realm(vm).clear())
    }

    // 1.1.3. debug(...data), https://console.spec.whatwg.org/#debug
    fn debug(vm: &Vm) -> ThrowCompletionOr<Value> {
        console_of_current_realm(vm).debug(vm)
    }

    // 1.1.4. error(...data), https://console.spec.whatwg.org/#error
    fn error(vm: &Vm) -> ThrowCompletionOr<Value> {
        console_of_current_realm(vm).error(vm)
    }

    // 1.1.5. info(...data), https://console.spec.whatwg.org/#info
    fn info(vm: &Vm) -> ThrowCompletionOr<Value> {
        console_of_current_realm(vm).info(vm)
    }

    // 1.1.6. log(...data), https://console.spec.whatwg.org/#log
    fn log(vm: &Vm) -> ThrowCompletionOr<Value> {
        console_of_current_realm(vm).log(vm)
    }

    // 1.1.7. table(tabularData, properties), https://console.spec.whatwg.org/#table
    fn table(vm: &Vm) -> ThrowCompletionOr<Value> {
        console_of_current_realm(vm).table(vm)
    }

    // 1.1.8. trace(...data), https://console.spec.whatwg.org/#trace
    fn trace(vm: &Vm) -> ThrowCompletionOr<Value> {
        console_of_current_realm(vm).trace(vm)
    }

    // 1.1.9. warn(...data), https://console.spec.whatwg.org/#warn
    fn warn(vm: &Vm) -> ThrowCompletionOr<Value> {
        console_of_current_realm(vm).warn(vm)
    }

    // 1.1.10. dir(item, options), https://console.spec.whatwg.org/#warn
    fn dir(vm: &Vm) -> ThrowCompletionOr<Value> {
        console_of_current_realm(vm).dir(vm)
    }

    // 1.1.11 dirxml(...data) https://console.spec.whatwg.org/#dirxml
    #[allow(dead_code, reason = "C++ defines dirxml with dir, as initialize() does")]
    fn dirxml(vm: &Vm) -> ThrowCompletionOr<Value> {
        console_of_current_realm(vm).dirxml(vm)
    }

    // 1.2.1. count(label), https://console.spec.whatwg.org/#count
    fn count(vm: &Vm) -> ThrowCompletionOr<Value> {
        console_of_current_realm(vm).count(vm)
    }

    // 1.2.2. countReset(label), https://console.spec.whatwg.org/#countreset
    fn count_reset(vm: &Vm) -> ThrowCompletionOr<Value> {
        console_of_current_realm(vm).count_reset(vm)
    }

    // 1.3.1. group(...data), https://console.spec.whatwg.org/#group
    fn group(vm: &Vm) -> ThrowCompletionOr<Value> {
        console_of_current_realm(vm).group(vm)
    }

    // 1.3.2. groupCollapsed(...data), https://console.spec.whatwg.org/#groupcollapsed
    fn group_collapsed(vm: &Vm) -> ThrowCompletionOr<Value> {
        console_of_current_realm(vm).group_collapsed(vm)
    }

    // 1.3.3. groupEnd(), https://console.spec.whatwg.org/#groupend
    #[allow(clippy::unnecessary_wraps, reason = "native functions return a completion")]
    fn group_end(vm: &Vm) -> ThrowCompletionOr<Value> {
        Ok(console_of_current_realm(vm).group_end())
    }

    // 1.4.1. time(label), https://console.spec.whatwg.org/#time
    fn time(vm: &Vm) -> ThrowCompletionOr<Value> {
        console_of_current_realm(vm).time(vm)
    }

    // 1.4.2. timeLog(label, ...data), https://console.spec.whatwg.org/#timelog
    fn time_log(vm: &Vm) -> ThrowCompletionOr<Value> {
        console_of_current_realm(vm).time_log(vm)
    }

    // 1.4.3. timeEnd(label), https://console.spec.whatwg.org/#timeend
    fn time_end(vm: &Vm) -> ThrowCompletionOr<Value> {
        console_of_current_realm(vm).time_end(vm)
    }
}
