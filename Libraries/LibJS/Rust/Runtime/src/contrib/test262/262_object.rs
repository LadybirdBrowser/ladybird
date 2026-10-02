/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use ak::Utf16FlyString;
use libjs_runtime_macros::Trace;

use crate::contrib::test262::agent_object::AgentObject;
use crate::contrib::test262::global_object::Test262GlobalObject;
use crate::contrib::test262::is_htmldda::IsHTMLDDA;
use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::runtime_functions::unimplemented_runtime_function;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::script::Script;
use crate::utf16::Utf16View;

/// JS::Test262::$262Object.
#[repr(C)]
#[derive(Trace)]
pub struct Dollar262Object {
    base: Object,
    agent: Cell<Option<Gc<AgentObject>>>,
    is_htmldda: Cell<Option<Gc<IsHTMLDDA>>>,
}

define_object_class!(Dollar262Object, extends: [Object], methods: {
    initialize: Dollar262Object::initialize,
    ..ORDINARY_OBJECT_METHODS
});

fn key(name: &str) -> PropertyKey {
    PropertyKey::from(Utf16FlyString::from_utf8(name))
}

impl Dollar262Object {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<Dollar262Object> {
        realm.create_object(
            vm,
            Dollar262Object {
                base: Object::new_without_prototype(vm, Self::CLASS, realm, MayInterfereWithIndexedPropertyAccess::No),
                agent: Cell::new(None),
                is_htmldda: Cell::new(None),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        // SAFETY: Only Dollar262Object has these methods.
        let this = unsafe { &*core::ptr::from_ref(object).cast::<Dollar262Object>() };

        let agent = AgentObject::create(vm, realm);
        this.agent.set(Some(agent));
        let is_htmldda = IsHTMLDDA::create(vm, realm);
        this.is_htmldda.set(Some(is_htmldda));

        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        let define = |name: &str, function, length| {
            object.define_native_function(vm, realm, &key(name), function, length, attr, None);
        };
        define("clearKeptObjects", raw_native!(Dollar262Object::clear_kept_objects), 0);
        define("createRealm", raw_native!(Dollar262Object::create_realm), 0);
        define(
            "detachArrayBuffer",
            raw_native!(Dollar262Object::detach_array_buffer),
            1,
        );
        define("evalScript", raw_native!(Dollar262Object::eval_script), 1);
        define("gc", raw_native!(Dollar262Object::collect_garbage), 1);

        object.define_direct_property(vm, &key("agent"), Value::from_object(agent), attr);
        object.define_direct_property(vm, &key("global"), Value::from_object(realm.global_object()), attr);
        object.define_direct_property(vm, &key("IsHTMLDDA"), Value::from_object(is_htmldda), attr);
    }

    #[allow(clippy::unnecessary_wraps, reason = "native functions return a completion")]
    fn clear_kept_objects(vm: &Vm) -> ThrowCompletionOr<Value> {
        // VM::finish_execution_generation()
        vm.head.execution_generation.set(vm.head.execution_generation.get() + 1);
        Ok(Value::UNDEFINED)
    }

    #[allow(clippy::unnecessary_wraps, reason = "native functions return a completion")]
    fn collect_garbage(vm: &Vm) -> ThrowCompletionOr<Value> {
        vm.heap().collect_garbage();
        Ok(Value::UNDEFINED)
    }

    #[allow(clippy::unnecessary_wraps, reason = "native functions return a completion")]
    fn create_realm(vm: &Vm) -> ThrowCompletionOr<Value> {
        let global_object: Cell<Option<Gc<Test262GlobalObject>>> = Cell::new(None);
        let create_global_object = |realm: Gc<Realm>| -> Gc<Object> {
            let object = Test262GlobalObject::allocate(vm, realm);
            global_object.set(Some(object));
            object.upcast()
        };
        let root_execution_context = Realm::initialize_host_defined_realm(vm, Some(&create_global_object), None).must();
        vm.pop_execution_context();
        drop(root_execution_context);
        let global_object = global_object
            .get()
            .expect("InitializeHostDefinedRealm creates the global object");
        Ok(Value::from_object(global_object.dollar_262()))
    }

    fn detach_array_buffer(vm: &Vm) -> ThrowCompletionOr<Value> {
        let array_buffer = vm.argument(0);
        // NB: The runtime has no ArrayBuffer yet, so no primitive and no object of the classes it has is one.
        if !array_buffer.is_object() {
            return vm.throw_completion_with_message(ErrorKind::TypeError, String::new());
        }

        unimplemented_runtime_function(
            "$262.detachArrayBuffer of an object, which needs ArrayBuffer and DetachArrayBuffer",
            0,
        )
    }

    fn eval_script(vm: &Vm) -> ThrowCompletionOr<Value> {
        let source_text = vm.argument(0).to_utf16_string(vm)?;

        // 1. Let hostDefined be any host-defined values for the provided sourceText (obtained in an implementation dependent manner)

        // 2. Let realm be the current Realm Record.
        let realm = vm.current_realm().expect("$262.evalScript runs in a realm");

        // 3. Let s be ParseScript(sourceText, realm, hostDefined).
        let mut source = Vec::new();
        Utf16View::of_string(&source_text).append_to(&mut source);
        let script_or_error = Script::parse(vm, &source, realm);

        // 4. If s is a List of errors, then
        let script = match script_or_error {
            Ok(script) => script,
            Err(errors) => {
                // a. Let error be the first element of s.
                let error = &errors[0];

                // b. Return Completion { [[Type]]: throw, [[Value]]: error, [[Target]]: empty }.
                return vm.throw_completion_with_message(ErrorKind::SyntaxError, error.to_string());
            }
        };

        // 5. Let status be ScriptEvaluation(s).
        let status = vm.run_script(script, None);

        // 6. Return Completion(status).
        status
    }
}
