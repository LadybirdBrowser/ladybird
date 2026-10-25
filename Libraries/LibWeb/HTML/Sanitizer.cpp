/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <LibGC/Heap.h>
#include <LibWeb/Bindings/Sanitizer.h>
#include <LibWeb/HTML/Sanitizer.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(Sanitizer);

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-sanitizer-constructor
WebIDL::ExceptionOr<GC::Ref<Sanitizer>> Sanitizer::create(Variant<Bindings::SanitizerConfig, Bindings::SanitizerPresets> const& configuration)
{
    // 1. If configuration is a SanitizerPresets string:
    //    1. Assert: configuration is "default".
    //    2. Set configuration to the built-in safe default configuration.
    auto fresh_configuration = configuration.visit(
        [](Bindings::SanitizerConfig const& config) { return canonicalize_sanitizer_config(config); },
        [](Bindings::SanitizerPresets preset) {
            VERIFY(preset == Bindings::SanitizerPresets::Default);
            return built_in_safe_default_configuration();
        });

    // 2. Configure this given configuration and true.
    auto canonical_configuration = TRY(configure_sanitizer_configuration(move(fresh_configuration), PermissiveDefaults::Yes));
    return create_with_configuration(move(canonical_configuration));
}

GC::Ref<Sanitizer> Sanitizer::create_with_configuration(SanitizerConfiguration configuration)
{
    VERIFY(sanitizer_configuration_is_valid(configuration));
    return GC::Heap::the().allocate<Sanitizer>(move(configuration));
}

Sanitizer::Sanitizer(SanitizerConfiguration configuration)
    : m_configuration(move(configuration))
{
}

Sanitizer::~Sanitizer() = default;

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#compare-sanitizer-items
static bool compare_sanitizer_items(SanitizerName const& item_a, SanitizerName const& item_b)
{
    // 1. Let namespaceA be itemA["namespace"].
    auto const& namespace_a = item_a.namespace_;

    // 2. Let namespaceB be itemB["namespace"].
    auto const& namespace_b = item_b.namespace_;

    // 3. If namespaceA is null:
    if (!namespace_a.has_value()) {
        // 1. If namespaceB is not null, then return true.
        if (namespace_b.has_value())
            return true;
    }
    // 4. Otherwise:
    else {
        // 1. If namespaceB is null, then return false.
        if (!namespace_b.has_value())
            return false;

        // 2. If namespaceA is code unit less than namespaceB, then return true.
        if (namespace_a->view().is_code_unit_less_than(namespace_b->view()))
            return true;

        // 3. If namespaceA is not namespaceB, then return false.
        if (*namespace_a != *namespace_b)
            return false;
    }

    // 5. If itemA["name"] is code unit less than itemB["name"], then return true.
    if (item_a.name.view().is_code_unit_less_than(item_b.name.view()))
        return true;

    // 6. Return false.
    return false;
}

static void sort_sanitizer_items(Vector<SanitizerName>& list)
{
    quick_sort(list, compare_sanitizer_items);
}

