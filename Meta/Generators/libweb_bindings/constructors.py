# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause


from typing import TextIO

from Generators.libweb_bindings import overload_resolution
from Generators.libweb_bindings.arguments import write_operation_parameter_conversions
from Generators.libweb_bindings.context import GenerationContext
from Generators.libweb_bindings.cpp_types import fully_qualified_name_for_interface
from Generators.libweb_bindings.cpp_types import idl_identifier_cpp_name
from Generators.libweb_bindings.includes import GeneratedIncludes
from Generators.libweb_bindings.operations import write_argument_count_check
from Generators.libweb_bindings.overload_resolution import parameter_list_length
from Utils.webidl_parser import Constructor
from Utils.webidl_parser import Interface


def write_constructor_function(
    out: TextIO,
    context: GenerationContext,
    includes: GeneratedIncludes,
    interface: Interface,
    constructor: Constructor,
    overload_index: int,
) -> None:
    out.write(
        f"""JS::ThrowCompletionOr<GC::Ref<JS::Object>> {interface.constructor_class}::construct{overload_index}([[maybe_unused]] InterfaceConstructor& constructor, [[maybe_unused]] JS::FunctionObject& new_target)
{{
    WebIDL::log_trace(constructor.vm(), "{interface.constructor_class}::construct{overload_index}");
"""
    )
    write_constructor_steps(out, context, includes, interface, constructor)
    out.write(
        """}

"""
    )


def write_constructor_overload_arbiter(
    out: TextIO, context: GenerationContext, includes: GeneratedIncludes, interface: Interface
) -> None:
    includes.add("AK/Optional.h")
    includes.add("AK/Vector.h")
    includes.add("LibWeb/WebIDL/OverloadResolution.h")

    out.write(
        """    auto& vm = constructor.vm();

"""
    )
    overload_resolution.write_overload_resolution_switch(out, context, interface, interface.constructors)
    out.write(
        """
    switch (chosen_overload_callable_id.value()) {
"""
    )
    for overload_index, _ in enumerate(interface.constructors):
        out.write(
            f"""    case {overload_index}:
        return construct{overload_index}(constructor, new_target);
"""
        )
    out.write(
        """    default:
        VERIFY_NOT_REACHED();
    }
"""
    )


# https://webidl.spec.whatwg.org/#overridden-constructor-steps
def write_constructor_steps(
    out: TextIO,
    context: GenerationContext,
    includes: GeneratedIncludes,
    interface: Interface,
    constructor: Constructor,
) -> None:
    if "HTMLConstructor" in constructor.extended_attributes:
        write_html_constructor_steps(out, interface, constructor, includes)
        return

    out.write(
        f"""    auto& vm = constructor.vm();
    [[maybe_unused]] auto& realm = *vm.current_realm();

    // To internally create a new object implementing the interface {interface.name}:

    // 3.2. Let prototype be ? Get(newTarget, "prototype").
    auto prototype = TRY(new_target.get(vm.names.prototype));

    // 3.3. If Type(prototype) is not Object, then:
    if (!prototype.is_object()) {{
        // 1. Let targetRealm be ? GetFunctionRealm(newTarget).
        auto* target_realm = TRY(JS::get_function_realm(vm, new_target));

        // 2. Set prototype to the interface prototype object for interface in targetRealm.
        VERIFY(target_realm);
        prototype = &Bindings::ensure_web_prototype<{interface.prototype_class}>(*target_realm, "{interface.namespaced_name}"_fly_string);
    }}

"""
    )
    write_argument_count_check(out, interface.name, parameter_list_length(constructor.parameters))
    write_operation_parameter_conversions(out, constructor.parameters, includes, context)
    arguments = ", ".join(idl_identifier_cpp_name(parameter) for parameter in constructor.parameters)
    if arguments:
        arguments = f", {arguments}"
    out.write(
        f"""    auto impl = TRY(throw_dom_exception_if_needed(vm, [&] {{ return {fully_qualified_name_for_interface(interface)}::construct_impl(realm{arguments}); }}));

    // 7. Set instance.[[Prototype]] to prototype.
    VERIFY(prototype.is_object());
    impl->set_prototype(&prototype.as_object());

    // FIXME: Steps 8...11. of the "internally create a new object implementing the interface {interface.name}" algorithm
    // (https://webidl.spec.whatwg.org/#js-platform-objects) are currently not handled, or are handled within {fully_qualified_name_for_interface(interface)}::construct_impl().

    return *impl;
"""
    )


