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
use crate::runtime::completion::{Completion, CompletionType, Throw, ThrowCompletionOr};
use crate::runtime::generator_object::{GeneratorObject, IterationResult};
use crate::runtime::iterator::create_iterator_result_object;
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::prototype_object::typed_this_object;
use crate::runtime::realm::Realm;

// 27.5.1 Properties of the Generator Prototype Object, https://tc39.es/ecma262/#sec-properties-of-generator-prototype
#[repr(C)]
#[derive(Trace)]
pub struct GeneratorPrototype {
    base: Object,
}

define_object_class!(GeneratorPrototype, extends: [Object], methods: {
    initialize: GeneratorPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

const DISPLAY_NAME: &str = "Generator";

impl GeneratorPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<GeneratorPrototype> {
        realm.create_object(
            vm,
            GeneratorPrototype {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.intrinsics().iterator_prototype(vm),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &vm.names.next,
            raw_native!(GeneratorPrototype::next),
            1,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &vm.names.return_,
            raw_native!(GeneratorPrototype::return_),
            1,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &vm.names.throw_,
            raw_native!(GeneratorPrototype::throw_),
            1,
            attr,
            None,
        );

        // 27.5.1.5 Generator.prototype [ @@toStringTag ], https://tc39.es/ecma262/#sec-generator.prototype-@@tostringtag
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_fly_string(
                vm,
                &Utf16FlyString::from_utf8("Generator"),
            )),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 27.5.1.2 Generator.prototype.next ( value ), https://tc39.es/ecma262/#sec-generator.prototype.next
    fn next(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Return ? GeneratorResume(this value, value, empty).
        let generator_object = typed_this_object::<GeneratorObject>(vm, DISPLAY_NAME)?;
        let iteration_result = generator_object.resume(vm, vm.argument(0), None)?;
        Ok(generator_resume_result_to_value(vm, iteration_result))
    }

    // 27.5.1.3 Generator.prototype.return ( value ), https://tc39.es/ecma262/#sec-generator.prototype.return
    fn return_(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let g be the this value.
        let generator_object = typed_this_object::<GeneratorObject>(vm, DISPLAY_NAME)?;

        // 2. Let C be Completion Record { [[Type]]: return, [[Value]]: value, [[Target]]: empty }.
        let completion = Completion::new(CompletionType::Return, vm.argument(0));

        // 3. Return ? GeneratorResumeAbrupt(g, C, empty).
        let iteration_result = generator_object.resume_abrupt(vm, completion, None)?;
        Ok(generator_resume_result_to_value(vm, iteration_result))
    }

    // 27.5.1.4 Generator.prototype.throw ( exception ), https://tc39.es/ecma262/#sec-generator.prototype.throw
    fn throw_(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let g be the this value.
        let generator_object = typed_this_object::<GeneratorObject>(vm, DISPLAY_NAME)?;

        // 2. Let C be ThrowCompletion(exception).
        let completion = Completion::from(Throw::new(vm.argument(0)));

        // 3. Return ? GeneratorResumeAbrupt(g, C, empty).
        let iteration_result = generator_object.resume_abrupt(vm, completion, None)?;
        Ok(generator_resume_result_to_value(vm, iteration_result))
    }
}

fn generator_resume_result_to_value(vm: &Vm, iteration_result: IterationResult) -> Value {
    if iteration_result.value_is_iterator_result {
        assert!(iteration_result.value.is_object());
        return iteration_result.value;
    }
    let realm = vm
        .current_realm()
        .expect("a generator is resumed in an execution context with a realm");
    Value::from_object(create_iterator_result_object(
        vm,
        realm,
        iteration_result.value,
        iteration_result.done,
    ))
}
