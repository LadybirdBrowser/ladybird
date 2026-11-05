/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use ak::{ScopeGuard, Utf16FlyString};
use libjs_abi::ClassElementKind;
use libjs_runtime_macros::Trace;

use crate::bytecode::class_blueprint::{ClassBlueprint, ClassElementDescriptor};
use crate::bytecode::executable::Executable;
use crate::gc::root::MarkedVec;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::environment::Environment;
use crate::layout::execution_context::ExecutionContext;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::call_function_object;
use crate::runtime::accessor::Accessor;
use crate::runtime::class_field_definition::{ClassElementName, ClassFieldDefinition, ClassFieldInitializer};
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::ecmascript_function_object::{EcmascriptFunctionObject, value_as_ecmascript_function_object};
use crate::runtime::environment::InitializeBindingHint;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::object::{PrivateElement, PrivateElementKind};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_descriptor::PropertyDescriptor;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::shared_function_instance_data::{ClassFieldInitializerName, ConstructorKind};
use crate::runtime::value::PreferredType;

fn running_execution_context(vm: &Vm) -> &ExecutionContext {
    vm.running_execution_context_if_any()
        .expect("a class is constructed in a running execution context")
}

fn update_function_name(vm: &Vm, value: Value, name: &ClassElementName, prefix: Option<&str>) {
    if let Some(function) = value_as_ecmascript_function_object(value)
        && function.name().is_empty()
    {
        function.set_inferred_name(vm, name, prefix);
    }
}

fn resolve_element_key(
    vm: &Vm,
    descriptor: &ClassElementDescriptor,
    property_key: Value,
) -> ThrowCompletionOr<ClassElementName> {
    if descriptor.is_private {
        let private_environment = running_execution_context(vm)
            .private_environment
            .get()
            .expect("a class with private elements has a private environment");
        return Ok(ClassElementName::PrivateName(
            private_environment.resolve_private_identifier(
                descriptor
                    .private_identifier
                    .as_ref()
                    .expect("a private element has a private identifier"),
            ),
        ));
    }

    assert!(!property_key.is_empty());

    let mut property_key = property_key;
    if property_key.is_object() {
        property_key = property_key.to_primitive(vm, PreferredType::String)?;
    }

    let key = PropertyKey::from_value(vm, property_key)?;
    Ok(ClassElementName::PropertyKey(key))
}

/// The values of the class heritage of a class being defined.
pub struct ClassHeritage {
    pub super_class: Value,
    pub proto_parent: Value,
}

/// https://tc39.es/ecma262/#sec-runtime-semantics-classdefinitionevaluation
pub fn class_proto_parent(vm: &Vm, super_class: Value) -> ThrowCompletionOr<Value> {
    // 8. Else,
    // f. If superclass is null, then
    if super_class.is_null() {
        // i. Let protoParent be null.
        return Ok(Value::NULL);
    }
    // g. Else if IsConstructor(superclass) is false, then
    if !super_class.is_constructor() {
        // i. Throw a TypeError exception.
        return vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::ClassExtendsValueNotAConstructorOrNull,
            &[&super_class],
        );
    }
    // h. Else,
    // i. Let protoParent be ? Get(superclass, "prototype").
    let proto_parent = super_class.get(vm, &vm.names.prototype)?;
    // ii. If protoParent is not an Object and protoParent is not null, throw a TypeError exception.
    if !proto_parent.is_object() && !proto_parent.is_null() {
        return vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::ClassExtendsValueInvalidPrototype,
            &[&proto_parent],
        );
    }
    Ok(proto_parent)
}

#[derive(Clone, Trace)]
enum StaticElement {
    Field(ClassFieldDefinition),
    Block(Gc<EcmascriptFunctionObject>),
}