static void sort_processing_instructions(Vector<Utf16String>& list)
{
    quick_sort(list, [](auto const& pi_a, auto const& pi_b) { return pi_a.utf16_view().is_code_unit_less_than(pi_b.utf16_view()); });
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-sanitizer-get
Bindings::SanitizerConfig Sanitizer::get() const
{
    // NOTE: Outside of the get() method, the order of the Sanitizer's elements and attributes is unobservable. By
    //       explicitly sorting the result of this method, we give implementations the opportunity to optimize by,
    //       for example, using unordered sets internally.

    // 1. Let config be this's configuration.
    auto config = m_configuration;

    // 2. Assert: config is valid.
    VERIFY(sanitizer_configuration_is_valid(config));

    // 3. If config["elements"] exists:
    if (config.elements.has_value()) {
        // 1. For each element of config["elements"]:
        for (auto& element : *config.elements) {
            // 1. If element["attributes"] exists, then set element["attributes"] to the result of sorting
            //    element["attributes"], with compare sanitizer items.
            if (element.attributes.has_value())
                sort_sanitizer_items(*element.attributes);

            // 2. If element["removeAttributes"] exists, then set element["removeAttributes"] to the result of sorting
            //    element["removeAttributes"], with compare sanitizer items.
            if (element.remove_attributes.has_value())
                sort_sanitizer_items(*element.remove_attributes);
        }

        // 2. Set config["elements"] to the result of sorting config["elements"], with compare sanitizer items.
        quick_sort(*config.elements, [](auto const& a, auto const& b) { return compare_sanitizer_items(a.element, b.element); });
    }
    // 4. Otherwise:
    else {
        // 1. Set config["removeElements"] to the result of sorting config["removeElements"], with compare sanitizer
        //    items.
        sort_sanitizer_items(*config.remove_elements);
    }

    // 5. If config["replaceWithChildrenElements"] exists, then set config["replaceWithChildrenElements"] to the result
    //    of sorting config["replaceWithChildrenElements"], with compare sanitizer items.
    if (config.replace_with_children_elements.has_value())
        sort_sanitizer_items(*config.replace_with_children_elements);

    // 6. If config["processingInstructions"] exists, then set config["processingInstructions"] to the result of
    //    sorting config["processingInstructions"], with piA["target"] being code unit less than piB["target"].
    if (config.processing_instructions.has_value())
        sort_processing_instructions(*config.processing_instructions);
    // 7. Otherwise:
    else {
        // 1. Set config["removeProcessingInstructions"] to the result of sorting
        //    config["removeProcessingInstructions"], with piA["target"] being code unit less than piB["target"].
        sort_processing_instructions(*config.remove_processing_instructions);
    }

    // 8. If config["attributes"] exists, then set config["attributes"] to the result of sorting config["attributes"]
    //    given compare sanitizer items.
    if (config.attributes.has_value())
        sort_sanitizer_items(*config.attributes);
    // 9. Otherwise:
    else {
        // 1. Set config["removeAttributes"] to the result of sorting config["removeAttributes"] given compare
        //    sanitizer items.
        sort_sanitizer_items(*config.remove_attributes);
    }

    // 10. Return config.
    return sanitizer_configuration_to_bindings(config);
}

// https://infra.spec.whatwg.org/#create-a-set
static Vector<SanitizerName> create_a_set(Vector<SanitizerName> const& list)
{
    Vector<SanitizerName> set;
    for (auto const& item : list) {
        if (!sanitizer_list_contains(set, item))
            set.append(item);
    }
    return set;
}

// https://infra.spec.whatwg.org/#set-difference
static Vector<SanitizerName> difference(Vector<SanitizerName> const& a, Vector<SanitizerName> const& b)
{
    Vector<SanitizerName> result;
    for (auto const& item : a) {
        if (!sanitizer_list_contains(b, item))
            result.append(item);
    }
    return result;
}

// https://infra.spec.whatwg.org/#set-intersection
static Vector<SanitizerName> intersection(Vector<SanitizerName> const& a, Vector<SanitizerName> const& b)
{
    Vector<SanitizerName> result;
    for (auto const& item : a) {
        if (sanitizer_list_contains(b, item))
            result.append(item);
    }
    return result;
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-sanitizer-allowelement
bool Sanitizer::allow_element(Variant<Utf16String, Bindings::SanitizerElementNamespaceWithAttributes> const& element_input)
{
    // 1. Let configuration be this's configuration.
    auto& configuration = m_configuration;

    // 2. Assert: configuration is valid.
    VERIFY(sanitizer_configuration_is_valid(configuration));

    // 3. Set element to the result of canonicalizing element.
    auto element = canonicalize_sanitizer_element_with_attributes(element_input);

    // 4. If configuration["elements"] exists:
    if (configuration.elements.has_value()) {
        // 1. Let modified be the result of removing element from configuration["replaceWithChildrenElements"].
        bool modified = sanitizer_list_remove(configuration.replace_with_children_elements, element.element);

        // 2. If configuration["attributes"] exists:
        if (configuration.attributes.has_value()) {
            // 1. If element["attributes"] exists:
            if (element.attributes.has_value()) {
                // 1. Set element["attributes"] to the result of creating a set from element["attributes"].
                element.attributes = create_a_set(*element.attributes);

                // 2. Set element["attributes"] to the difference of element["attributes"] and
                //    configuration["attributes"].
                element.attributes = difference(*element.attributes, *configuration.attributes);

                // 3. If configuration["dataAttributes"] is true, then remove all items item from element["attributes"]
                //    where item is a custom data attribute.
                if (configuration.data_attributes == true)
                    element.attributes->remove_all_matching([](auto const& item) { return is_custom_data_attribute(item); });
            }

            // 2. If element["removeAttributes"] exists:
            if (element.remove_attributes.has_value()) {
                // 1. Set element["removeAttributes"] to the result of creating a set from element["removeAttributes"].
                element.remove_attributes = create_a_set(*element.remove_attributes);

                // 2. Set element["removeAttributes"] to the intersection of element["removeAttributes"] and
                //    configuration["attributes"].
                element.remove_attributes = intersection(*element.remove_attributes, *configuration.attributes);
            }
        }
        // 3. Otherwise:
        else {
            // 1. If element["attributes"] exists:
            if (element.attributes.has_value()) {
                // 1. Set element["attributes"] to the result of creating a set from element["attributes"].
                element.attributes = create_a_set(*element.attributes);

                // 2. Set element["attributes"] to the difference of element["attributes"] and
                //    element["removeAttributes"] with default « ».
                element.attributes = difference(*element.attributes, element.remove_attributes.value_or({}));

                // 3. Remove element["removeAttributes"].
                element.remove_attributes = {};

                // 4. Set element["attributes"] to the difference of element["attributes"] and
                //    configuration["removeAttributes"].
                element.attributes = difference(*element.attributes, *configuration.remove_attributes);
            }

            // 2. If element["removeAttributes"] exists:
            if (element.remove_attributes.has_value()) {
                // 1. Set element["removeAttributes"] to the result of creating a set from element["removeAttributes"].
                element.remove_attributes = create_a_set(*element.remove_attributes);

                // 2. Set element["removeAttributes"] to the difference of element["removeAttributes"] and
                //    configuration["removeAttributes"].
                element.remove_attributes = difference(*element.remove_attributes, *configuration.remove_attributes);
            }
        }

        // 4. If configuration["elements"] does not contain element:
        auto current_element = configuration.elements->find_if([&](auto const& item) { return item.element == element.element; });
        if (current_element.is_end()) {
            // 1. Append element to configuration["elements"].
            configuration.elements->append(move(element));

            // 2. Return true.
            return true;
        }

        // 5. Let currentElement be the item in configuration["elements"] whose name member is element's name member
        //    and whose namespace member is element's namespace member.
        // 6. If element is equal to currentElement, then return modified.
        if (element == *current_element)
            return modified;

        // 7. Remove element from configuration["elements"].
        configuration.elements->remove(current_element.index());

        // 8. Append element to configuration["elements"].
        configuration.elements->append(move(element));

        // 9. Return true.
        return true;
    }

    // 5. Otherwise:
    // 1. If element["attributes"] exists or element["removeAttributes"] with default « » is not empty, then return
    //    false.
    if (element.attributes.has_value() || (element.remove_attributes.has_value() && !element.remove_attributes->is_empty()))
        return false;

    // 2. Let modified be the result of removing element from configuration["replaceWithChildrenElements"].
    bool modified = sanitizer_list_remove(configuration.replace_with_children_elements, element.element);

    // 3. If configuration["removeElements"] does not contain element, then return modified.
    if (!sanitizer_list_contains(*configuration.remove_elements, element.element))
        return modified;

    // 4. Remove element from configuration["removeElements"].
    sanitizer_list_remove(configuration.remove_elements, element.element);

    // 5. Return true.
    return true;
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-sanitizer-removeelement
bool Sanitizer::remove_element(Variant<Utf16String, Bindings::SanitizerElementNamespace> const& element)
{
    // The removeElement(element) method steps are to return the result of removing element from this's
    // configuration.
    return remove_an_element_from_sanitizer_config(m_configuration, canonicalize_sanitizer_element(element));
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-sanitizer-replaceelementwithchildren
bool Sanitizer::replace_element_with_children(Variant<Utf16String, Bindings::SanitizerElementNamespace> const& element_input)
{
    // 1. Let configuration be this's configuration.
    auto& configuration = m_configuration;

    // 2. Assert: configuration is valid.
    VERIFY(sanitizer_configuration_is_valid(configuration));

    // 3. Set element to the result of canonicalizing element.
    auto element = canonicalize_sanitizer_element(element_input);

    // 4. If the built-in non-replaceable elements list contains element, then return false.
    if (built_in_non_replaceable_elements_list_contains(element))
        return false;

    // 5. Let modified be the result of removing element from configuration["elements"].
    bool modified = false;
    if (configuration.elements.has_value())
        modified = configuration.elements->remove_all_matching([&](auto const& item) { return item.element == element; });

    // 6. If removing element from configuration["removeElements"] is true, then set modified to true.
    if (sanitizer_list_remove(configuration.remove_elements, element))
        modified = true;

    // 7. If configuration["replaceWithChildrenElements"] does not contain element:
    if (!configuration.replace_with_children_elements.has_value() || !sanitizer_list_contains(*configuration.replace_with_children_elements, element)) {
        // 1. Append element to configuration["replaceWithChildrenElements"].
        if (!configuration.replace_with_children_elements.has_value())
            configuration.replace_with_children_elements = Vector<SanitizerName> {};
        configuration.replace_with_children_elements->append(element);

        // 2. Return true.
        return true;
    }

    // 8. Return modified.
    return modified;
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-sanitizer-allowattribute
bool Sanitizer::allow_attribute(Variant<Utf16String, Bindings::SanitizerAttributeNamespace> const& attribute_input)
{
    // 1. Let configuration be this's configuration.
    auto& configuration = m_configuration;

    // 2. Assert: configuration is valid.
    VERIFY(sanitizer_configuration_is_valid(configuration));

    // 3. Set attribute to the result of canonicalizing attribute.
    auto attribute = canonicalize_sanitizer_attribute(attribute_input);

    // 4. If configuration["attributes"] exists:
    if (configuration.attributes.has_value()) {
        // 1. If configuration["dataAttributes"] is true and attribute is a custom data attribute, then return false.
        if (configuration.data_attributes == true && is_custom_data_attribute(attribute))
            return false;

        // 2. If configuration["attributes"] contains attribute, then return false.
        if (sanitizer_list_contains(*configuration.attributes, attribute))
            return false;

        // 3. If configuration["elements"] exists:
        if (configuration.elements.has_value()) {
            // 1. For each element in configuration["elements"]:
            for (auto& element : *configuration.elements) {
                // 1. If element["attributes"] with default « » contains attribute, then remove attribute from
                //    element["attributes"].
                sanitizer_list_remove(element.attributes, attribute);
            }
        }

        // 4. Append attribute to configuration["attributes"].
        configuration.attributes->append(attribute);

        // 5. Return true.
        return true;
    }

    // 5. Otherwise:
    // 1. If configuration["removeAttributes"] does not contain attribute, then return false.
    if (!sanitizer_list_contains(*configuration.remove_attributes, attribute))
        return false;

    // 2. Remove attribute from configuration["removeAttributes"].
    sanitizer_list_remove(configuration.remove_attributes, attribute);

    // 3. Return true.
    return true;
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-sanitizer-removeattribute
bool Sanitizer::remove_attribute(Variant<Utf16String, Bindings::SanitizerAttributeNamespace> const& attribute)
{
    // The removeAttribute(attribute) method steps are to return the result of removing attribute from this's
    // configuration.
    return remove_an_attribute_from_sanitizer_config(m_configuration, canonicalize_sanitizer_attribute(attribute));
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-sanitizer-setcomments
bool Sanitizer::set_comments(bool allow)
{
    // 1. Let configuration be this's configuration.
    auto& configuration = m_configuration;

    // 2. Assert: configuration is valid.
    VERIFY(sanitizer_configuration_is_valid(configuration));

    // 3. If configuration["comments"] is allow, then return false.
    if (configuration.comments == allow)
        return false;

    // 4. Set configuration["comments"] to allow.
    configuration.comments = allow;

    // 5. Return true.
    return true;
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-sanitizer-setdataattributes
bool Sanitizer::set_data_attributes(bool allow)
{
    // 1. Let configuration be this's configuration.
    auto& configuration = m_configuration;

    // 2. Assert: configuration is valid.
    VERIFY(sanitizer_configuration_is_valid(configuration));

    // 3. If configuration["attributes"] does not exist, then return false.
    if (!configuration.attributes.has_value())
        return false;

    // 4. If configuration["dataAttributes"] exists and is equal to allow, then return false.
    if (configuration.data_attributes == allow)
        return false;

    // 5. If allow is true:
    if (allow) {
        // 1. If configuration["elements"] exists:
        if (configuration.elements.has_value()) {
            // 1. For each element of configuration["elements"]:
            for (auto& element : *configuration.elements) {
                // 1. If element["attributes"] exists, then remove all items item from element["attributes"] where item
                //    is a custom data attribute.
                if (element.attributes.has_value())
                    element.attributes->remove_all_matching([](auto const& item) { return is_custom_data_attribute(item); });
            }
        }

        // 2. Remove all items item from configuration["attributes"] where item is a custom data attribute.
        configuration.attributes->remove_all_matching([](auto const& item) { return is_custom_data_attribute(item); });
    }

    // 6. Set configuration["dataAttributes"] to allow.
    configuration.data_attributes = allow;

    // 7. Return true.
    return true;
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-sanitizer-setjavascripturls
bool Sanitizer::set_javascript_urls(bool allow)
{
    // 1. Let configuration be this's configuration.
    auto& configuration = m_configuration;

    // 2. Assert: configuration is valid.
    VERIFY(sanitizer_configuration_is_valid(configuration));

    // 3. If configuration["javascriptURLs"] is allow, then return false.
    if (configuration.javascript_urls == allow)
        return false;

    // 4. Set configuration["javascriptURLs"] to allow.
    configuration.javascript_urls = allow;

    // 5. Return true.
    return true;
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-sanitizer-allowprocessinginstruction
bool Sanitizer::allow_processing_instruction(Variant<Utf16String, Bindings::SanitizerProcessingInstruction> const& pi_input)
{
    // 1. Let configuration be this's configuration.
    auto& configuration = m_configuration;

    // 2. Assert: configuration is valid.
    VERIFY(sanitizer_configuration_is_valid(configuration));

    // 3. Set pi to the result of canonicalizing pi.
    auto pi = canonicalize_sanitizer_processing_instruction(pi_input);

    // 4. If configuration["processingInstructions"] exists:
    if (configuration.processing_instructions.has_value()) {
        // 1. If configuration["processingInstructions"] contains pi, then return false.
        if (configuration.processing_instructions->contains_slow(pi))
            return false;

        // 2. Append pi to configuration["processingInstructions"].
        configuration.processing_instructions->append(move(pi));

        // 3. Return true.
        return true;
    }

    // 5. Otherwise:
    // 1. If configuration["removeProcessingInstructions"] contains pi:
    if (configuration.remove_processing_instructions->contains_slow(pi)) {
        // 1. Remove pi from configuration["removeProcessingInstructions"].
        configuration.remove_processing_instructions->remove_all_matching([&](auto const& item) { return item == pi; });

        // 2. Return true.
        return true;
    }

    // 2. Return false.
    return false;
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-sanitizer-removeprocessinginstruction
bool Sanitizer::remove_processing_instruction(Variant<Utf16String, Bindings::SanitizerProcessingInstruction> const& pi_input)
{
    // 1. Let configuration be this's configuration.
    auto& configuration = m_configuration;

    // 2. Assert: configuration is valid.
    VERIFY(sanitizer_configuration_is_valid(configuration));

    // 3. Set pi to the result of canonicalizing pi.
    auto pi = canonicalize_sanitizer_processing_instruction(pi_input);

    // 4. If configuration["processingInstructions"] exists:
    if (configuration.processing_instructions.has_value()) {
        // 1. If configuration["processingInstructions"] contains pi:
        if (configuration.processing_instructions->contains_slow(pi)) {
            // 1. Remove pi from configuration["processingInstructions"].
            configuration.processing_instructions->remove_all_matching([&](auto const& item) { return item == pi; });

            // 2. Return true.
            return true;
        }

        // 2. Return false.
        return false;
    }

    // 5. Otherwise:
    // 1. If configuration["removeProcessingInstructions"] contains pi, then return false.
    if (configuration.remove_processing_instructions->contains_slow(pi))
        return false;

    // 2. Append pi to configuration["removeProcessingInstructions"].
    configuration.remove_processing_instructions->append(move(pi));

    // 3. Return true.
    return true;
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-sanitizer-removeunsafe
bool Sanitizer::remove_unsafe()
{
    // The removeUnsafe() method steps are to return the result of removing unsafe from this's configuration.
    return remove_unsafe_from_sanitizer_config(m_configuration);
}

}
