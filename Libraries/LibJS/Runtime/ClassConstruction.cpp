/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibGC/ConservativeVector.h>
#include <LibJS/Bytecode/Executable.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Accessor.h>
#include <LibJS/Runtime/ClassConstruction.h>
#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/PrivateEnvironment.h>
#include <LibJS/Runtime/SharedFunctionInstanceData.h>
#include <LibJS/Runtime/ValueInlines.h>

namespace JS {

static void update_function_name(Value value, Utf16FlyString const& name)
{
    if (auto function = value.as_if<ECMAScriptFunctionObject>(); function && function->name().is_empty())
        function->set_name(name);
}

static ThrowCompletionOr<ClassElementName> resolve_element_key(VM& vm, Bytecode::ClassElementDescriptor const& descriptor, Value property_key)
{
    if (descriptor.is_private) {
        auto private_environment = vm.running_execution_context().private_environment;
        VERIFY(private_environment);
        return ClassElementName { private_environment->resolve_private_identifier(*descriptor.private_identifier) };
    }

    VERIFY(!property_key.is_special_empty_value());

    if (property_key.is_object())
        property_key = TRY(property_key.to_primitive(vm, Value::PreferredType::String));

    auto key = TRY(PropertyKey::from_value(vm, property_key));
    return ClassElementName { key };
}

static Utf16String compute_element_name(ClassElementName const& element_name, StringView prefix = {})
{
    auto name = element_name.visit(
        [&](PropertyKey const& property_key) {
            if (property_key.is_symbol()) {
                auto description = property_key.as_symbol()->description();
                if (!description.has_value() || description->is_empty())
                    return Utf16String {};
                return Utf16String::formatted("[{}]", *description);
            }
            return property_key.to_string();
        },
        [&](PrivateName const& private_name) {
            return private_name.description.to_utf16_string();
        });

    return Utf16String::formatted("{}{}{}", prefix, prefix.is_empty() ? "" : " ", name);
}

ThrowCompletionOr<ECMAScriptFunctionObject*> construct_class(
    VM& vm,
    Bytecode::ClassBlueprint const& blueprint,
    Bytecode::Executable const& executable,
    Environment* class_environment,
    Environment* outer_environment,
    Value super_class,
    ReadonlySpan<Value> element_keys,
    Optional<Utf16FlyString> const& binding_name,
    Utf16FlyString const& class_name)
{
    auto& realm = *vm.current_realm();

    // We might not set the lexical environment but we always want to restore it eventually.
    ArmedScopeGuard restore_environment = [&] {
        vm.running_execution_context().lexical_environment = outer_environment;
    };

    vm.running_execution_context().lexical_environment = class_environment;

    auto proto_parent = GC::Ptr { realm.intrinsics().object_prototype() };
    auto constructor_parent = realm.intrinsics().function_prototype();

    if (blueprint.has_super_class) {
        if (super_class.is_null()) {
            proto_parent = nullptr;
        } else if (!super_class.is_constructor()) {
            return vm.throw_completion<TypeError>(ErrorType::ClassExtendsValueNotAConstructorOrNull, super_class);
        } else {
            auto super_class_prototype = TRY(super_class.get(vm, vm.names.prototype));
            if (!super_class_prototype.is_null() && !super_class_prototype.is_object())
                return vm.throw_completion<TypeError>(ErrorType::ClassExtendsValueInvalidPrototype, super_class_prototype);

            if (super_class_prototype.is_null())
                proto_parent = nullptr;
            else
                proto_parent = super_class_prototype.as_object();

            constructor_parent = super_class.as_object();
        }
    }

    auto prototype = Object::create_prototype(realm, proto_parent);

    // FIXME: Step 14.a is done in the parser. By using a synthetic super(...args) which does not call @@iterator of %Array.prototype%
    auto constructor_shared_data = executable.shared_function_data[blueprint.constructor_shared_function_data_index];
    auto class_constructor = ECMAScriptFunctionObject::create_from_function_data(
        realm,
        *constructor_shared_data,
        vm.lexical_environment(),
        vm.running_execution_context().private_environment);

    class_constructor->set_name(class_name);
    class_constructor->set_home_object(prototype);
    class_constructor->set_is_class_constructor();
    class_constructor->define_direct_property(vm.names.prototype, prototype, Attribute::Writable);
    TRY(class_constructor->internal_set_prototype_of(constructor_parent));

    if (blueprint.has_super_class)
        class_constructor->set_constructor_kind(ConstructorKind::Derived);

    prototype->define_direct_property(vm.names.constructor, class_constructor, Attribute::Writable | Attribute::Configurable);

    using StaticElement = Variant<ClassFieldDefinition, GC::Ref<ECMAScriptFunctionObject>>;

    GC::ConservativeVector<PrivateElement> static_private_methods(vm.heap());
    GC::ConservativeVector<PrivateElement> instance_private_methods(vm.heap());
    GC::ConservativeVector<ClassFieldDefinition> instance_fields(vm.heap());
    GC::ConservativeVector<StaticElement> static_elements(vm.heap());

    for (size_t element_index = 0; element_index < blueprint.elements.size(); ++element_index) {
        auto const& descriptor = blueprint.elements[element_index];
        auto& home_object = descriptor.is_static ? static_cast<Object&>(*class_constructor) : static_cast<Object&>(*prototype);

        switch (descriptor.kind) {
        case Bytecode::ClassElementDescriptor::Kind::Method:
        case Bytecode::ClassElementDescriptor::Kind::Getter:
        case Bytecode::ClassElementDescriptor::Kind::Setter: {
            auto element_name = TRY(resolve_element_key(vm, descriptor, element_keys[element_index]));

            auto shared_data = executable.shared_function_data[*descriptor.shared_function_data_index];
            auto& method_function = *ECMAScriptFunctionObject::create_from_function_data(
                realm,
                *shared_data,
                vm.lexical_environment(),
                vm.running_execution_context().private_environment);

            auto method_value = Value(&method_function);
            method_function.make_method(home_object);

            if (element_name.has<PropertyKey>()) {
                auto& property_key = element_name.get<PropertyKey>();
                switch (descriptor.kind) {
                case Bytecode::ClassElementDescriptor::Kind::Method: {
                    update_function_name(method_value, compute_element_name(element_name));
                    PropertyDescriptor property_descriptor { .value = method_value, .writable = true, .enumerable = false, .configurable = true };
                    TRY(home_object.define_property_or_throw(property_key, property_descriptor));
                    break;
                }
                case Bytecode::ClassElementDescriptor::Kind::Getter: {
                    update_function_name(method_value, compute_element_name(element_name, "get"sv));
                    PropertyDescriptor property_descriptor { .get = &method_function, .enumerable = false, .configurable = true };
                    TRY(home_object.define_property_or_throw(property_key, property_descriptor));
                    break;
                }
                case Bytecode::ClassElementDescriptor::Kind::Setter: {
                    update_function_name(method_value, compute_element_name(element_name, "set"sv));
                    PropertyDescriptor property_descriptor { .set = &method_function, .enumerable = false, .configurable = true };
                    TRY(home_object.define_property_or_throw(property_key, property_descriptor));
                    break;
                }
                default:
                    VERIFY_NOT_REACHED();
                }
            } else {
                auto& private_name = element_name.get<PrivateName>();
                auto& container = descriptor.is_static ? static_private_methods : instance_private_methods;

                PrivateElement private_element = [&] {
                    switch (descriptor.kind) {
                    case Bytecode::ClassElementDescriptor::Kind::Method:
                        update_function_name(method_value, compute_element_name(element_name));
                        return PrivateElement { private_name, PrivateElement::Kind::Method, method_value };
                    case Bytecode::ClassElementDescriptor::Kind::Getter:
                        update_function_name(method_value, compute_element_name(element_name, "get"sv));
                        return PrivateElement { private_name, PrivateElement::Kind::Accessor, Value(Accessor::create(vm, &method_function, nullptr)) };
                    case Bytecode::ClassElementDescriptor::Kind::Setter:
                        update_function_name(method_value, compute_element_name(element_name, "set"sv));
                        return PrivateElement { private_name, PrivateElement::Kind::Accessor, Value(Accessor::create(vm, nullptr, &method_function)) };
                    default:
                        VERIFY_NOT_REACHED();
                    }
                }();

                // Merge accessor pairs.
                auto added_to_existing = false;
                for (auto& existing : container) {
                    if (existing.key == private_element.key) {
                        VERIFY(existing.kind == PrivateElement::Kind::Accessor);
                        VERIFY(private_element.kind == PrivateElement::Kind::Accessor);
                        auto& accessor = private_element.value.as_accessor();
                        if (!accessor.getter())
                            existing.value.as_accessor().set_setter(accessor.setter());
                        else
                            existing.value.as_accessor().set_getter(accessor.getter());
                        added_to_existing = true;
                    }
                }

                if (!added_to_existing)
                    container.append(move(private_element));
            }
            break;
        }

        case Bytecode::ClassElementDescriptor::Kind::Field: {
            auto element_name = TRY(resolve_element_key(vm, descriptor, element_keys[element_index]));

            Variant<GC::Ref<ECMAScriptFunctionObject>, Value, Empty> initializer;
            if (descriptor.has_initializer) {
                if (descriptor.literal_value.has_value()) {
                    initializer = *descriptor.literal_value;
                } else {
                    auto shared_data = executable.shared_function_data[*descriptor.shared_function_data_index];

                    // Set class_field_initializer_name at runtime for computed keys.
                    if (!descriptor.is_private && !shared_data->m_class_field_initializer_name.has<PropertyKey>()
                        && !shared_data->m_class_field_initializer_name.has<PrivateName>()) {
                        shared_data->m_class_field_initializer_name = element_name.visit(
                            [](PropertyKey const& key) -> Variant<PropertyKey, PrivateName, Empty> { return key; },
                            [](PrivateName const& name) -> Variant<PropertyKey, PrivateName, Empty> { return name; });
                    }

                    auto function = ECMAScriptFunctionObject::create_from_function_data(
                        realm,
                        *shared_data,
                        vm.lexical_environment(),
                        vm.running_execution_context().private_environment);
                    function->make_method(home_object);
                    initializer = function;
                }
            }

            ClassFieldDefinition field {
                move(element_name),
                move(initializer),
            };

            if (descriptor.is_static)
                static_elements.append(move(field));
            else
                instance_fields.append(move(field));
            break;
        }

        case Bytecode::ClassElementDescriptor::Kind::StaticInitializer: {
            auto shared_data = executable.shared_function_data[*descriptor.shared_function_data_index];
            auto body_function = ECMAScriptFunctionObject::create_from_function_data(
                realm,
                *shared_data,
                vm.lexical_environment(),
                vm.running_execution_context().private_environment);
            body_function->make_method(home_object);
            static_elements.append(GC::Ref { *body_function });
            break;
        }
        }
    }

    vm.running_execution_context().lexical_environment = outer_environment;
    restore_environment.disarm();

    if (binding_name.has_value())
        MUST(class_environment->initialize_binding(vm, binding_name.value(), class_constructor, Environment::InitializeBindingHint::Normal));

    for (auto& field : instance_fields)
        class_constructor->add_field(field);

    for (auto& private_method : instance_private_methods)
        class_constructor->add_private_method(private_method);

    for (auto& method : static_private_methods)
        TRY(class_constructor->private_method_or_accessor_add(move(method)));

    for (auto& element : static_elements) {
        TRY(element.visit(
            [&](ClassFieldDefinition& field) -> ThrowCompletionOr<void> {
                return TRY(class_constructor->define_field(field));
            },
            [&](GC::Ref<ECMAScriptFunctionObject> static_block_function) -> ThrowCompletionOr<void> {
                // We discard any value returned here.
                TRY(call(vm, *static_block_function, class_constructor));
                return {};
            }));
    }

    class_constructor->set_source_text(blueprint.source_text);

    return { class_constructor };
}

}