/// ClassDefinitionEvaluation from the class's blueprint, which `executable` holds and keeps alive.
#[allow(clippy::too_many_arguments)]
pub fn construct_class(
    vm: &Vm,
    blueprint: &ClassBlueprint,
    executable: Gc<Executable>,
    class_environment: Option<Gc<Environment>>,
    outer_environment: Option<Gc<Environment>>,
    heritage: Option<ClassHeritage>,
    element_keys: &[Value],
    binding_name: Option<&Utf16FlyString>,
    class_name: &ClassElementName,
) -> ThrowCompletionOr<Gc<EcmascriptFunctionObject>> {
    let realm = vm.current_realm().expect("there is a current realm");

    // We might not set the lexical environment but we always want to restore it eventually.
    let restore_environment_armed = Cell::new(true);
    let _restore_environment = ScopeGuard::new(|| {
        if restore_environment_armed.get() {
            running_execution_context(vm).lexical_environment.set(outer_environment);
        }
    });

    running_execution_context(vm).lexical_environment.set(class_environment);

    let mut proto_parent = Some(realm.object_prototype());
    let mut constructor_parent = realm.function_prototype();

    assert_eq!(heritage.is_some(), blueprint.has_super_class);
    if let Some(heritage) = heritage {
        // NB: The bytecode performs the steps that check the heritage, before evaluating the class elements.
        if heritage.proto_parent.is_null() {
            proto_parent = None;
        } else {
            proto_parent = Some(heritage.proto_parent.as_object());
        }
        if !heritage.super_class.is_null() {
            constructor_parent = heritage.super_class.as_object();
        }
    }

    let prototype = Object::create_prototype(vm, realm, proto_parent);

    // FIXME: Step 14.a is done in the parser. By using a synthetic super(...args) which does not call @@iterator of %Array.prototype%
    let constructor_shared_data = executable.shared_function_data(blueprint.constructor_shared_function_data_index);
    constructor_shared_data.set_is_class_constructor();
    let class_constructor = EcmascriptFunctionObject::create_from_function_data(
        vm,
        realm,
        constructor_shared_data,
        vm.lexical_environment(),
        running_execution_context(vm).private_environment.get(),
    );

    class_constructor.set_inferred_name(vm, class_name, None);
    class_constructor.set_home_object_of_class_constructor(prototype);
    class_constructor.define_direct_property(
        vm,
        &vm.names.prototype,
        Value::from_object(prototype),
        PropertyAttributes::new(0),
    );
    class_constructor.internal_set_prototype_of(vm, Some(constructor_parent))?;

    if blueprint.has_super_class {
        class_constructor.set_constructor_kind(ConstructorKind::Derived);
    }

    prototype.define_direct_property(
        vm,
        &vm.names.constructor,
        Value::from_object(class_constructor),
        PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE),
    );

    let static_private_methods: MarkedVec<PrivateElement> = MarkedVec::new(vm);
    let instance_private_methods: MarkedVec<PrivateElement> = MarkedVec::new(vm);
    let instance_fields: MarkedVec<ClassFieldDefinition> = MarkedVec::new(vm);
    let static_elements: MarkedVec<StaticElement> = MarkedVec::new(vm);

    for (element_index, descriptor) in blueprint.elements.iter().enumerate() {
        let home_object: Gc<Object> = if descriptor.is_static {
            class_constructor.upcast()
        } else {
            prototype
        };

        match descriptor.kind {
            ClassElementKind::Method | ClassElementKind::Getter | ClassElementKind::Setter => {
                let element_name = resolve_element_key(vm, descriptor, element_keys[element_index])?;

                let shared_data = executable.shared_function_data(
                    descriptor
                        .shared_function_data_index
                        .expect("a method has shared function data"),
                );
                let method_function = EcmascriptFunctionObject::create_method(
                    vm,
                    realm,
                    shared_data,
                    vm.lexical_environment(),
                    running_execution_context(vm).private_environment.get(),
                    home_object,
                    realm.function_prototype(),
                );

                let method_value = Value::from_object(method_function);

                match &element_name {
                    ClassElementName::PropertyKey(property_key) => match descriptor.kind {
                        ClassElementKind::Method => {
                            update_function_name(vm, method_value, &element_name, None);
                            let mut property_descriptor = PropertyDescriptor {
                                value: Some(method_value),
                                writable: Some(true),
                                enumerable: Some(false),
                                configurable: Some(true),
                                ..Default::default()
                            };
                            home_object.define_property_or_throw(vm, property_key, &mut property_descriptor)?;
                        }
                        ClassElementKind::Getter => {
                            update_function_name(vm, method_value, &element_name, Some("get"));
                            let mut property_descriptor = PropertyDescriptor {
                                get: Some(Some(method_function.upcast())),
                                enumerable: Some(false),
                                configurable: Some(true),
                                ..Default::default()
                            };
                            home_object.define_property_or_throw(vm, property_key, &mut property_descriptor)?;
                        }
                        ClassElementKind::Setter => {
                            update_function_name(vm, method_value, &element_name, Some("set"));
                            let mut property_descriptor = PropertyDescriptor {
                                set: Some(Some(method_function.upcast())),
                                enumerable: Some(false),
                                configurable: Some(true),
                                ..Default::default()
                            };
                            home_object.define_property_or_throw(vm, property_key, &mut property_descriptor)?;
                        }
                        _ => unreachable!("the element is a method, getter or setter"),
                    },
                    ClassElementName::PrivateName(private_name) => {
                        let container = if descriptor.is_static {
                            &static_private_methods
                        } else {
                            &instance_private_methods
                        };

                        let private_element = match descriptor.kind {
                            ClassElementKind::Method => {
                                update_function_name(vm, method_value, &element_name, None);
                                PrivateElement {
                                    key: private_name.clone(),
                                    kind: PrivateElementKind::Method,
                                    value: method_value,
                                }
                            }
                            ClassElementKind::Getter => {
                                update_function_name(vm, method_value, &element_name, Some("get"));
                                PrivateElement {
                                    key: private_name.clone(),
                                    kind: PrivateElementKind::Accessor,
                                    value: Value::from_accessor(Accessor::create(
                                        vm,
                                        Some(method_function.upcast()),
                                        None,
                                        None,
                                    )),
                                }
                            }
                            ClassElementKind::Setter => {
                                update_function_name(vm, method_value, &element_name, Some("set"));
                                PrivateElement {
                                    key: private_name.clone(),
                                    kind: PrivateElementKind::Accessor,
                                    value: Value::from_accessor(Accessor::create(
                                        vm,
                                        None,
                                        Some(method_function.upcast()),
                                        None,
                                    )),
                                }
                            }
                            _ => unreachable!("the element is a method, getter or setter"),
                        };

                        // Merge accessor pairs.
                        let mut added_to_existing = false;
                        for index in 0..container.len() {
                            let existing = container.get(index).expect("the index is in bounds");
                            if existing.key == private_element.key {
                                assert!(existing.kind == PrivateElementKind::Accessor);
                                assert!(private_element.kind == PrivateElementKind::Accessor);
                                let accessor = private_element.value.as_accessor();
                                if accessor.getter().is_none() {
                                    existing.value.as_accessor().set_setter(accessor.setter());
                                } else {
                                    existing.value.as_accessor().set_getter(accessor.getter());
                                }
                                added_to_existing = true;
                            }
                        }

                        if !added_to_existing {
                            container.push(private_element);
                        }
                    }
                }
            }

            ClassElementKind::Field => {
                let element_name = resolve_element_key(vm, descriptor, element_keys[element_index])?;

                let mut initializer = ClassFieldInitializer::Empty;
                if descriptor.has_initializer {
                    if let Some(literal_value) = descriptor.literal_value {
                        initializer = ClassFieldInitializer::Value(literal_value);
                    } else {
                        let shared_data = executable.shared_function_data(
                            descriptor
                                .shared_function_data_index
                                .expect("a field initializer has shared function data"),
                        );
                        let has_computed_name = matches!(
                            shared_data.class_field_initializer_name(),
                            ClassFieldInitializerName::Empty
                        );

                        let function = EcmascriptFunctionObject::create_method(
                            vm,
                            realm,
                            shared_data,
                            vm.lexical_environment(),
                            running_execution_context(vm).private_environment.get(),
                            home_object,
                            realm.function_prototype(),
                        );
                        if has_computed_name {
                            let ClassElementName::PropertyKey(key) = &element_name else {
                                unreachable!("a private name is never computed");
                            };
                            function.set_class_field_initializer_name(
                                vm,
                                ClassFieldInitializerName::PropertyKey(key.clone()),
                            );
                        }
                        initializer = ClassFieldInitializer::Function(function);
                    }
                }

                let field = ClassFieldDefinition {
                    name: element_name,
                    initializer,
                };

                if descriptor.is_static {
                    static_elements.push(StaticElement::Field(field));
                } else {
                    instance_fields.push(field);
                }
            }

            ClassElementKind::StaticInitializer => {
                let shared_data = executable.shared_function_data(
                    descriptor
                        .shared_function_data_index
                        .expect("a static initializer has shared function data"),
                );
                let body_function = EcmascriptFunctionObject::create_method(
                    vm,
                    realm,
                    shared_data,
                    vm.lexical_environment(),
                    running_execution_context(vm).private_environment.get(),
                    home_object,
                    realm.function_prototype(),
                );
                static_elements.push(StaticElement::Block(body_function));
            }
        }
    }

    running_execution_context(vm).lexical_environment.set(outer_environment);
    restore_environment_armed.set(false);

    if let Some(binding_name) = binding_name {
        class_environment
            .expect("a class with a binding has a class environment")
            .initialize_binding(
                vm,
                binding_name,
                Value::from_object(class_constructor),
                InitializeBindingHint::Normal,
            )
            .must();
    }

    for index in 0..instance_fields.len() {
        class_constructor.add_field(vm, instance_fields.get(index).expect("the index is in bounds"));
    }

    for index in 0..instance_private_methods.len() {
        class_constructor.add_private_method(vm, instance_private_methods.get(index).expect("the index is in bounds"));
    }

    for index in 0..static_private_methods.len() {
        class_constructor
            .private_method_or_accessor_add(vm, static_private_methods.get(index).expect("the index is in bounds"))?;
    }

    for index in 0..static_elements.len() {
        match static_elements.get(index).expect("the index is in bounds") {
            StaticElement::Field(field) => class_constructor.define_field(vm, &field)?,
            StaticElement::Block(static_block_function) => {
                // We discard any value returned here.
                call_function_object(
                    vm,
                    static_block_function.upcast(),
                    Value::from_object(class_constructor),
                    &[],
                )?;
            }
        }
    }

    if let Some(source_code) = &blueprint.source_code {
        class_constructor.set_source_text_range(
            source_code,
            blueprint.source_text_offset,
            blueprint.source_text_length,
        );
    }

    Ok(class_constructor)
}