# https://html.spec.whatwg.org/#htmlconstructor
def write_html_constructor_steps(
    out: TextIO, interface: Interface, constructor: Constructor, includes: GeneratedIncludes
) -> None:
    if constructor.parameters:
        raise RuntimeError(f"Unsupported [HTMLConstructor] with parameters on '{interface.name}'")

    includes.add("AK/Optional.h")
    includes.add("AK/String.h")
    includes.add("AK/TypeCasts.h")
    includes.add("LibGC/Ptr.h")
    includes.add("LibJS/Runtime/Error.h")
    includes.add("LibWeb/Bindings/MainThreadVM.h")
    includes.add("LibWeb/DOM/Document.h")
    includes.add("LibWeb/DOM/Element.h")
    includes.add("LibWeb/DOM/ElementFactory.h")
    includes.add("LibWeb/HTML/CustomElements/CustomElementDefinition.h")
    includes.add("LibWeb/HTML/CustomElements/CustomElementRegistry.h")
    includes.add("LibWeb/HTML/Scripting/SimilarOriginWindowAgent.h")
    includes.add("LibWeb/HTML/Window.h")
    includes.add("LibWeb/WebIDL/AbstractOperations.h")

    out.write(
        f"""    auto& vm = constructor.vm();
    auto& realm = *vm.current_realm();
    auto& window = as<HTML::Window>(HTML::current_global_object());

    // 1. If NewTarget is equal to the active function object, then throw a TypeError.
    if (&new_target == vm.active_function_object())
        return vm.throw_completion<JS::TypeError>("Cannot directly construct an HTML element, it must be inherited"sv);

    // 2. Let registry be null.
    GC::Ptr<HTML::CustomElementRegistry> registry;

    // 3. If the surrounding agent's active custom element constructor map[NewTarget] exists:
    auto& surrounding_agent = HTML::relevant_similar_origin_window_agent(window);
    if (auto registry_for_constructor = surrounding_agent.active_custom_element_constructor_map.get(GC::Ref {{ new_target }}); registry_for_constructor.has_value() && !registry_for_constructor->is_null()) {{
        // 1. Set registry to the surrounding agent's active custom element constructor map[NewTarget].
        registry = registry_for_constructor.value();

        // 2. Remove the surrounding agent's active custom element constructor map[NewTarget].
        surrounding_agent.active_custom_element_constructor_map.remove(GC::Ref {{ new_target }});
    }}
    // 4. Otherwise, set registry to current global object's associated Document's custom element registry.
    else {{
        registry = window.associated_document().custom_element_registry();
    }}

    // 5. Let definition be the item in registry's custom element definition set with constructor equal to NewTarget.
    //    If there is no such item, then throw a TypeError.
    auto definition = registry->get_definition_from_new_target(new_target);
    if (!definition)
        return vm.throw_completion<JS::TypeError>("There is no custom element definition assigned to the given constructor"sv);

    // 6. Let isValue be null.
    Optional<String> is_value;

    // 7. If definition's local name is equal to definition's name (i.e., definition is for an autonomous custom element):
    if (definition->local_name() == definition->name()) {{
        // 1. If the active function object is not HTMLElement, then throw a TypeError.
        {'return vm.throw_completion<JS::TypeError>("Autonomous custom elements can only inherit from HTMLElement"sv);' if interface.name != "HTMLElement" else ""}
    }}
    // 8. Otherwise (i.e., if definition is for a customized built-in element):
    else {{
        // 1. Let valid local names be the list of local names for elements defined in this specification or in other applicable specifications that use the active function object as their element interface.
        static auto const& valid_local_names = *new auto(MUST(DOM::valid_local_names_for_given_html_element_interface("{interface.name}"sv)));

        // 2. If valid local names does not contain definition's local name, then throw a TypeError.
        if (!valid_local_names.contains_slow(definition->local_name()))
            return vm.throw_completion<JS::TypeError>(MUST(String::formatted("Local name '{{}}' of customized built-in element is not a valid local name for {interface.name}", definition->local_name())));

        // 3. Set isValue to definition's name.
        is_value = definition->name();
    }}

    // 9. If definition's construction stack is empty:
    if (definition->construction_stack().is_empty()) {{
        // 1. Let element be the result of internally creating a new object implementing the interface to which the active function object corresponds, given the current Realm Record and NewTarget.
        // 2. Set element's node document to the current global object's associated Document.
        // 3. Set element's namespace to the HTML namespace.
        // 4. Set element's namespace prefix to null.
        // 5. Set element's local name to definition's local name.
        auto element = realm.create<{fully_qualified_name_for_interface(interface)}>(window.associated_document(), DOM::QualifiedName {{ definition->local_name(), {{}}, Namespace::HTML }});

        // https://webidl.spec.whatwg.org/#internally-create-a-new-object-implementing-the-interface
        TRY(WebIDL::set_prototype_from_new_target<{interface.prototype_class}>(vm, new_target, "{interface.namespaced_name}"_fly_string, *element));

        // 6. Set element's custom element registry to registry.
        element->set_custom_element_registry(registry);

        // 7. Set element's custom element state to "custom".
        // 8. Set element's custom element definition to definition.
        // 9. Set element's is value to isValue.
        element->setup_custom_element_from_constructor(*definition, is_value);

        // 10. Return element.
        return *element;
    }}

    // 10. Let prototype be ? Get(NewTarget, "prototype").
    auto prototype = TRY(new_target.get(vm.names.prototype));

    // 11. If Type(prototype) is not Object, then:
    if (!prototype.is_object()) {{
        // 1. Let realm be ? GetFunctionRealm(NewTarget).
        auto* function_realm = TRY(JS::get_function_realm(vm, new_target));

        // 2. Set prototype to the interface prototype object of realm whose interface is the same as the interface of the active function object.
        VERIFY(function_realm);
        prototype = &Bindings::ensure_web_prototype<{interface.prototype_class}>(*function_realm, "{interface.namespaced_name}"_fly_string);
    }}

    VERIFY(prototype.is_object());

    // 12. Let element be the last entry in definition's construction stack.
    auto& element = definition->construction_stack().last();

    // 13. If element is an already constructed marker, then throw a TypeError.
    if (element.has<HTML::AlreadyConstructedCustomElementMarker>())
        return vm.throw_completion<JS::TypeError>("Custom element has already been constructed"sv);

    // 14. Perform ? element.[[SetPrototypeOf]](prototype).
    auto actual_element = element.get<GC::Ref<DOM::Element>>();
    TRY(actual_element->internal_set_prototype_of(&prototype.as_object()));

    // 15. Replace the last entry in definition's construction stack with an already constructed marker.
    definition->construction_stack().last() = HTML::AlreadyConstructedCustomElementMarker {{}};

    // 16. Return element.
    return *actual_element;
"""
    )
