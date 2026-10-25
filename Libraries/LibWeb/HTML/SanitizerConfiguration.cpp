/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <AK/QuickSort.h>
#include <LibURL/Parser.h>
#include <LibWeb/Bindings/Sanitizer.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/GlobalEventHandlers.h>
#include <LibWeb/HTML/Sanitizer.h>
#include <LibWeb/HTML/SanitizerConfiguration.h>
#include <LibWeb/HTML/TagNames.h>
#include <LibWeb/HTML/WindowEventHandlers.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/TrustedTypes/TrustedHTMLParserOptions.h>

namespace Web::HTML {

template<typename T>
static constexpr bool is_string_type = IsSame<RemoveCVReference<T>, Utf16String>;

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#canonicalize-a-sanitizer-name
template<typename Name>
static SanitizerName canonicalize_a_sanitizer_name(Name const& name, Optional<Utf16FlyString> const& default_namespace)
{
    return name.visit(
        [&](Utf16String const& string) {
            // 1. If name is a DOMString, then return «[ "name" → name, "namespace" → defaultNamespace ]».
            return SanitizerName { Utf16FlyString { string }, default_namespace };
        },
        [&](auto const& dictionary) {
            // 2. Assert: name is a dictionary and both name["name"] and name["namespace"] exist.
            Optional<Utf16FlyString> namespace_;
            if (dictionary.namespace_.has_value())
                namespace_ = Utf16FlyString { *dictionary.namespace_ };

            // 3. If name["namespace"] is the empty string, then set it to null.
            if (namespace_.has_value() && namespace_->is_empty())
                namespace_ = {};

            // 4. Return «[ "name" → name["name"], "namespace" → name["namespace"] ]».
            return SanitizerName { Utf16FlyString { dictionary.name }, move(namespace_) };
        });
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#canonicalize-a-sanitizer-element
template<typename Element>
static SanitizerName canonicalize_a_sanitizer_element(Element const& element)
{
    // Return the result of canonicalizing element with the HTML namespace as the default namespace.
    return canonicalize_a_sanitizer_name(element, Namespace::HTML);
}

// NB: Attributes are canonicalized with the default namespace of null.
template<typename Attribute>
static SanitizerName canonicalize_a_sanitizer_attribute(Attribute const& attribute)
{
    return canonicalize_a_sanitizer_name(attribute, OptionalNone {});
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#canonicalize-a-sanitizer-element-list
template<typename List>
static Vector<SanitizerName> canonicalize_a_sanitizer_element_list(List const& list)
{
    // 1. Let newList be « ».
    Vector<SanitizerName> new_list;
    new_list.ensure_capacity(list.size());

    // 2. For each item in list, append the result of canonicalizing item to newList.
    for (auto const& item : list)
        new_list.unchecked_append(canonicalize_a_sanitizer_element(item));

    // 3. Return newList.
    return new_list;
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#canonicalize-a-sanitizer-list
template<typename List>
static Vector<SanitizerName> canonicalize_a_sanitizer_list(List const& list)
{
    // 1. Let newList be « ».
    Vector<SanitizerName> new_list;
    new_list.ensure_capacity(list.size());

    // 2. For each item in list, append the result of canonicalizing item to newList.
    for (auto const& item : list)
        new_list.unchecked_append(canonicalize_a_sanitizer_attribute(item));

    // 3. Return newList.
    return new_list;
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#canonicalize-a-processing-instruction-list
template<typename List>
static Vector<Utf16String> canonicalize_a_processing_instruction_list(List const& list)
{
    // 1. Let newList be « ».
    Vector<Utf16String> new_list;
    new_list.ensure_capacity(list.size());

    // 2. For each item in list, append the result of canonicalizing item to newList.
    for (auto const& item : list)
        new_list.unchecked_append(canonicalize_sanitizer_processing_instruction(item));

    // 3. Return newList.
    return new_list;
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#canonicalize-a-sanitizer-element-with-attributes
template<typename Element>
static SanitizerElementWithAttributes canonicalize_a_sanitizer_element_with_attributes(Element const& element)
{
    // 1. Let result be the result of canonicalizing element.
    SanitizerElementWithAttributes result { canonicalize_a_sanitizer_element(element), {}, {} };

    // 2. If element is a dictionary:
    if (auto const* dictionary = element.template get_pointer<Bindings::SanitizerElementNamespaceWithAttributes>()) {
        // 1. If element["attributes"] exists, then set result["attributes"] to the result of canonicalizing
        //    element["attributes"].
        if (dictionary->attributes.has_value())
            result.attributes = canonicalize_a_sanitizer_list(*dictionary->attributes);

        // 2. If element["removeAttributes"] exists, then set result["removeAttributes"] to the result of
        //    canonicalizing element["removeAttributes"].
        if (dictionary->remove_attributes.has_value())
            result.remove_attributes = canonicalize_a_sanitizer_list(*dictionary->remove_attributes);
    }

    // 3. If neither result["attributes"] nor result["removeAttributes"] exists, then set result["removeAttributes"]
    //    to an empty list.
    if (!result.attributes.has_value() && !result.remove_attributes.has_value())
        result.remove_attributes = Vector<SanitizerName> {};

    // 4. Return result.
    return result;
}

SanitizerName canonicalize_sanitizer_element(Variant<Utf16String, Bindings::SanitizerElementNamespace> const& element)
{
    return canonicalize_a_sanitizer_element(element);
}

SanitizerElementWithAttributes canonicalize_sanitizer_element_with_attributes(Variant<Utf16String, Bindings::SanitizerElementNamespaceWithAttributes> const& element)
{
    return canonicalize_a_sanitizer_element_with_attributes(element);
}

SanitizerName canonicalize_sanitizer_attribute(Variant<Utf16String, Bindings::SanitizerAttributeNamespace> const& attribute)
{
    return canonicalize_a_sanitizer_attribute(attribute);
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#canonicalize-a-processing-instruction
Utf16String canonicalize_sanitizer_processing_instruction(Variant<Utf16String, Bindings::SanitizerProcessingInstruction> const& pi)
{
    return pi.visit(
        // 1. If pi is a DOMString, then return «[ "target" → pi ]».
        [](Utf16String const& string) { return string; },
        // 2. Assert: pi is a dictionary and pi["target"] exists.
        // 3. Return «[ "target" → pi["target"] ]».
        [](Bindings::SanitizerProcessingInstruction const& dictionary) { return dictionary.target; });
}

// NB: This performs the per-item canonicalization steps of "canonicalize the configuration" on a dictionary supplied
//     by script. The remaining steps are performed by canonicalize_the_configuration().
// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#canonicalize-the-configuration
SanitizerConfiguration canonicalize_sanitizer_config(Bindings::SanitizerConfig const& configuration)
{
    SanitizerConfiguration result;

    // 4. If configuration["elements"] exists:
    if (configuration.elements.has_value()) {
        // 1. Let newElements be « ».
        Vector<SanitizerElementWithAttributes> new_elements;
        new_elements.ensure_capacity(configuration.elements->size());

        // 2. For each element of configuration["elements"], append the result of canonicalizing element to
        //    newElements.
        for (auto const& element : *configuration.elements)
            new_elements.unchecked_append(canonicalize_a_sanitizer_element_with_attributes(element));

        // 3. Set configuration["elements"] to newElements.
        result.elements = move(new_elements);
    }

    // 5. If configuration["removeElements"] exists, then set configuration["removeElements"] to the result of
    //    canonicalizing configuration["removeElements"].
    if (configuration.remove_elements.has_value())
        result.remove_elements = canonicalize_a_sanitizer_element_list(*configuration.remove_elements);

    // 6. If configuration["attributes"] exists, then set configuration["attributes"] to the result of canonicalizing
    //    configuration["attributes"].
    if (configuration.attributes.has_value())
        result.attributes = canonicalize_a_sanitizer_list(*configuration.attributes);

    // 7. If configuration["removeAttributes"] exists, then set configuration["removeAttributes"] to the result of
    //    canonicalizing configuration["removeAttributes"].
    if (configuration.remove_attributes.has_value())
        result.remove_attributes = canonicalize_a_sanitizer_list(*configuration.remove_attributes);

    // 8. If configuration["replaceWithChildrenElements"] exists, then set configuration["replaceWithChildrenElements"]
    //    to the result of canonicalizing configuration["replaceWithChildrenElements"].
    if (configuration.replace_with_children_elements.has_value())
        result.replace_with_children_elements = canonicalize_a_sanitizer_element_list(*configuration.replace_with_children_elements);

    // 9. If configuration["processingInstructions"] exists, then set configuration["processingInstructions"] to the
    //    result of canonicalizing configuration["processingInstructions"].
    if (configuration.processing_instructions.has_value())
        result.processing_instructions = canonicalize_a_processing_instruction_list(*configuration.processing_instructions);

    // 10. If configuration["removeProcessingInstructions"] exists, then set
    //     configuration["removeProcessingInstructions"] to the result of canonicalizing
    //     configuration["removeProcessingInstructions"].
    if (configuration.remove_processing_instructions.has_value())
        result.remove_processing_instructions = canonicalize_a_processing_instruction_list(*configuration.remove_processing_instructions);

    result.comments = configuration.comments;
    result.data_attributes = configuration.data_attributes;
    result.javascript_urls = configuration.javascript_ur_ls;

    return result;
}

// NB: This performs the steps of "canonicalize the configuration" that do not canonicalize individual items. Items of
//     a SanitizerConfiguration are always canonical, see canonicalize_sanitizer_config().
// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#canonicalize-the-configuration
void canonicalize_the_configuration(SanitizerConfiguration& configuration, PermissiveDefaults permissive_defaults)
{
    bool permissive = permissive_defaults == PermissiveDefaults::Yes;

    // 1. If neither configuration["elements"] nor configuration["removeElements"] exists, then set
    //    configuration["removeElements"] to an empty list.
    if (!configuration.elements.has_value() && !configuration.remove_elements.has_value())
        configuration.remove_elements = Vector<SanitizerName> {};

    // 2. If neither configuration["attributes"] nor configuration["removeAttributes"] exists, then set
    //    configuration["removeAttributes"] to an empty list.
    if (!configuration.attributes.has_value() && !configuration.remove_attributes.has_value())
        configuration.remove_attributes = Vector<SanitizerName> {};

    // 3. If neither configuration["processingInstructions"] nor configuration["removeProcessingInstructions"] exists:
    if (!configuration.processing_instructions.has_value() && !configuration.remove_processing_instructions.has_value()) {
        // 1. If permissiveDefaults is true, then set configuration["removeProcessingInstructions"] to an empty list.
        if (permissive)
            configuration.remove_processing_instructions = Vector<Utf16String> {};
        // 2. Otherwise, set configuration["processingInstructions"] to an empty list.
        else
            configuration.processing_instructions = Vector<Utf16String> {};
    }

    // NB: Steps 4 to 10 canonicalize individual items, which canonicalize_sanitizer_config() has done.

    // 11. If configuration["comments"] does not exist, then set it to permissiveDefaults.
    if (!configuration.comments.has_value())
        configuration.comments = permissive;

    // 12. If configuration["attributes"] exists and configuration["dataAttributes"] does not exist, then set it to
    //     permissiveDefaults.
    if (configuration.attributes.has_value() && !configuration.data_attributes.has_value())
        configuration.data_attributes = permissive;

    // 13. If configuration["javascriptURLs"] does not exist, then set it to permissiveDefaults.
    if (!configuration.javascript_urls.has_value())
        configuration.javascript_urls = permissive;
}

bool sanitizer_list_contains(Vector<SanitizerName> const& list, SanitizerName const& item)
{
    return list.contains_slow(item);
}

static bool sanitizer_list_contains(Optional<Vector<SanitizerName>> const& list, SanitizerName const& item)
{
    return list.has_value() && sanitizer_list_contains(*list, item);
}

// NB: Returns whether the list contained the item.
bool sanitizer_list_remove(Optional<Vector<SanitizerName>>& list, SanitizerName const& item)
{
    if (!list.has_value())
        return false;
    return list->remove_all_matching([&](auto const& entry) { return entry == item; });
}

static SanitizerElementWithAttributes const* find_element(Optional<Vector<SanitizerElementWithAttributes>> const& elements, Utf16FlyString const& local_name, Optional<Utf16FlyString> const& namespace_)
{
    if (!elements.has_value())
        return nullptr;
    for (auto const& element : *elements) {
        if (element.element.name == local_name && element.element.namespace_ == namespace_)
            return &element;
    }
    return nullptr;
}

static bool elements_contain(Optional<Vector<SanitizerElementWithAttributes>> const& elements, SanitizerName const& name)
{
    return find_element(elements, name.name, name.namespace_);
}

static bool elements_remove(Optional<Vector<SanitizerElementWithAttributes>>& elements, SanitizerName const& name)
{
    if (!elements.has_value())
        return false;
    return elements->remove_all_matching([&](auto const& entry) { return entry.element == name; });
}

// https://infra.spec.whatwg.org/#list-has-duplicates
static bool has_duplicates(Vector<SanitizerName> const& list)
{
    for (size_t i = 0; i < list.size(); ++i) {
        for (size_t j = i + 1; j < list.size(); ++j) {
            if (list[i] == list[j])
                return true;
        }
    }
    return false;
}

static bool has_duplicates(Vector<SanitizerElementWithAttributes> const& list)
{
    for (size_t i = 0; i < list.size(); ++i) {
        for (size_t j = i + 1; j < list.size(); ++j) {
            if (list[i].element == list[j].element)
                return true;
        }
    }
    return false;
}

static bool has_duplicates(Vector<Utf16String> const& list)
{
    for (size_t i = 0; i < list.size(); ++i) {
        for (size_t j = i + 1; j < list.size(); ++j) {
            if (list[i] == list[j])
                return true;
        }
    }
    return false;
}

// https://infra.spec.whatwg.org/#set-intersection
template<typename A, typename B>
static bool intersection_is_empty(A const& a, B const& b)
{
    for (auto const& item : a) {
        for (auto const& other : b) {
            if constexpr (IsSame<RemoveCVReference<decltype(item)>, SanitizerElementWithAttributes>) {
                if (item.element == other)
                    return false;
            } else {
                if (item == other)
                    return false;
            }
        }
    }
    return true;
}

// https://infra.spec.whatwg.org/#set-subset
static bool is_subset(Vector<SanitizerName> const& subset, Vector<SanitizerName> const& superset)
{
    for (auto const& item : subset) {
        if (!sanitizer_list_contains(superset, item))
            return false;
    }
    return true;
}

// https://html.spec.whatwg.org/multipage/dom.html#custom-data-attribute
bool is_custom_data_attribute(SanitizerName const& attribute)
{
    // A custom data attribute is an attribute in no namespace whose name starts with the string "data-", has at least
    // one character after the hyphen, is a valid attribute local name, and contains no ASCII upper alphas.
    if (attribute.namespace_.has_value())
        return false;
    auto name = attribute.name.view();
    if (!name.starts_with("data-"sv) || name.length_in_code_units() <= 5)
        return false;
    if (!DOM::is_valid_attribute_local_name(name))
        return false;
    for (size_t i = 0; i < name.length_in_code_units(); ++i) {
        if (is_ascii_upper_alpha(name.code_unit_at(i)))
            return false;
    }
    return true;
}

static bool contains_custom_data_attribute(Vector<SanitizerName> const& list)
{
    return list.contains([](auto const& attribute) { return is_custom_data_attribute(attribute); });
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-sanitizerconfig-valid
bool sanitizer_configuration_is_valid(SanitizerConfiguration const& config)
{
    // 1. Assert: config["elements"] exists or config["removeElements"] exists.
    VERIFY(config.elements.has_value() || config.remove_elements.has_value());

    // 2. If config["elements"] exists and config["removeElements"] exists, then return false.
    if (config.elements.has_value() && config.remove_elements.has_value())
        return false;

    // 3. Assert: Either config["processingInstructions"] exists or config["removeProcessingInstructions"] exists.
    VERIFY(config.processing_instructions.has_value() || config.remove_processing_instructions.has_value());

    // 4. If config["processingInstructions"] exists and config["removeProcessingInstructions"] exists, then return
    //    false.
    if (config.processing_instructions.has_value() && config.remove_processing_instructions.has_value())
        return false;

    // 5. Assert: Either config["attributes"] exists or config["removeAttributes"] exists.
    VERIFY(config.attributes.has_value() || config.remove_attributes.has_value());

    // 6. If config["attributes"] exists and config["removeAttributes"] exists, then return false.
    if (config.attributes.has_value() && config.remove_attributes.has_value())
        return false;

    // 7. Assert: All SanitizerElementNamespaceWithAttributes, SanitizerElementNamespace,
    //    SanitizerProcessingInstruction, and SanitizerAttributeNamespace items in config are canonical, meaning they
    //    have been run through canonicalizing, as appropriate.
    // NB: The items of a SanitizerConfiguration are canonical by construction.

    // 8. If config["elements"] exists:
    if (config.elements.has_value()) {
        // 1. If config["elements"] has duplicates, then return false.
        if (has_duplicates(*config.elements))
            return false;
    }
    // 9. Otherwise:
    else {
        // 1. If config["removeElements"] has duplicates, then return false.
        if (has_duplicates(*config.remove_elements))
            return false;
    }

    // 10. If config["replaceWithChildrenElements"] exists and has duplicates, then return false.
    if (config.replace_with_children_elements.has_value() && has_duplicates(*config.replace_with_children_elements))
        return false;

    // 11. If config["processingInstructions"] exists:
    if (config.processing_instructions.has_value()) {
        // 1. If config["processingInstructions"] has duplicates, then return false.
        if (has_duplicates(*config.processing_instructions))
            return false;
    }
    // 12. Otherwise:
    else {
        // 1. If config["removeProcessingInstructions"] has duplicates, then return false.
        if (has_duplicates(*config.remove_processing_instructions))
            return false;
    }

    // 13. If config["attributes"] exists:
    if (config.attributes.has_value()) {
        // 1. If config["attributes"] has duplicates, then return false.
        if (has_duplicates(*config.attributes))
            return false;
    }
    // 14. Otherwise:
    else {
        // 1. If config["removeAttributes"] has duplicates, then return false.
        if (has_duplicates(*config.remove_attributes))
            return false;
    }

    // 15. If config["replaceWithChildrenElements"] exists:
    if (config.replace_with_children_elements.has_value()) {
        // 1. For each element of config["replaceWithChildrenElements"]:
        for (auto const& element : *config.replace_with_children_elements) {
            // 1. If the built-in non-replaceable elements list contains element, then return false.
            if (built_in_non_replaceable_elements_list_contains(element))
                return false;
        }

        // 2. If config["elements"] exists:
        if (config.elements.has_value()) {
            // 1. If the intersection of config["elements"] and config["replaceWithChildrenElements"] is not empty, then
            //    return false.
            if (!intersection_is_empty(*config.elements, *config.replace_with_children_elements))
                return false;
        }
        // 3. Otherwise:
        else {
            // 1. If the intersection of config["removeElements"] and config["replaceWithChildrenElements"] is not
            //    empty, then return false.
            if (!intersection_is_empty(*config.remove_elements, *config.replace_with_children_elements))
                return false;
        }
    }

    // 16. If config["attributes"] exists:
    if (config.attributes.has_value()) {
        // 1. Assert: config["dataAttributes"] exists.
        VERIFY(config.data_attributes.has_value());

        // 2. If config["elements"] exists:
        if (config.elements.has_value()) {
            // 1. For each element of config["elements"]:
            for (auto const& element : *config.elements) {
                // 1. If element["attributes"] exists and element["attributes"] has duplicates, then return false.
                if (element.attributes.has_value() && has_duplicates(*element.attributes))
                    return false;

                // 2. If element["removeAttributes"] exists and element["removeAttributes"] has duplicates, then return
                //    false.
                if (element.remove_attributes.has_value() && has_duplicates(*element.remove_attributes))
                    return false;

                // 3. If the intersection of config["attributes"] and element["attributes"] with default « » is not
                //    empty, then return false.
                if (element.attributes.has_value() && !intersection_is_empty(*config.attributes, *element.attributes))
                    return false;

                // 4. If element["removeAttributes"] with default « » is not a subset of config["attributes"], then
                //    return false.
                if (element.remove_attributes.has_value() && !is_subset(*element.remove_attributes, *config.attributes))
                    return false;

                // 5. If config["dataAttributes"] is true and element["attributes"] contains a custom data attribute,
                //    then return false.
                if (*config.data_attributes && element.attributes.has_value() && contains_custom_data_attribute(*element.attributes))
                    return false;
            }
        }

        // 3. If config["dataAttributes"] is true and config["attributes"] contains a custom data attribute, then return
        //    false.
        if (*config.data_attributes && contains_custom_data_attribute(*config.attributes))
            return false;
    }
    // 17. Otherwise:
    else {
        // 1. If config["elements"] exists:
        if (config.elements.has_value()) {
            // 1. For each element of config["elements"]:
            for (auto const& element : *config.elements) {
                // 1. If element["attributes"] exists and element["removeAttributes"] exists, then return false.
                if (element.attributes.has_value() && element.remove_attributes.has_value())
                    return false;

                // 2. If element["attributes"] exists and element["attributes"] has duplicates, then return false.
                if (element.attributes.has_value() && has_duplicates(*element.attributes))
                    return false;

                // 3. If element["removeAttributes"] exists and element["removeAttributes"] has duplicates, then return
                //    false.
                if (element.remove_attributes.has_value() && has_duplicates(*element.remove_attributes))
                    return false;

                // 4. If the intersection of config["removeAttributes"] and element["attributes"] with default « » is
                //    not empty, then return false.
                if (element.attributes.has_value() && !intersection_is_empty(*config.remove_attributes, *element.attributes))
                    return false;

                // 5. If the intersection of config["removeAttributes"] and element["removeAttributes"] with default « »
                //    is not empty, then return false.
                if (element.remove_attributes.has_value() && !intersection_is_empty(*config.remove_attributes, *element.remove_attributes))
                    return false;
            }
        }

        // 2. If config["dataAttributes"] exists, then return false.
        if (config.data_attributes.has_value())
            return false;
    }

    // 18. Return true.
    return true;
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#configure-a-sanitizer
WebIDL::ExceptionOr<SanitizerConfiguration> configure_sanitizer_configuration(SanitizerConfiguration fresh_configuration, PermissiveDefaults permissive_defaults)
{
    // 1. Let freshConfiguration be a clone of configuration.
    // NB: The caller passes a clone.

    // 2. Canonicalize the configuration freshConfiguration with permissiveDefaults.
    canonicalize_the_configuration(fresh_configuration, permissive_defaults);

    // 3. If freshConfiguration is not valid, then throw a TypeError.
    if (!sanitizer_configuration_is_valid(fresh_configuration))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Sanitizer configuration is not valid"_utf16 };

    // 4. Set sanitizer's configuration to freshConfiguration.
    // NB: The caller stores the result.
    return fresh_configuration;
}

static Bindings::SanitizerElementNamespace to_bindings_name(SanitizerName const& name)
{
    Bindings::SanitizerElementNamespace result;
    result.name = name.name.to_utf16_string();
    result.namespace_ = name.namespace_.map([](auto const& namespace_) { return namespace_.to_utf16_string(); });
    return result;
}

static Vector<Variant<Utf16String, Bindings::SanitizerAttributeNamespace>> to_bindings_attribute_list(Vector<SanitizerName> const& list)
{
    Vector<Variant<Utf16String, Bindings::SanitizerAttributeNamespace>> result;
    result.ensure_capacity(list.size());
    for (auto const& item : list) {
        Bindings::SanitizerAttributeNamespace attribute;
        attribute.name = item.name.to_utf16_string();
        attribute.namespace_ = item.namespace_.map([](auto const& namespace_) { return namespace_.to_utf16_string(); });
        result.unchecked_append(move(attribute));
    }
    return result;
}

static Vector<Variant<Utf16String, Bindings::SanitizerElementNamespace>> to_bindings_element_list(Vector<SanitizerName> const& list)
{
    Vector<Variant<Utf16String, Bindings::SanitizerElementNamespace>> result;
    result.ensure_capacity(list.size());
    for (auto const& item : list)
        result.unchecked_append(to_bindings_name(item));
    return result;
}

static Vector<Variant<Utf16String, Bindings::SanitizerProcessingInstruction>> to_bindings_processing_instruction_list(Vector<Utf16String> const& list)
{
    Vector<Variant<Utf16String, Bindings::SanitizerProcessingInstruction>> result;
    result.ensure_capacity(list.size());
    for (auto const& item : list) {
        Bindings::SanitizerProcessingInstruction processing_instruction;
        processing_instruction.target = item;
        result.unchecked_append(move(processing_instruction));
    }
    return result;
}

Bindings::SanitizerConfig sanitizer_configuration_to_bindings(SanitizerConfiguration const& configuration)
{
    Bindings::SanitizerConfig result;
    if (configuration.elements.has_value()) {
        Vector<Variant<Utf16String, Bindings::SanitizerElementNamespaceWithAttributes>> elements;
        elements.ensure_capacity(configuration.elements->size());
        for (auto const& element : *configuration.elements) {
            Bindings::SanitizerElementNamespaceWithAttributes dictionary;
            dictionary.name = element.element.name.to_utf16_string();
            dictionary.namespace_ = element.element.namespace_.map([](auto const& namespace_) { return namespace_.to_utf16_string(); });
            if (element.attributes.has_value())
                dictionary.attributes = to_bindings_attribute_list(*element.attributes);
            if (element.remove_attributes.has_value())
                dictionary.remove_attributes = to_bindings_attribute_list(*element.remove_attributes);
            elements.unchecked_append(move(dictionary));
        }
        result.elements = move(elements);
    }
    if (configuration.remove_elements.has_value())
        result.remove_elements = to_bindings_element_list(*configuration.remove_elements);
    if (configuration.replace_with_children_elements.has_value())
        result.replace_with_children_elements = to_bindings_element_list(*configuration.replace_with_children_elements);
    if (configuration.processing_instructions.has_value())
        result.processing_instructions = to_bindings_processing_instruction_list(*configuration.processing_instructions);
    if (configuration.remove_processing_instructions.has_value())
        result.remove_processing_instructions = to_bindings_processing_instruction_list(*configuration.remove_processing_instructions);
    if (configuration.attributes.has_value())
        result.attributes = to_bindings_attribute_list(*configuration.attributes);
    if (configuration.remove_attributes.has_value())
        result.remove_attributes = to_bindings_attribute_list(*configuration.remove_attributes);
    result.comments = configuration.comments;
    result.data_attributes = configuration.data_attributes;
    result.javascript_ur_ls = configuration.javascript_urls;
    return result;
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#remove-an-element-from-sanitizer-config
bool remove_an_element_from_sanitizer_config(SanitizerConfiguration& configuration, SanitizerName const& element)
{
    // 1. Assert: configuration is valid.
    // 2. Set element to the result of canonicalizing element.
    // NB: element is canonical.

    // 3. Let modified be the result of removing element from configuration["replaceWithChildrenElements"].
    bool modified = sanitizer_list_remove(configuration.replace_with_children_elements, element);

    // 4. If configuration["elements"] exists:
    if (configuration.elements.has_value()) {
        // 1. If configuration["elements"] contains element:
        if (elements_contain(configuration.elements, element)) {
            // 1. Remove element from configuration["elements"].
            elements_remove(configuration.elements, element);

            // 2. Return true.
            return true;
        }

        // 2. Return modified.
        return modified;
    }

    // 5. Otherwise:
    // 1. If configuration["removeElements"] contains element, then return modified.
    if (sanitizer_list_contains(*configuration.remove_elements, element))
        return modified;

    // 2. Append element to configuration["removeElements"].
    configuration.remove_elements->append(element);

    // 3. Return true.
    return true;
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#remove-an-attribute-from-sanitizer-config
bool remove_an_attribute_from_sanitizer_config(SanitizerConfiguration& configuration, SanitizerName const& attribute)
{
    // 1. Assert: configuration is valid.
    // 2. Set attribute to the result of canonicalizing attribute.
    // NB: attribute is canonical.

    // 3. If configuration["attributes"] exists:
    if (configuration.attributes.has_value()) {
        // 1. Let modified be the result of removing attribute from configuration["attributes"].
        bool modified = sanitizer_list_remove(configuration.attributes, attribute);

        // 2. If configuration["elements"] exists:
        if (configuration.elements.has_value()) {
            // 1. For each element of configuration["elements"]:
            for (auto& element : *configuration.elements) {
                // 1. If element["attributes"] with default « » contains attribute:
                if (sanitizer_list_contains(element.attributes, attribute)) {
                    // 1. Set modified to true.
                    modified = true;

                    // 2. Remove attribute from element["attributes"].
                    sanitizer_list_remove(element.attributes, attribute);
                }

                // 2. If element["removeAttributes"] with default « » contains attribute:
                if (sanitizer_list_contains(element.remove_attributes, attribute)) {
                    // 1. Assert: modified is true.
                    VERIFY(modified);

                    // 2. Remove attribute from element["removeAttributes"].
                    sanitizer_list_remove(element.remove_attributes, attribute);
                }
            }
        }

        // 3. Return modified.
        return modified;
    }

    // 4. Otherwise:
    // 1. If configuration["removeAttributes"] contains attribute, then return false.
    if (sanitizer_list_contains(*configuration.remove_attributes, attribute))
        return false;

    // 2. If configuration["elements"] exists:
    if (configuration.elements.has_value()) {
        // 1. For each element in configuration["elements"]:
        for (auto& element : *configuration.elements) {
            // 1. If element["attributes"] with default « » contains attribute, then remove attribute from
            //    element["attributes"].
            sanitizer_list_remove(element.attributes, attribute);

            // 2. If element["removeAttributes"] with default « » contains attribute, then remove attribute from
            //    element["removeAttributes"].
            sanitizer_list_remove(element.remove_attributes, attribute);
        }
    }

    // 3. Append attribute to configuration["removeAttributes"].
    configuration.remove_attributes->append(attribute);

    // 4. Return true.
    return true;
}

static Vector<SanitizerName> const& event_handler_content_attributes()
{
    static NeverDestroyed<Vector<SanitizerName> const> attributes { [] {
        Vector<SanitizerName> attributes;
#define __ENUMERATE(attribute_name, event_name) \
    attributes.append({ HTML::AttributeNames::attribute_name, {} });
        ENUMERATE_GLOBAL_EVENT_HANDLERS(__ENUMERATE)
        ENUMERATE_WINDOW_EVENT_HANDLERS(__ENUMERATE)
#undef __ENUMERATE
        attributes.append({ "onbegin"_utf16_fly_string, {} });
        attributes.append({ "onend"_utf16_fly_string, {} });
        attributes.append({ "onrepeat"_utf16_fly_string, {} });
        return attributes;
    }() };
    return *attributes;
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#remove-unsafe
bool remove_unsafe_from_sanitizer_config(SanitizerConfiguration& configuration)
{
    // 1. Assert: configuration is valid.

    // 2. Let result be false.
    bool result = false;

    // 3. For each element in built-in safe baseline configuration["removeElements"]:
    for (auto const& element : *built_in_safe_baseline_configuration().remove_elements) {
        // 1. If removing element from configuration is true, then set result to true.
        if (remove_an_element_from_sanitizer_config(configuration, element))
            result = true;
    }

    // 4. For each attribute in built-in safe baseline configuration["removeAttributes"]:
    for (auto const& attribute : *built_in_safe_baseline_configuration().remove_attributes) {
        // 1. If removing attribute from configuration is true, then set result to true.
        if (remove_an_attribute_from_sanitizer_config(configuration, attribute))
            result = true;
    }

    // 5. For each attribute that is an event handler content attribute:
    for (auto const& attribute : event_handler_content_attributes()) {
        // 1. If removing attribute from configuration is true, then set result to true.
        if (remove_an_attribute_from_sanitizer_config(configuration, attribute))
            result = true;
    }

    // 6. If configuration["javascriptURLs"] is true:
    if (configuration.javascript_urls == true) {
        // 1. Set result to true.
        result = true;

        // 2. Set configuration["javascriptURLs"] to false.
        configuration.javascript_urls = false;
    }

    // 7. Return result.
    return result;
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#sanitizer-config-allows-comments
bool sanitizer_config_allows_comments(SanitizerConfiguration const& configuration)
{
    // Return true if configuration["comments"] is true; otherwise false.
    return configuration.comments == true;
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#sanitizer-config-allows-processing-instruction-target
bool sanitizer_config_allows_processing_instruction_target(Utf16View const& pi_target, SanitizerConfiguration const& configuration)
{
    // 1. Let pi be «[ "target" → piTarget ]».
    auto contains = [&](Vector<Utf16String> const& list) {
        return list.contains([&](auto const& target) { return target.utf16_view() == pi_target; });
    };

    // 2. If configuration["processingInstructions"] exists, then return whether
    //    configuration["processingInstructions"] contains pi.
    if (configuration.processing_instructions.has_value())
        return contains(*configuration.processing_instructions);

    // 3. If configuration["removeProcessingInstructions"] exists and configuration["removeProcessingInstructions"]
    //    contains pi, then return false.
    if (configuration.remove_processing_instructions.has_value() && contains(*configuration.remove_processing_instructions))
        return false;

    // 4. Return true.
    return true;
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#get-the-sanitizer-action-for-an-element-name
SanitizerAction sanitizer_action_for_element_name(Utf16FlyString const& local_name, Optional<Utf16FlyString> const& namespace_, SanitizerConfiguration const& configuration)
{
    // 1. Let elementName be «[ "name" → localName, "namespace" → namespace ]».
    SanitizerName element_name { local_name, namespace_ };

    // 2. If configuration["replaceWithChildrenElements"] exists and configuration["replaceWithChildrenElements"]
    //    contains elementName, then return "Replace with children".
    if (sanitizer_list_contains(configuration.replace_with_children_elements, element_name))
        return SanitizerAction::ReplaceWithChildren;

    // 3. If configuration["elements"] exists:
    if (configuration.elements.has_value()) {
        // 1. If there is no item in configuration["elements"] whose name member is localName and whose namespace
        //    member is namespace, then return "Remove".
        if (!find_element(configuration.elements, local_name, namespace_))
            return SanitizerAction::Remove;
    }
    // 4. Otherwise, if configuration["removeElements"] contains elementName, then return "Remove".
    else if (sanitizer_list_contains(configuration.remove_elements, element_name)) {
        return SanitizerAction::Remove;
    }

    // 5. Return "Keep".
    return SanitizerAction::Keep;
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#contains-a-javascript:-url
static bool contains_a_javascript_url(Utf16View const& value)
{
    // 1. Let url be the result of running the basic URL parser on value.
    auto url = URL::Parser::basic_parse(value);

    // 2. If url is failure, then return false.
    if (!url.has_value())
        return false;

    // 3. Return true if url's scheme is "javascript", and false otherwise.
    return url->scheme() == "javascript"sv;
}

struct NavigatingURLAttribute {
    Utf16FlyString const& element_name;
    Utf16FlyString const& element_namespace;
    Utf16FlyString const& attribute_name;
    Utf16FlyString const* attribute_namespace;
};

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#built-in-navigating-url-attributes-list
static bool built_in_navigating_url_attributes_list_matches(SanitizerName const& element_name, SanitizerName const& attribute_name)
{
    // The built-in navigating URL attributes list corresponds to all HTML elements marked with navigating URL
    // attributes in their normative definitions, as well as the element-attribute pairs represented in the following
    // table.
    static NavigatingURLAttribute const list[] = {
        { HTML::TagNames::a, Namespace::HTML, HTML::AttributeNames::href, nullptr },
        { HTML::TagNames::area, Namespace::HTML, HTML::AttributeNames::href, nullptr },
        { HTML::TagNames::form, Namespace::HTML, HTML::AttributeNames::action, nullptr },
        { HTML::TagNames::input, Namespace::HTML, HTML::AttributeNames::formaction, nullptr },
        { HTML::TagNames::button, Namespace::HTML, HTML::AttributeNames::formaction, nullptr },
        { HTML::TagNames::a, Namespace::SVG, HTML::AttributeNames::href, nullptr },
        { HTML::TagNames::a, Namespace::SVG, HTML::AttributeNames::href, &Namespace::XLink },
    };
    for (auto const& entry : list) {
        if (element_name.name != entry.element_name || element_name.namespace_ != entry.element_namespace)
            continue;
        if (attribute_name.name != entry.attribute_name)
            continue;
        if (entry.attribute_namespace) {
            if (attribute_name.namespace_ == *entry.attribute_namespace)
                return true;
        } else if (!attribute_name.namespace_.has_value()) {
            return true;
        }
    }
    return false;
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#built-in-animating-url-attributes-list
static bool built_in_animating_url_attributes_list_contains(SanitizerName const& element_name, SanitizerName const& attribute_name)
{
    if (element_name.namespace_ != Namespace::SVG || attribute_name.namespace_.has_value())
        return false;
    if (attribute_name.name != "attributeName"_utf16_fly_string)
        return false;
    return element_name.name.is_one_of("animate"_utf16_fly_string, "animateTransform"_utf16_fly_string, "set"_utf16_fly_string);
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#sanitizer-config-allows-an-attribute
bool sanitizer_config_allows_attribute(Utf16FlyString const& local_name, Optional<Utf16FlyString> const& namespace_, Utf16View const& value, Utf16FlyString const& element_local_name, Optional<Utf16FlyString> const& element_namespace, SanitizerConfiguration const& configuration)
{
    // 1. Let attrName be «[ "name" → localName, "namespace" → namespace ]».
    SanitizerName attribute_name { local_name, namespace_ };

    // 2. Let elementName be «[ "name" → elementLocalName, "namespace" → elementNamespace ]».
    SanitizerName element_name { element_local_name, element_namespace };

    // 3. Let elementWithLocalAttributes be «[ ]».
    // 4. If configuration["elements"] exists and there is an item element in configuration["elements"] whose name
    //    member is elementLocalName and whose namespace member is elementNamespace, then set
    //    elementWithLocalAttributes to element.
    auto const* element_with_local_attributes = find_element(configuration.elements, element_local_name, element_namespace);
    Optional<Vector<SanitizerName>> const no_attributes;
    auto const& local_attributes = element_with_local_attributes ? element_with_local_attributes->attributes : no_attributes;
    auto const& local_remove_attributes = element_with_local_attributes ? element_with_local_attributes->remove_attributes : no_attributes;

    // 5. If elementWithLocalAttributes["removeAttributes"] exists and elementWithLocalAttributes["removeAttributes"]
    //    contains attrName, then return false.
    if (sanitizer_list_contains(local_remove_attributes, attribute_name))
        return false;

    // 6. Otherwise, if configuration["attributes"] exists:
    if (configuration.attributes.has_value()) {
        // 1. If configuration["attributes"] does not contain attrName and elementWithLocalAttributes["attributes"]
        //    with default « » does not contain attrName, and if "data-" is not a code unit prefix of localName or
        //    namespace is not null or configuration["dataAttributes"] is not true, then return false.
        if (!sanitizer_list_contains(*configuration.attributes, attribute_name)
            && !sanitizer_list_contains(local_attributes, attribute_name)
            && (!local_name.view().starts_with("data-"sv) || namespace_.has_value() || configuration.data_attributes != true)) {
            return false;
        }
    }
    // 7. Otherwise:
    else {
        // 1. If elementWithLocalAttributes["attributes"] exists and elementWithLocalAttributes["attributes"] does not
        //    contain attrName, then return false.
        if (local_attributes.has_value() && !sanitizer_list_contains(*local_attributes, attribute_name))
            return false;

        // 2. Otherwise, if configuration["removeAttributes"] exists and configuration["removeAttributes"] contains
        //    attrName, then return false.
        if (sanitizer_list_contains(configuration.remove_attributes, attribute_name))
            return false;
    }

    // 8. If configuration["javascriptURLs"] is true, then return true.
    if (configuration.javascript_urls == true)
        return true;

    // 9. If the pair (elementName, attrName) matches an entry in the built-in navigating URL attributes list, and if
    //    value contains a javascript: URL, then return false.
    if (built_in_navigating_url_attributes_list_matches(element_name, attribute_name) && contains_a_javascript_url(value))
        return false;

    // 10. If elementNamespace is the MathML namespace, localName is "href", namespace is null or the XLink namespace,
    //     and value contains a javascript: URL, then return false.
    if (element_namespace == Namespace::MathML && local_name == HTML::AttributeNames::href
        && (!namespace_.has_value() || namespace_ == Namespace::XLink)
        && contains_a_javascript_url(value)) {
        return false;
    }

    // 11. If the built-in animating URL attributes list contains the pair (elementName, attrName), and value is "href"
    //     or "xlink:href", then return false.
    if (built_in_animating_url_attributes_list_contains(element_name, attribute_name) && (value == "href"sv || value == "xlink:href"sv))
        return false;

    // 12. Return true.
    return true;
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#built-in-non-replaceable-elements-list
bool built_in_non_replaceable_elements_list_contains(SanitizerName const& element)
{
    return (element.name == HTML::TagNames::html && element.namespace_ == Namespace::HTML)
        || (element.name == "svg"_utf16_fly_string && element.namespace_ == Namespace::SVG)
        || (element.name == "math"_utf16_fly_string && element.namespace_ == Namespace::MathML);
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#built-in-safe-baseline-configuration
SanitizerConfiguration const& built_in_safe_baseline_configuration()
{
    // The built-in safe baseline configuration is a SanitizerConfig. Its removeElements list consists of all HTML
    // elements normatively marked as Unsafe within their individual definitions, along with the obsolete frame
    // element, and the SVG script and SVG use elements, and its removeAttributes list is empty.
    static NeverDestroyed<SanitizerConfiguration const> configuration { [] {
        SanitizerConfiguration configuration;
        configuration.remove_elements = Vector<SanitizerName> {
            { HTML::TagNames::base, Namespace::HTML },
            { HTML::TagNames::embed, Namespace::HTML },
            { HTML::TagNames::frame, Namespace::HTML },
            { HTML::TagNames::iframe, Namespace::HTML },
            { HTML::TagNames::object, Namespace::HTML },
            { HTML::TagNames::script, Namespace::HTML },
            { HTML::TagNames::script, Namespace::SVG },
            { "use"_utf16_fly_string, Namespace::SVG },
        };
        configuration.remove_attributes = Vector<SanitizerName> {};
        return configuration;
    }() };
    return *configuration;
}

struct DefaultElement {
    StringView name;
    StringView attributes;
};

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#built-in-safe-default-configuration
SanitizerConfiguration const& built_in_safe_default_configuration()
{
    static NeverDestroyed<SanitizerConfiguration const> configuration { [] {
        auto attribute_list = [](StringView attributes) {
            Vector<SanitizerName> list;
            attributes.for_each_split_view(' ', SplitBehavior::Nothing, [&](StringView attribute) {
                list.append({ Utf16FlyString::from_utf8(attribute), {} });
            });
            return list;
        };

        SanitizerConfiguration configuration;

        // elements
        //     All HTML elements normatively categorized as Default within their individual definitions, alongside the
        //     MathML and SVG elements listed in the table below. Attributes included in those definitions are included
        //     in each element's respective attributes list.
        static constexpr DefaultElement html_elements[] = {
            { "html"sv, {} },
            { "head"sv, {} },
            { "title"sv, {} },
            { "body"sv, {} },
            { "article"sv, {} },
            { "section"sv, {} },
            { "nav"sv, {} },
            { "aside"sv, {} },
            { "h1"sv, {} },
            { "h2"sv, {} },
            { "h3"sv, {} },
            { "h4"sv, {} },
            { "h5"sv, {} },
            { "h6"sv, {} },
            { "hgroup"sv, {} },
            { "header"sv, {} },
            { "footer"sv, {} },
            { "address"sv, {} },
            { "p"sv, {} },
            { "hr"sv, {} },
            { "pre"sv, {} },
            { "blockquote"sv, "cite"sv },
            { "ol"sv, "reversed start type"sv },
            { "ul"sv, {} },
            { "menu"sv, {} },
            { "li"sv, "value"sv },
            { "dl"sv, {} },
            { "dt"sv, {} },
            { "dd"sv, {} },
            { "figure"sv, {} },
            { "figcaption"sv, {} },
            { "main"sv, {} },
            { "search"sv, {} },
            { "div"sv, {} },
            { "a"sv, "href hreflang type"sv },
            { "em"sv, {} },
            { "strong"sv, {} },
            { "small"sv, {} },
            { "s"sv, {} },
            { "cite"sv, {} },
            { "q"sv, {} },
            { "dfn"sv, {} },
            { "abbr"sv, {} },
            { "ruby"sv, {} },
            { "rt"sv, {} },
            { "rp"sv, {} },
            { "data"sv, "value"sv },
            { "time"sv, "datetime"sv },
            { "code"sv, {} },
            { "var"sv, {} },
            { "samp"sv, {} },
            { "kbd"sv, {} },
            { "sub"sv, {} },
            { "sup"sv, {} },
            { "i"sv, {} },
            { "b"sv, {} },
            { "u"sv, {} },
            { "mark"sv, {} },
            { "bdi"sv, {} },
            { "bdo"sv, {} },
            { "span"sv, {} },
            { "br"sv, {} },
            { "wbr"sv, {} },
            { "ins"sv, "cite datetime"sv },
            { "del"sv, "cite datetime"sv },
            { "table"sv, {} },
            { "caption"sv, {} },
            { "colgroup"sv, "span"sv },
            { "col"sv, "span"sv },
            { "tbody"sv, {} },
            { "thead"sv, {} },
            { "tfoot"sv, {} },
            { "tr"sv, {} },
            { "td"sv, "colspan headers rowspan"sv },
            { "th"sv, "abbr colspan headers rowspan scope"sv },
        };

        static constexpr DefaultElement mathml_elements[] = {
            { "a"sv, "href hreflang type"sv },
            { "math"sv, {} },
            { "merror"sv, {} },
            { "mfrac"sv, {} },
            { "mi"sv, {} },
            { "mmultiscripts"sv, {} },
            { "mn"sv, {} },
            { "mo"sv, "fence form largeop lspace maxsize minsize movablelimits rspace separator stretchy symmetric"sv },
            { "mover"sv, "accent"sv },
            { "mpadded"sv, "depth height lspace voffset width"sv },
            { "mphantom"sv, {} },
            { "mprescripts"sv, {} },
            { "mroot"sv, {} },
            { "mrow"sv, {} },
            { "ms"sv, {} },
            { "mspace"sv, "depth height width"sv },
            { "msqrt"sv, {} },
            { "mstyle"sv, {} },
            { "msub"sv, {} },
            { "msubsup"sv, {} },
            { "msup"sv, {} },
            { "mtable"sv, {} },
            { "mtd"sv, "columnspan rowspan"sv },
            { "mtext"sv, {} },
            { "mtr"sv, {} },
            { "munder"sv, "accentunder"sv },
            { "munderover"sv, "accent accentunder"sv },
            { "semantics"sv, {} },
        };

        static constexpr DefaultElement svg_elements[] = {
            { "a"sv, "href hreflang type"sv },
            { "circle"sv, "cx cy pathLength r"sv },
            { "defs"sv, {} },
            { "desc"sv, {} },
            { "ellipse"sv, "cx cy pathLength rx ry"sv },
            { "foreignObject"sv, "height width x y"sv },
            { "g"sv, {} },
            { "line"sv, "pathLength x1 x2 y1 y2"sv },
            { "marker"sv, "markerHeight markerUnits markerWidth orient preserveAspectRatio refX refY viewBox"sv },
            { "metadata"sv, {} },
            { "path"sv, "d pathLength"sv },
            { "polygon"sv, "pathLength points"sv },
            { "polyline"sv, "pathLength points"sv },
            { "rect"sv, "height pathLength rx ry width x y"sv },
            { "svg"sv, "height preserveAspectRatio viewBox width x y"sv },
            { "text"sv, "dx dy lengthAdjust rotate textLength x y"sv },
            { "textPath"sv, "lengthAdjust method path side spacing startOffset textLength"sv },
            { "title"sv, {} },
            { "tspan"sv, "dx dy lengthAdjust rotate textLength x y"sv },
        };

        Vector<SanitizerElementWithAttributes> elements;
        auto append_elements = [&](ReadonlySpan<DefaultElement> list, Utf16FlyString const& namespace_) {
            for (auto const& element : list)
                elements.append({ { Utf16FlyString::from_utf8(element.name), namespace_ }, attribute_list(element.attributes), {} });
        };
        append_elements(html_elements, Namespace::HTML);
        append_elements(mathml_elements, Namespace::MathML);
        append_elements(svg_elements, Namespace::SVG);
        configuration.elements = move(elements);

        // processingInstructions
        //     « »
        configuration.processing_instructions = Vector<Utf16String> {};

        // attributes
        configuration.attributes = attribute_list(
            "dir lang title alignment-baseline baseline-shift clip-path clip-rule color color-interpolation cursor "
            "direction display displaystyle dominant-baseline fill fill-opacity fill-rule font-family font-size "
            "font-size-adjust font-stretch font-style font-variant font-weight letter-spacing marker-end marker-mid "
            "marker-start mathbackground mathcolor mathsize opacity paint-order pointer-events scriptlevel "
            "shape-rendering stop-color stop-opacity stroke stroke-dasharray stroke-dashoffset stroke-linecap "
            "stroke-linejoin stroke-miterlimit stroke-opacity stroke-width text-anchor text-decoration text-overflow "
            "text-rendering transform transform-origin unicode-bidi vector-effect visibility white-space word-spacing "
            "writing-mode"sv);

        // comments
        //     false
        configuration.comments = false;

        // dataAttributes
        //     false
        configuration.data_attributes = false;

        // javascriptURLs
        //     false
        configuration.javascript_urls = false;

        return configuration;
    }() };
    return *configuration;
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#get-a-sanitizer-config-from-options
WebIDL::ExceptionOr<Optional<SanitizerConfiguration>> get_a_sanitizer_config_from_options(SanitizerOptions const& options, SanitizerSafety safety)
{
    bool safe = safety == SanitizerSafety::Safe;

    // 1. Let sanitizerInput be null.
    using SanitizerInput = Variant<Empty, GC::Ref<Sanitizer>, Bindings::SanitizerConfig const*, SanitizerConfiguration const*>;
    SanitizerInput sanitizer_input;

    auto set_sanitizer_input = [&](auto const& sanitizer) {
        sanitizer.visit(
            [&](Bindings::SanitizerConfig const& config) { sanitizer_input = &config; },
            [&](Bindings::SanitizerPresets preset) {
                // 1. Assert: sanitizerInput is "default".
                VERIFY(preset == Bindings::SanitizerPresets::Default);

                // 2. Set sanitizerInput to the built-in safe default configuration.
                sanitizer_input = &built_in_safe_default_configuration();
            },
            [&](auto const& sanitizer_object) { sanitizer_input = GC::Ref<Sanitizer> { *sanitizer_object }; });
    };

    // 2. If options is a TrustedHTMLParserOptions:
    if (auto const* trusted_options = options.get_pointer<GC::Ref<TrustedTypes::TrustedHTMLParserOptions>>()) {
        // 1. If options's sanitizer configuration is null:
        if (!(*trusted_options)->sanitizer_configuration().has_value()) {
            // 1. Assert: safe is false.
            VERIFY(!safe);

            // 2. Return null.
            return Optional<SanitizerConfiguration> {};
        }

        // 2. Set sanitizerInput to options's sanitizer configuration.
        sanitizer_input = &*(*trusted_options)->sanitizer_configuration();
    }

    // 3. Otherwise, if options["sanitizer"] exists, then set sanitizerInput to options["sanitizer"].
    bool has_sanitizer = options.visit(
        [&](GC::Ref<TrustedTypes::TrustedHTMLParserOptions>) {
            return true;
        },
        [&](Bindings::SetHTMLOptions const* set_html_options) {
            set_sanitizer_input(set_html_options->sanitizer);
            return true;
        },
        [&](auto const* dictionary) {
            if (!dictionary->sanitizer.has_value())
                return false;
            set_sanitizer_input(*dictionary->sanitizer);
            return true;
        });

    if (!has_sanitizer) {
        // 4. Otherwise, if safe is false, then return null.
        if (!safe)
            return Optional<SanitizerConfiguration> {};

        // 5. Otherwise, set sanitizerInput to "default".
        // NB: The following step sets sanitizerInput to the built-in safe default configuration.
        sanitizer_input = &built_in_safe_default_configuration();
    }

    // 6. Assert: sanitizerInput is either a Sanitizer instance, a SanitizerPresets member, or a SanitizerConfig
    //    dictionary.
    // 7. If sanitizerInput is a string:
    //    1. Assert: sanitizerInput is "default".
    //    2. Set sanitizerInput to the built-in safe default configuration.
    // NB: This was done above.

    // 8. If sanitizerInput is a Sanitizer, then set sanitizerInput to sanitizerInput's configuration.
    // 9. Assert: sanitizerInput is a dictionary.
    auto clone = sanitizer_input.visit(
        [](Empty) -> SanitizerConfiguration { VERIFY_NOT_REACHED(); },
        [](GC::Ref<Sanitizer> sanitizer) { return sanitizer->configuration(); },
        [](Bindings::SanitizerConfig const* config) { return canonicalize_sanitizer_config(*config); },
        [](SanitizerConfiguration const* config) { return *config; });

    // 10. Let sanitizer be a new Sanitizer object.
    // 11. Let permissiveDefaults be true if safe is false; false otherwise.
    auto permissive_defaults = safe ? PermissiveDefaults::No : PermissiveDefaults::Yes;

    // 12. Configure sanitizer given sanitizerInput and permissiveDefaults.
    // 13. Let configuration be sanitizer's configuration.
    auto configuration = TRY(configure_sanitizer_configuration(move(clone), permissive_defaults));

    // 14. If safe is true, then remove unsafe from configuration.
    if (safe)
        remove_unsafe_from_sanitizer_config(configuration);

    // 15. Assert: configuration is valid.
    VERIFY(sanitizer_configuration_is_valid(configuration));

    // 16. Return configuration.
    // NOTE: The returned SanitizerConfig is a fresh copy, so removing unsafe items from it never affects a Sanitizer
    //       object or SanitizerConfig dictionary that script can observe.
    return Optional<SanitizerConfiguration> { move(configuration) };
}

// https://w3c.github.io/trusted-types/dist/spec/#get-a-sanitizer-from-dictionary
// NB: The steps of "get a sanitizer from dictionary" after sanitizerInput has been found to be non-null.
WebIDL::ExceptionOr<GC::Ref<Sanitizer>> get_a_sanitizer_from_sanitizer_input(SanitizerInput const& sanitizer_input)
{
    // 3. Let configuration be null.
    auto configuration = sanitizer_input.visit(
        // 4. If sanitizerInput is a SanitizerPresets:
        [](Bindings::SanitizerPresets preset) {
            // 1. Assert: sanitizerInput is "default".
            VERIFY(preset == Bindings::SanitizerPresets::Default);

            // 2. Set configuration to the built-in safe default configuration.
            return built_in_safe_default_configuration();
        },
        // 5. Otherwise, if sanitizerInput is a Sanitizer:
        //    1. Set configuration to sanitizerInput's configuration.
        [](GC::Ref<Sanitizer> sanitizer) { return sanitizer->configuration(); },
        // 6. Otherwise, if sanitizerInput is a SanitizerConfig:
        //    1. Set configuration to sanitizerInput.
        [](Bindings::SanitizerConfig const& config) { return canonicalize_sanitizer_config(config); });

    // 7. Assert: configuration is not null.
    // 8. Let sanitizer be a new Sanitizer in realm.
    // 9. Configure sanitizer given configuration and true.
    auto canonical_configuration = TRY(configure_sanitizer_configuration(move(configuration), PermissiveDefaults::Yes));

    // 10. Return sanitizer.
    return Sanitizer::create_with_configuration(move(canonical_configuration));
}

}
