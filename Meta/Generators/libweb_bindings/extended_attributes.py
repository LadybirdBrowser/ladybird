# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

from Generators.libweb_bindings.includes import GeneratedIncludes


def wrap_with_extended_attribute_exposure_checks(
    includes: GeneratedIncludes, extended_attributes: dict[str, str], text: str
) -> str:
    if "SecureContext" in extended_attributes:
        includes.add("LibWeb/Bindings/PrincipalHostDefined.h")
        text = text.replace("\n", "\n    ")
        text = f"""    if (HTML::is_secure_context(Bindings::principal_host_defined_environment_settings_object(realm))) {{
    {text}    }}
"""

    if extended_attributes.get("Exposed") == "Window":
        includes.add("AK/TypeCasts.h")
        includes.add("LibWeb/HTML/Window.h")
        text = text.replace("\n", "\n    ")
        text = f"""    if (is<HTML::Window>(realm.global_object())) {{
    {text}    }}
"""

    if "Experimental" in extended_attributes:
        includes.add("LibWeb/HTML/UniversalGlobalScope.h")
        text = text.replace("\n", "\n    ")
        text = f"""    if (HTML::UniversalGlobalScopeMixin::expose_experimental_interfaces()) {{
    {text}    }}
"""

    return text


def wrap_with_ce_reactions(includes: GeneratedIncludes, expression: str) -> str:
    includes.add("LibWeb/HTML/Scripting/SimilarOriginWindowAgent.h")
    includes.add("LibWeb/Bindings/MainThreadVM.h")
    return f"""[&]() -> decltype({expression}) {{
        // For [CEReactions]: https://html.spec.whatwg.org/multipage/custom-elements.html#cereactions

        // 1. Push a new element queue onto this object's relevant agent's custom element reactions stack.
        auto& reactions_stack = HTML::relevant_similar_origin_window_agent(*idl_object).custom_element_reactions_stack;
        reactions_stack.element_queue_stack.append({{}});

        // 2. Run the originally-specified steps for this construct, catching any exceptions. If the steps return a value, let value be the returned value. If they throw an exception, let exception be the thrown exception.
        auto value_or_exception = {expression};

        // 3. Let queue be the result of popping from this object's relevant agent's custom element reactions stack.
        // 4. Invoke custom element reactions in queue.
        auto queue = reactions_stack.element_queue_stack.take_last();
        Bindings::invoke_custom_element_reactions(queue);

        // 5. If an exception exception was thrown by the original steps, rethrow exception.
        if (value_or_exception.is_error())
            return value_or_exception.release_error();

        // 6. If a value value was returned from the original steps, return value.
        return value_or_exception.release_value();
    }}()"""
