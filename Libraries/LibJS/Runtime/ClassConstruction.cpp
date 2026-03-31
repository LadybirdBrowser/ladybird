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
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/PrivateEnvironment.h>
#include <LibJS/Runtime/SharedFunctionInstanceData.h>
#include <LibJS/Runtime/ValueInlines.h>

namespace JS {

static void update_function_name(Value value, ClassElementName const& name, Optional<StringView> const& prefix = {})
{
    if (auto function = value.as_if<ECMAScriptFunctionObject>(); function && function->name().is_empty())
        function->set_inferred_name(name, prefix);
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

// === Decorator support ===

// A decorator value/receiver pair, corresponding to the spec's DecoratorDefinition Record.
struct DecoratorPair {
    Value decorator;
    Value receiver;
};

// ReadonlySpan<Value> contains (value, receiver) pairs in sequence.
// Extract decorator pairs for a given element from the flat decorator_values array.
static Vector<DecoratorPair> extract_decorator_pairs(ReadonlySpan<Value> decorator_values, size_t offset, u32 count)
{
    Vector<DecoratorPair> pairs;
    pairs.ensure_capacity(count);
    for (u32 i = 0; i < count; ++i) {
        auto base = offset + i * 2;
        pairs.unchecked_append({ decorator_values[base], decorator_values[base + 1] });
    }
    return pairs;
}

// CreateDecoratorAccessObject
// https://arai-a.github.io/ecma262-compare/?pr=2417#sec-createdecoratoraccessobject
static GC::Ref<Object> create_decorator_access_object(VM& vm, Bytecode::ClassElementDescriptor::Kind kind, ClassElementName const& name)
{
    auto& realm = *vm.current_realm();
    auto access_obj = Object::create_prototype(realm, realm.intrinsics().object_prototype());

    bool has_getter = kind == Bytecode::ClassElementDescriptor::Kind::Field
        || kind == Bytecode::ClassElementDescriptor::Kind::Method
        || kind == Bytecode::ClassElementDescriptor::Kind::AutoAccessor
        || kind == Bytecode::ClassElementDescriptor::Kind::Getter;

    bool has_setter = kind == Bytecode::ClassElementDescriptor::Kind::Field
        || kind == Bytecode::ClassElementDescriptor::Kind::AutoAccessor
        || kind == Bytecode::ClassElementDescriptor::Kind::Setter;

    if (has_getter) {
        auto getter_closure = [name](VM& vm) -> ThrowCompletionOr<Value> {
            auto obj_value = vm.argument(0);
            if (!obj_value.is_object())
                return vm.throw_completion<TypeError>(ErrorType::NotAnObject, obj_value);
            auto& obj = obj_value.as_object();
            return name.visit(
                [&](PropertyKey const& key) -> ThrowCompletionOr<Value> {
                    return obj.get(key);
                },
                [&](PrivateName const& priv) -> ThrowCompletionOr<Value> {
                    return obj.private_get(priv);
                });
        };
        auto getter = NativeFunction::create(realm, move(getter_closure), 1);
        MUST(access_obj->create_data_property_or_throw(vm.names.get, getter));
    }

    if (has_setter) {
        auto setter_closure = [name](VM& vm) -> ThrowCompletionOr<Value> {
            auto obj_value = vm.argument(0);
            if (!obj_value.is_object())
                return vm.throw_completion<TypeError>(ErrorType::NotAnObject, obj_value);
            auto& obj = obj_value.as_object();
            auto value = vm.argument(1);
            TRY(name.visit(
                [&](PropertyKey const& key) -> ThrowCompletionOr<void> {
                    return obj.set(key, value, Object::ShouldThrowExceptions::Yes);
                },
                [&](PrivateName const& priv) -> ThrowCompletionOr<void> {
                    return obj.private_set(priv, value);
                }));
            return js_undefined();
        };
        auto setter = NativeFunction::create(realm, move(setter_closure), 2);
        MUST(access_obj->create_data_property_or_throw(vm.names.set, setter));
    }

    auto has_closure = [name](VM& vm) -> ThrowCompletionOr<Value> {
        auto obj_value = vm.argument(0);
        if (!obj_value.is_object())
            return vm.throw_completion<TypeError>(ErrorType::NotAnObject, obj_value);
        auto& obj = obj_value.as_object();
        return name.visit(
            [&](PropertyKey const& key) -> ThrowCompletionOr<Value> {
                return Value(TRY(obj.has_property(key)));
            },
            [&](PrivateName const& priv) -> ThrowCompletionOr<Value> {
                return Value(obj.private_element_find(priv) != nullptr);
            });
    };
    auto has_fn = NativeFunction::create(realm, move(has_closure), 1);
    MUST(access_obj->create_data_property_or_throw(vm.names.has, has_fn));

    return access_obj;
}

// CreateAddInitializerFunction
// https://arai-a.github.io/ecma262-compare/?pr=2417#sec-createaddinitializerfunction
//
// NB: This closure captures `initializers` by reference. The reference points
// into `initializer_storage` on construct_class's stack frame. This is safe
// because `decoration_finished` (heap-allocated via shared_ptr) acts as a
// guard: construct_class sets *decoration_finished = true immediately after
// each decorator call returns, and again for class decorators before
// returning. The closure checks this flag FIRST, throwing TypeError before
// touching `initializers`. So:
//
//   - During decoration: construct_class is on the stack, initializers is live.
//   - After decoration: *decoration_finished is true, the closure throws
//     before reaching the initializers.append() line.
//
// Trade-off: this avoids heap-allocating a GC-managed container for each
// decorator call (which would require a custom Cell subclass and adds GC
// pressure), at the cost of relying on the finished-flag invariant. If a
// future change removes or reorders the flag-setting relative to construct_class's
// return, this will become a use-after-free. An alternative would be to
// allocate initializer_storage entries as GC-managed Cells.
static GC::Ref<NativeFunction> create_add_initializer_function(VM& vm, GC::ConservativeVector<Value>& initializers, std::shared_ptr<bool> decoration_finished)
{
    auto& realm = *vm.current_realm();
    auto closure = [&initializers, decoration_finished](VM& vm) -> ThrowCompletionOr<Value> {
        // Guard: must check BEFORE accessing `initializers`, which may be dangling
        // if construct_class has already returned.
        if (*decoration_finished)
            return vm.throw_completion<TypeError>(ErrorType::DecoratorAddInitializerAfterFinished);
        auto initializer = vm.argument(0);
        if (!initializer.is_function())
            return vm.throw_completion<TypeError>(ErrorType::DecoratorAddInitializerNotCallable);
        initializers.append(initializer);
        return js_undefined();
    };
    return NativeFunction::create(realm, move(closure), 1);
}

// CreateDecoratorContextObject
// https://arai-a.github.io/ecma262-compare/?pr=2417#sec-createdecoratorcontextobject
static GC::Ref<Object> create_decorator_context_object(
    VM& vm,
    Bytecode::ClassElementDescriptor::Kind kind,
    ClassElementName const& name,
    GC::ConservativeVector<Value>& initializers,
    std::shared_ptr<bool> decoration_finished,
    Optional<bool> is_static = {})
{
    auto& realm = *vm.current_realm();
    auto context_obj = Object::create_prototype(realm, realm.intrinsics().object_prototype());

    // Set kind string.
    StringView kind_str;
    switch (kind) {
    case Bytecode::ClassElementDescriptor::Kind::Method:
        kind_str = "method"sv;
        break;
    case Bytecode::ClassElementDescriptor::Kind::Getter:
        kind_str = "getter"sv;
        break;
    case Bytecode::ClassElementDescriptor::Kind::Setter:
        kind_str = "setter"sv;
        break;
    case Bytecode::ClassElementDescriptor::Kind::AutoAccessor:
        kind_str = "accessor"sv;
        break;
    case Bytecode::ClassElementDescriptor::Kind::Field:
        kind_str = "field"sv;
        break;
    default:
        VERIFY_NOT_REACHED();
    }
    MUST(context_obj->create_data_property_or_throw(vm.names.kind, PrimitiveString::create(vm, kind_str)));

    // For non-class kinds, set access, static, private, and name.
    MUST(context_obj->create_data_property_or_throw(vm.names.access, create_decorator_access_object(vm, kind, name)));
    if (is_static.has_value())
        MUST(context_obj->create_data_property_or_throw(vm.names.static_, Value(is_static.value())));

    name.visit(
        [&](PropertyKey const& key) {
            MUST(context_obj->create_data_property_or_throw(vm.names.private_, Value(false)));
            if (key.is_string())
                MUST(context_obj->create_data_property_or_throw(vm.names.name, PrimitiveString::create(vm, key.as_string())));
            else if (key.is_symbol())
                MUST(context_obj->create_data_property_or_throw(vm.names.name, key.as_symbol()));
            else if (key.is_number())
                MUST(context_obj->create_data_property_or_throw(vm.names.name, PrimitiveString::create(vm, String::number(key.as_number()))));
        },
        [&](PrivateName const& priv) {
            MUST(context_obj->create_data_property_or_throw(vm.names.private_, Value(true)));
            MUST(context_obj->create_data_property_or_throw(vm.names.name, PrimitiveString::create(vm, priv.description)));
        });

    auto add_initializer = create_add_initializer_function(vm, initializers, decoration_finished);
    MUST(context_obj->create_data_property_or_throw(vm.names.addInitializer, add_initializer));

    return context_obj;
}

// CreateDecoratorContextObject for class decorators
static GC::Ref<Object> create_class_decorator_context_object(
    VM& vm,
    Utf16FlyString const& class_name,
    GC::ConservativeVector<Value>& initializers,
    std::shared_ptr<bool> decoration_finished)
{
    auto& realm = *vm.current_realm();
    auto context_obj = Object::create_prototype(realm, realm.intrinsics().object_prototype());

    MUST(context_obj->create_data_property_or_throw(vm.names.kind, PrimitiveString::create(vm, "class"sv)));
    MUST(context_obj->create_data_property_or_throw(vm.names.name, PrimitiveString::create(vm, class_name)));

    auto add_initializer = create_add_initializer_function(vm, initializers, decoration_finished);
    MUST(context_obj->create_data_property_or_throw(vm.names.addInitializer, add_initializer));

    return context_obj;
}

ThrowCompletionOr<FunctionObject*> construct_class(
    VM& vm,
    Bytecode::ClassBlueprint const& blueprint,
    Bytecode::Executable const& executable,
    Environment* class_environment,
    Environment* outer_environment,
    Value super_class,
    ReadonlySpan<Value> element_keys,
    ReadonlySpan<Value> decorator_values,
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
    class_constructor->define_direct_property(vm.names.prototype, prototype, 0);
    TRY(class_constructor->internal_set_prototype_of(constructor_parent));

    if (blueprint.has_super_class)
        class_constructor->set_constructor_kind(ConstructorKind::Derived);

    prototype->define_direct_property(vm.names.constructor, class_constructor, Attribute::Writable | Attribute::Configurable);

    // Track whether any element has decorators to enable the fast path.
    bool has_any_decorators = blueprint.class_decorator_count > 0;
    if (!has_any_decorators) {
        for (auto const& descriptor : blueprint.elements) {
            if (descriptor.decorator_count > 0) {
                has_any_decorators = true;
                break;
            }
        }
    }

    // Fast path: no decorators at all -- use the existing simple algorithm.
    if (!has_any_decorators) {
        using StaticElement = Variant<ClassFieldDefinition, GC::Ref<ECMAScriptFunctionObject>>;

        GC::ConservativeVector<PrivateElement> static_private_methods;
        GC::ConservativeVector<PrivateElement> instance_private_methods;
        GC::ConservativeVector<ClassFieldDefinition> instance_fields;
        GC::ConservativeVector<StaticElement> static_elements;

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
                        update_function_name(method_value, element_name);
                        PropertyDescriptor property_descriptor { .value = method_value, .writable = true, .enumerable = false, .configurable = true };
                        TRY(home_object.define_property_or_throw(property_key, property_descriptor));
                        break;
                    }
                    case Bytecode::ClassElementDescriptor::Kind::Getter: {
                        update_function_name(method_value, element_name, "get"sv);
                        PropertyDescriptor property_descriptor { .get = &method_function, .enumerable = false, .configurable = true };
                        TRY(home_object.define_property_or_throw(property_key, property_descriptor));
                        break;
                    }
                    case Bytecode::ClassElementDescriptor::Kind::Setter: {
                        update_function_name(method_value, element_name, "set"sv);
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
                            update_function_name(method_value, element_name);
                            return PrivateElement { private_name, PrivateElement::Kind::Method, method_value };
                        case Bytecode::ClassElementDescriptor::Kind::Getter:
                            update_function_name(method_value, element_name, "get"sv);
                            return PrivateElement { private_name, PrivateElement::Kind::Accessor, Value(Accessor::create(vm, &method_function, nullptr)) };
                        case Bytecode::ClassElementDescriptor::Kind::Setter:
                            update_function_name(method_value, element_name, "set"sv);
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
                    {},
                    {},
                };

                if (descriptor.is_static)
                    static_elements.append(move(field));
                else
                    instance_fields.append(move(field));
                break;
            }

            case Bytecode::ClassElementDescriptor::Kind::AutoAccessor: {
                auto element_name = TRY(resolve_element_key(vm, descriptor, element_keys[element_index]));

                // Resolve the backing storage private name from the private environment.
                auto private_environment = vm.running_execution_context().private_environment;
                VERIFY(private_environment);
                auto backing_storage_name = private_environment->resolve_private_identifier(*descriptor.backing_storage_name);

                // Create initializer for the backing storage field.
                Variant<GC::Ref<ECMAScriptFunctionObject>, Value, Empty> initializer;
                if (descriptor.has_initializer) {
                    if (descriptor.literal_value.has_value()) {
                        initializer = *descriptor.literal_value;
                    } else {
                        auto shared_data = executable.shared_function_data[*descriptor.shared_function_data_index];

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

                // The backing storage is always a private field, initialized with the auto-accessor's initializer value.
                ClassFieldDefinition backing_field {
                    ClassElementName(backing_storage_name),
                    move(initializer),
                    {},
                    {},
                };

                // MakeAutoAccessorGetter: create a getter that reads from the backing storage.
                // NOTE: The base spec (ecma262 PR #2417) calls SetFunctionName to produce
                // "get x" / "set x" names, but pzuraq/ecma262 PR #14 removes those calls.
                // See tc39/proposal-decorators#502 for context on SetFunctionName in decorators.
                auto getter_closure = [backing_storage_name](VM& vm) -> ThrowCompletionOr<Value> {
                    auto this_value = vm.this_value();
                    if (!this_value.is_object())
                        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, this_value);
                    return this_value.as_object().private_get(backing_storage_name);
                };
                auto getter = NativeFunction::create(realm, move(getter_closure), 0);

                // MakeAutoAccessorSetter: create a setter that writes to the backing storage.
                auto setter_closure = [backing_storage_name](VM& vm) -> ThrowCompletionOr<Value> {
                    auto this_value = vm.this_value();
                    if (!this_value.is_object())
                        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, this_value);
                    TRY(this_value.as_object().private_set(backing_storage_name, vm.argument(0)));
                    return js_undefined();
                };
                auto setter = NativeFunction::create(realm, move(setter_closure), 1);

                if (element_name.has<PropertyKey>()) {
                    // Public auto-accessor: define an accessor property on the home object.
                    auto& property_key = element_name.get<PropertyKey>();
                    PropertyDescriptor property_descriptor { .get = getter, .set = setter, .enumerable = false, .configurable = true };
                    TRY(home_object.define_property_or_throw(property_key, property_descriptor));
                } else {
                    // Private auto-accessor: add getter/setter as a private accessor element.
                    auto& private_name = element_name.get<PrivateName>();
                    auto accessor = Accessor::create(vm, getter, setter);
                    PrivateElement private_element { private_name, PrivateElement::Kind::Accessor, Value(accessor) };

                    auto& container = descriptor.is_static ? static_private_methods : instance_private_methods;
                    container.append(move(private_element));
                }

                // Add the backing storage field to the appropriate list.
                if (descriptor.is_static)
                    static_elements.append(move(backing_field));
                else
                    instance_fields.append(move(backing_field));
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

        if (blueprint.source_code)
            class_constructor->set_source_text_range(*blueprint.source_code, blueprint.source_text_offset, blueprint.source_text_length);

        return { class_constructor };
    }

    // =========================================================================
    // Slow path: decorators present -- implement the full Decoration Order
    // algorithm from ClassDefinitionEvaluation.
    // =========================================================================

    // Private element vectors for accumulating during decoration.
    GC::ConservativeVector<PrivateElement> static_private_methods;
    GC::ConservativeVector<PrivateElement> instance_private_methods;

    // Compute decorator value offsets per element. decorator_values is a flat
    // array of (value, receiver) pairs: first all element decorator pairs (in
    // source order), then class decorator pairs.
    // Each element's decorator_count tells how many pairs belong to it.
    size_t decorator_offset = 0;

    // Build element records with resolved names, values, and decorator pairs.
    // We classify into instance and static elements for the 4-pass algorithm.

    struct ElementRecord {
        size_t blueprint_index;
        ClassElementName name { PropertyKey { static_cast<unsigned>(0) } };
        Bytecode::ClassElementDescriptor::Kind kind;
        bool is_static;
        // For methods/getters/setters: the function value.
        Value value {};
        // For auto-accessors: getter and setter.
        GC::Ptr<FunctionObject> getter {};
        GC::Ptr<FunctionObject> setter {};
        // For fields/auto-accessors: the base initializer.
        Variant<GC::Ref<ECMAScriptFunctionObject>, Value, Empty> initializer { Empty {} };
        // Private backing storage name (auto-accessors only).
        Optional<PrivateName> backing_storage_name {};
        // Decorator pairs for this element.
        Vector<DecoratorPair> decorators;
        // Initializers added by decorators (prepended by field/accessor decorators).
        GC::ConservativeVector<Value>* decorator_initializers { nullptr };
        // Extra initializers added via addInitializer().
        GC::ConservativeVector<Value>* extra_initializers { nullptr };
        // Whether this is a private element.
        bool is_private { false };
        // For private methods: the PrivateName for installation.
        Optional<PrivateName> private_name {};
    };

    // We need GC-safe storage for initializer lists. Allocate them per-element.
    // Use a vector of unique_ptrs to GC::ConservativeVector since we need stable addresses.
    Vector<NonnullOwnPtr<GC::ConservativeVector<Value>>> initializer_storage;
    auto allocate_initializer_list = [&]() -> GC::ConservativeVector<Value>* {
        initializer_storage.append(make<GC::ConservativeVector<Value>>());
        return initializer_storage.last().ptr();
    };

    Vector<ElementRecord> instance_elements;
    Vector<ElementRecord> static_elements_vec;

    for (size_t element_index = 0; element_index < blueprint.elements.size(); ++element_index) {
        auto const& descriptor = blueprint.elements[element_index];
        auto& home_object = descriptor.is_static ? static_cast<Object&>(*class_constructor) : static_cast<Object&>(*prototype);

        auto dec_pairs = extract_decorator_pairs(decorator_values, decorator_offset, descriptor.decorator_count);
        decorator_offset += descriptor.decorator_count * 2;

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
            method_function.make_method(home_object);

            ElementRecord record;
            record.blueprint_index = element_index;
            record.name = move(element_name);
            record.kind = descriptor.kind;
            record.is_static = descriptor.is_static;
            record.value = Value(&method_function);
            record.decorators = move(dec_pairs);
            record.is_private = descriptor.is_private;
            record.extra_initializers = allocate_initializer_list();
            if (descriptor.is_private)
                record.private_name = record.name.get<PrivateName>();

            if (descriptor.is_static)
                static_elements_vec.append(move(record));
            else
                instance_elements.append(move(record));
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

            ElementRecord record;
            record.blueprint_index = element_index;
            record.name = move(element_name);
            record.kind = descriptor.kind;
            record.is_static = descriptor.is_static;
            record.initializer = move(initializer);
            record.decorators = move(dec_pairs);
            record.is_private = descriptor.is_private;
            record.decorator_initializers = allocate_initializer_list();
            record.extra_initializers = allocate_initializer_list();

            if (descriptor.is_static)
                static_elements_vec.append(move(record));
            else
                instance_elements.append(move(record));
            break;
        }

        case Bytecode::ClassElementDescriptor::Kind::AutoAccessor: {
            auto element_name = TRY(resolve_element_key(vm, descriptor, element_keys[element_index]));

            auto private_environment = vm.running_execution_context().private_environment;
            VERIFY(private_environment);
            auto backing_storage = private_environment->resolve_private_identifier(*descriptor.backing_storage_name);

            Variant<GC::Ref<ECMAScriptFunctionObject>, Value, Empty> initializer;
            if (descriptor.has_initializer) {
                if (descriptor.literal_value.has_value()) {
                    initializer = *descriptor.literal_value;
                } else {
                    auto shared_data = executable.shared_function_data[*descriptor.shared_function_data_index];
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

            // MakeAutoAccessorGetter/Setter
            auto getter_closure = [backing_storage](VM& vm) -> ThrowCompletionOr<Value> {
                auto this_value = vm.this_value();
                if (!this_value.is_object())
                    return vm.throw_completion<TypeError>(ErrorType::NotAnObject, this_value);
                return this_value.as_object().private_get(backing_storage);
            };
            auto acc_getter = NativeFunction::create(realm, move(getter_closure), 0);

            auto setter_closure = [backing_storage](VM& vm) -> ThrowCompletionOr<Value> {
                auto this_value = vm.this_value();
                if (!this_value.is_object())
                    return vm.throw_completion<TypeError>(ErrorType::NotAnObject, this_value);
                TRY(this_value.as_object().private_set(backing_storage, vm.argument(0)));
                return js_undefined();
            };
            auto acc_setter = NativeFunction::create(realm, move(setter_closure), 1);

            ElementRecord record;
            record.blueprint_index = element_index;
            record.name = move(element_name);
            record.kind = descriptor.kind;
            record.is_static = descriptor.is_static;
            record.getter = acc_getter;
            record.setter = acc_setter;
            record.initializer = move(initializer);
            record.backing_storage_name = backing_storage;
            record.decorators = move(dec_pairs);
            record.is_private = descriptor.is_private;
            record.decorator_initializers = allocate_initializer_list();
            record.extra_initializers = allocate_initializer_list();

            if (descriptor.is_static)
                static_elements_vec.append(move(record));
            else
                instance_elements.append(move(record));
            break;
        }

        case Bytecode::ClassElementDescriptor::Kind::StaticInitializer: {
            auto shared_data = executable.shared_function_data[*descriptor.shared_function_data_index];
            auto body_function = ECMAScriptFunctionObject::create_from_function_data(
                realm,
                *shared_data,
                vm.lexical_environment(),
                vm.running_execution_context().private_environment);
            body_function->make_method(static_cast<Object&>(*class_constructor));

            // Static initializer blocks are not decorated; store as a field-kind
            // element with the body function as its value.
            ElementRecord record;
            record.blueprint_index = element_index;
            // Static initializers don't have a meaningful name.
            record.kind = descriptor.kind;
            record.is_static = true;
            record.value = Value(body_function.ptr());
            record.is_private = false;

            static_elements_vec.append(move(record));
            break;
        }
        }
    }

    // === Apply Decorators per the spec's Decoration Order ===

    // Helper: apply decorators to an element record.
    // ApplyDecoratorsToElementDefinition
    auto apply_decorators_to_element = [&](ElementRecord& record) -> ThrowCompletionOr<void> {
        if (record.decorators.is_empty())
            return {};

        // Decorators are applied in reverse order (bottom-up / inner-to-outer).
        for (ssize_t di = record.decorators.size() - 1; di >= 0; --di) {
            auto& dec_pair = record.decorators[di];
            auto decoration_finished = std::make_shared<bool>(false);
            auto context = create_decorator_context_object(vm, record.kind, record.name, *record.extra_initializers, decoration_finished, record.is_static);

            // Determine the value to pass to the decorator.
            Value decorator_input;
            switch (record.kind) {
            case Bytecode::ClassElementDescriptor::Kind::Method:
                decorator_input = record.value;
                break;
            case Bytecode::ClassElementDescriptor::Kind::Getter:
                decorator_input = record.value;
                break;
            case Bytecode::ClassElementDescriptor::Kind::Setter:
                decorator_input = record.value;
                break;
            case Bytecode::ClassElementDescriptor::Kind::AutoAccessor: {
                auto accessor_obj = Object::create_prototype(realm, realm.intrinsics().object_prototype());
                MUST(accessor_obj->create_data_property_or_throw(vm.names.get, record.getter));
                MUST(accessor_obj->create_data_property_or_throw(vm.names.set, record.setter));
                decorator_input = accessor_obj;
                break;
            }
            case Bytecode::ClassElementDescriptor::Kind::Field:
                decorator_input = js_undefined();
                break;
            default:
                VERIFY_NOT_REACHED();
            }

            // Call the decorator: decorator.call(receiver, value, context)
            auto new_value = TRY(call(vm, dec_pair.decorator, dec_pair.receiver, decorator_input, context));
            *decoration_finished = true;

            // Handle return value based on kind.
            switch (record.kind) {
            case Bytecode::ClassElementDescriptor::Kind::Field:
                if (new_value.is_function()) {
                    record.decorator_initializers->append(new_value);
                } else if (!new_value.is_undefined()) {
                    return vm.throw_completion<TypeError>(ErrorType::DecoratorInvalidReturnValue);
                }
                break;

            case Bytecode::ClassElementDescriptor::Kind::AutoAccessor:
                if (new_value.is_object()) {
                    auto new_getter = TRY(new_value.get(vm, vm.names.get));
                    if (new_getter.is_function())
                        record.getter = &new_getter.as_function();
                    else if (!new_getter.is_undefined())
                        return vm.throw_completion<TypeError>(ErrorType::DecoratorInvalidReturnValue);

                    auto new_setter = TRY(new_value.get(vm, vm.names.set));
                    if (new_setter.is_function())
                        record.setter = &new_setter.as_function();
                    else if (!new_setter.is_undefined())
                        return vm.throw_completion<TypeError>(ErrorType::DecoratorInvalidReturnValue);

                    auto init = TRY(new_value.get(vm, vm.names.init));
                    if (init.is_function())
                        record.decorator_initializers->append(init);
                    else if (!init.is_undefined())
                        return vm.throw_completion<TypeError>(ErrorType::DecoratorInvalidReturnValue);
                } else if (!new_value.is_undefined()) {
                    return vm.throw_completion<TypeError>(ErrorType::DecoratorInvalidReturnValue);
                }
                break;

            case Bytecode::ClassElementDescriptor::Kind::Method:
            case Bytecode::ClassElementDescriptor::Kind::Getter:
            case Bytecode::ClassElementDescriptor::Kind::Setter:
                if (new_value.is_function()) {
                    record.value = new_value;
                } else if (!new_value.is_undefined()) {
                    return vm.throw_completion<TypeError>(ErrorType::DecoratorInvalidReturnValue);
                }
                break;

            default:
                VERIFY_NOT_REACHED();
            }
        }

        record.decorators.clear();
        return {};
    };

    // Helper: define a method/getter/setter element on its home object.
    auto define_method_element = [&](ElementRecord& record) -> ThrowCompletionOr<void> {
        auto& home_object = record.is_static ? static_cast<Object&>(*class_constructor) : static_cast<Object&>(*prototype);

        if (record.name.has<PropertyKey>()) {
            auto& property_key = record.name.get<PropertyKey>();
            switch (record.kind) {
            case Bytecode::ClassElementDescriptor::Kind::Method: {
                update_function_name(record.value, record.name);
                PropertyDescriptor desc { .value = record.value, .writable = true, .enumerable = false, .configurable = true };
                TRY(home_object.define_property_or_throw(property_key, desc));
                break;
            }
            case Bytecode::ClassElementDescriptor::Kind::Getter: {
                update_function_name(record.value, record.name, "get"sv);
                PropertyDescriptor desc { .get = &record.value.as_function(), .enumerable = false, .configurable = true };
                TRY(home_object.define_property_or_throw(property_key, desc));
                break;
            }
            case Bytecode::ClassElementDescriptor::Kind::Setter: {
                update_function_name(record.value, record.name, "set"sv);
                PropertyDescriptor desc { .set = &record.value.as_function(), .enumerable = false, .configurable = true };
                TRY(home_object.define_property_or_throw(property_key, desc));
                break;
            }
            case Bytecode::ClassElementDescriptor::Kind::AutoAccessor: {
                PropertyDescriptor desc { .get = record.getter, .set = record.setter, .enumerable = false, .configurable = true };
                TRY(home_object.define_property_or_throw(property_key, desc));
                break;
            }
            default:
                break;
            }
        } else {
            auto& priv = record.name.get<PrivateName>();
            auto& container = record.is_static ? static_private_methods : instance_private_methods;

            if (record.kind == Bytecode::ClassElementDescriptor::Kind::Method) {
                update_function_name(record.value, record.name);
                container.append(PrivateElement { priv, PrivateElement::Kind::Method, record.value });
            } else if (record.kind == Bytecode::ClassElementDescriptor::Kind::AutoAccessor) {
                auto accessor = Accessor::create(vm, record.getter, record.setter);
                container.append(PrivateElement { priv, PrivateElement::Kind::Accessor, Value(accessor) });
            } else {
                // Getter or setter -- merge with existing accessor pair.
                GC::Ptr<FunctionObject> g;
                GC::Ptr<FunctionObject> s;
                if (record.kind == Bytecode::ClassElementDescriptor::Kind::Getter) {
                    update_function_name(record.value, record.name, "get"sv);
                    g = &record.value.as_function();
                } else {
                    update_function_name(record.value, record.name, "set"sv);
                    s = &record.value.as_function();
                }

                bool merged = false;
                for (auto& existing : container) {
                    if (existing.key == priv && existing.kind == PrivateElement::Kind::Accessor) {
                        if (g)
                            existing.value.as_accessor().set_getter(g);
                        else
                            existing.value.as_accessor().set_setter(s);
                        merged = true;
                        break;
                    }
                }
                if (!merged)
                    container.append(PrivateElement { priv, PrivateElement::Kind::Accessor, Value(Accessor::create(vm, g, s)) });
            }
        }
        return {};
    };

    // Shared extra-initializer lists for method-level decorators (per spec).
    GC::ConservativeVector<Value> instance_method_extra_initializers;
    GC::ConservativeVector<Value> static_method_extra_initializers;

    // Pass 1: Apply decorators to static methods/accessors, then define them.
    for (auto& elem : static_elements_vec) {
        if (elem.kind != Bytecode::ClassElementDescriptor::Kind::Field
            && elem.kind != Bytecode::ClassElementDescriptor::Kind::StaticInitializer) {
            // For accessors, extra initializers go into elem.extra_initializers.
            // For methods/getters/setters, they go into static_method_extra_initializers.
            auto* target_extra_inits = (elem.kind == Bytecode::ClassElementDescriptor::Kind::AutoAccessor)
                ? elem.extra_initializers
                : &static_method_extra_initializers;
            // Temporarily redirect extra_initializers for method decorators.
            auto* saved = elem.extra_initializers;
            elem.extra_initializers = target_extra_inits;
            TRY(apply_decorators_to_element(elem));
            elem.extra_initializers = saved;
            TRY(define_method_element(elem));
        }
    }

    // Pass 2: Apply decorators to instance methods/accessors, then define them.
    for (auto& elem : instance_elements) {
        if (elem.kind != Bytecode::ClassElementDescriptor::Kind::Field
            && elem.kind != Bytecode::ClassElementDescriptor::Kind::StaticInitializer) {
            auto* target_extra_inits = (elem.kind == Bytecode::ClassElementDescriptor::Kind::AutoAccessor)
                ? elem.extra_initializers
                : &instance_method_extra_initializers;
            auto* saved = elem.extra_initializers;
            elem.extra_initializers = target_extra_inits;
            TRY(apply_decorators_to_element(elem));
            elem.extra_initializers = saved;
            TRY(define_method_element(elem));
        }
    }

    // Pass 3: Apply decorators to static fields.
    // Auto-accessors are already decorated in Pass 1 (as accessors, not fields).
    for (auto& elem : static_elements_vec) {
        if (elem.kind == Bytecode::ClassElementDescriptor::Kind::Field)
            TRY(apply_decorators_to_element(elem));
    }

    // Pass 4: Apply decorators to instance fields.
    for (auto& elem : instance_elements) {
        if (elem.kind == Bytecode::ClassElementDescriptor::Kind::Field)
            TRY(apply_decorators_to_element(elem));
    }

    // Store instance elements on F.[[Elements]].
    for (auto& elem : instance_elements) {
        if (elem.kind == Bytecode::ClassElementDescriptor::Kind::Field) {
            Vector<GC::Ref<FunctionObject>> dec_inits;
            if (elem.decorator_initializers) {
                for (auto& di : *elem.decorator_initializers)
                    dec_inits.append(di.as_function());
            }
            Vector<GC::Ref<FunctionObject>> extra_inits;
            if (elem.extra_initializers) {
                for (auto& ei : *elem.extra_initializers)
                    extra_inits.append(ei.as_function());
            }
            ClassFieldDefinition field { move(elem.name), move(elem.initializer), move(dec_inits), move(extra_inits) };
            class_constructor->add_field(field);
        } else if (elem.kind == Bytecode::ClassElementDescriptor::Kind::AutoAccessor) {
            Vector<GC::Ref<FunctionObject>> dec_inits;
            if (elem.decorator_initializers) {
                for (auto& di : *elem.decorator_initializers)
                    dec_inits.append(di.as_function());
            }
            Vector<GC::Ref<FunctionObject>> extra_inits;
            if (elem.extra_initializers) {
                for (auto& ei : *elem.extra_initializers)
                    extra_inits.append(ei.as_function());
            }
            ClassFieldDefinition backing_field {
                ClassElementName(*elem.backing_storage_name),
                move(elem.initializer),
                move(dec_inits),
                move(extra_inits),
            };
            class_constructor->add_field(backing_field);
        }
        // Instance methods/accessors are already defined on prototype above.
    }

    // Install instance and static private methods.
    for (auto& private_method : instance_private_methods)
        class_constructor->add_private_method(private_method);

    for (auto& method : static_private_methods)
        TRY(class_constructor->private_method_or_accessor_add(move(method)));

    // Store instance method extra initializers on the constructor for use
    // during InitializeInstanceElements.
    for (auto& initializer : instance_method_extra_initializers) {
        VERIFY(initializer.is_function());
        class_constructor->add_instance_extra_initializer(initializer.as_function());
    }

    // Field/accessor extra initializers are now stored per-field on
    // ClassFieldDefinition::extra_initializers and run in define_field
    // after each specific field is defined, matching the spec's
    // InitializeFieldOrAccessor ordering.

    // Apply class decorators.
    auto class_decorator_pairs = extract_decorator_pairs(decorator_values, decorator_offset, blueprint.class_decorator_count);
    GC::ConservativeVector<Value> class_extra_initializers;

    // Class decorators are applied in reverse order (inner-to-outer).
    // Per spec, IsCallable(newDef) accepts any callable, not just
    // ECMAScriptFunctionObject.
    GC::Ptr<FunctionObject> current_class = class_constructor;
    for (ssize_t di = class_decorator_pairs.size() - 1; di >= 0; --di) {
        auto& dec_pair = class_decorator_pairs[di];
        auto decoration_finished = std::make_shared<bool>(false);
        auto context = create_class_decorator_context_object(vm, class_name, class_extra_initializers, decoration_finished);
        auto new_def = TRY(call(vm, dec_pair.decorator, dec_pair.receiver, current_class, context));
        *decoration_finished = true;
        if (new_def.is_function()) {
            current_class = &new_def.as_function();
        } else if (!new_def.is_undefined()) {
            return vm.throw_completion<TypeError>(ErrorType::DecoratorInvalidReturnValue);
        }
    }
    auto* result_class = current_class.ptr();

    vm.running_execution_context().lexical_environment = outer_environment;
    restore_environment.disarm();

    if (binding_name.has_value())
        MUST(class_environment->initialize_binding(vm, binding_name.value(), result_class, Environment::InitializeBindingHint::Normal));

    // Run static method extra initializers.
    for (auto& initializer : static_method_extra_initializers)
        TRY(call(vm, initializer.as_function(), result_class));

    // Initialize static fields/accessors + static blocks.
    for (auto& elem : static_elements_vec) {
        if (elem.kind == Bytecode::ClassElementDescriptor::Kind::StaticInitializer) {
            TRY(call(vm, elem.value.as_function(), result_class));
        } else if (elem.kind == Bytecode::ClassElementDescriptor::Kind::Field) {
            // Initialize the static field on the class.
            auto init_value = js_undefined();
            if (!elem.initializer.has<Empty>()) {
                if (auto const* init_val = elem.initializer.get_pointer<Value>())
                    init_value = *init_val;
                else
                    init_value = TRY(call(vm, *elem.initializer.get<GC::Ref<ECMAScriptFunctionObject>>(), result_class));
            }
            // Run decorator initializer chain.
            if (elem.decorator_initializers) {
                for (auto& di : *elem.decorator_initializers)
                    init_value = TRY(call(vm, di.as_function(), result_class, init_value));
            }
            // Define the field.
            TRY(elem.name.visit(
                [&](PropertyKey const& key) -> ThrowCompletionOr<void> {
                    TRY(result_class->create_data_property_or_throw(key, init_value));
                    return {};
                },
                [&](PrivateName const& priv) -> ThrowCompletionOr<void> {
                    return result_class->private_field_add(priv, init_value);
                }));
            // Run extra initializers.
            if (elem.extra_initializers) {
                for (auto& ei : *elem.extra_initializers)
                    TRY(call(vm, ei.as_function(), result_class));
            }
        } else if (elem.kind == Bytecode::ClassElementDescriptor::Kind::AutoAccessor) {
            // Initialize the backing storage on the class.
            auto init_value = js_undefined();
            if (!elem.initializer.has<Empty>()) {
                if (auto const* init_val = elem.initializer.get_pointer<Value>())
                    init_value = *init_val;
                else
                    init_value = TRY(call(vm, *elem.initializer.get<GC::Ref<ECMAScriptFunctionObject>>(), result_class));
            }
            if (elem.decorator_initializers) {
                for (auto& di : *elem.decorator_initializers)
                    init_value = TRY(call(vm, di.as_function(), result_class, init_value));
            }
            TRY(result_class->private_field_add(*elem.backing_storage_name, init_value));
            if (elem.extra_initializers) {
                for (auto& ei : *elem.extra_initializers)
                    TRY(call(vm, ei.as_function(), result_class));
            }
        }
    }

    // Run class extra initializers.
    for (auto& initializer : class_extra_initializers)
        TRY(call(vm, initializer.as_function(), result_class));

    if (blueprint.source_code)
        class_constructor->set_source_text_range(*blueprint.source_code, blueprint.source_text_offset, blueprint.source_text_length);

    return { result_class };
}

}
