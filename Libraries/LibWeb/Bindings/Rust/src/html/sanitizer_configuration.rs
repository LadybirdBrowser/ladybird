/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ffi::c_void;
use std::sync::{Arc, OnceLock};

use ak::Utf16String;
use libjs_rust::api::Utf16View;
use liburl_rust::url::{BasicParseOptions, UrlInput, basic_parse};

use super::sanitizer::{
    self, Sanitizer, SanitizerAttribute, SanitizerAttributeNamespace, SanitizerConfig, SanitizerElement,
    SanitizerElementNamespace, SanitizerElementNamespaceWithAttributes, SanitizerPI, SanitizerPresets,
    SanitizerProcessingInstruction,
};
use crate::bindings::interface::{Ref, cell_from_abi, cell_into_abi};
use crate::bindings::{Exception, WebIdlResult};

const HTML: &str = "http://www.w3.org/1999/xhtml";
const SVG: &str = "http://www.w3.org/2000/svg";
const MATHML: &str = "http://www.w3.org/1998/Math/MathML";
const XLINK: &str = "http://www.w3.org/1999/xlink";

// NB: A canonicalized SanitizerElementNamespace or SanitizerAttributeNamespace dictionary.
#[derive(Clone, PartialEq, Eq)]
pub(crate) struct SanitizerName {
    pub(crate) name: Utf16String,
    pub(crate) namespace_: Option<Utf16String>,
}

// NB: A canonicalized SanitizerElementNamespaceWithAttributes dictionary.
#[derive(Clone, PartialEq, Eq)]
pub(crate) struct SanitizerElementWithAttributes {
    pub(crate) element: SanitizerName,
    pub(crate) attributes: Option<Vec<SanitizerName>>,
    pub(crate) remove_attributes: Option<Vec<SanitizerName>>,
}

// NB: A canonicalized SanitizerConfig dictionary. Processing instructions are represented by their target.
#[derive(Clone, Default)]
pub(crate) struct SanitizerConfiguration {
    pub(crate) elements: Option<Vec<SanitizerElementWithAttributes>>,
    pub(crate) remove_elements: Option<Vec<SanitizerName>>,
    pub(crate) replace_with_children_elements: Option<Vec<SanitizerName>>,
    pub(crate) processing_instructions: Option<Vec<Utf16String>>,
    pub(crate) remove_processing_instructions: Option<Vec<Utf16String>>,
    pub(crate) attributes: Option<Vec<SanitizerName>>,
    pub(crate) remove_attributes: Option<Vec<SanitizerName>>,
    pub(crate) comments: Option<bool>,
    pub(crate) data_attributes: Option<bool>,
    pub(crate) javascript_urls: Option<bool>,
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#sanitizer-action
// NB: SanitizerAction in SanitizerConfiguration.h matches this.
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum SanitizerAction {
    Keep,
    Remove,
    ReplaceWithChildren,
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum PermissiveDefaults {
    No,
    Yes,
}

pub(crate) enum NameInput<'a> {
    String(&'a Utf16String),
    Dictionary(&'a Utf16String, Option<&'a Utf16String>),
}

impl<'a> From<&'a SanitizerElement> for NameInput<'a> {
    fn from(element: &'a SanitizerElement) -> Self {
        match element {
            SanitizerElement::DOMString(name) => Self::String(name),
            SanitizerElement::SanitizerElementNamespace(dictionary) => {
                Self::Dictionary(&dictionary.name, dictionary.namespace.as_ref())
            }
        }
    }
}

impl<'a> From<&'a sanitizer::SanitizerElementWithAttributes> for NameInput<'a> {
    fn from(element: &'a sanitizer::SanitizerElementWithAttributes) -> Self {
        match element {
            sanitizer::SanitizerElementWithAttributes::DOMString(name) => Self::String(name),
            sanitizer::SanitizerElementWithAttributes::SanitizerElementNamespaceWithAttributes(dictionary) => {
                Self::Dictionary(&dictionary.parent.name, dictionary.parent.namespace.as_ref())
            }
        }
    }
}

impl<'a> From<&'a SanitizerAttribute> for NameInput<'a> {
    fn from(attribute: &'a SanitizerAttribute) -> Self {
        match attribute {
            SanitizerAttribute::DOMString(name) => Self::String(name),
            SanitizerAttribute::SanitizerAttributeNamespace(dictionary) => {
                Self::Dictionary(&dictionary.name, dictionary.namespace.as_ref())
            }
        }
    }
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#canonicalize-a-sanitizer-name
fn canonicalize_a_sanitizer_name(name: NameInput<'_>, default_namespace: Option<&str>) -> SanitizerName {
    match name {
        NameInput::String(string) => {
            // 1. If name is a DOMString, then return «[ "name" → name, "namespace" → defaultNamespace ]».
            SanitizerName {
                name: string.clone(),
                namespace_: default_namespace.map(Utf16String::from_utf8),
            }
        }
        NameInput::Dictionary(name, namespace_) => {
            // 2. Assert: name is a dictionary and both name["name"] and name["namespace"] exist.
            let mut namespace_ = namespace_.cloned();

            // 3. If name["namespace"] is the empty string, then set it to null.
            if namespace_.as_ref().is_some_and(Utf16String::is_empty) {
                namespace_ = None;
            }

            // 4. Return «[ "name" → name["name"], "namespace" → name["namespace"] ]».
            SanitizerName {
                name: name.clone(),
                namespace_,
            }
        }
    }
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#canonicalize-a-sanitizer-element
fn canonicalize_a_sanitizer_element(element: NameInput<'_>) -> SanitizerName {
    // Return the result of canonicalizing element with the HTML namespace as the default namespace.
    canonicalize_a_sanitizer_name(element, Some(HTML))
}

// NB: Attributes are canonicalized with the default namespace of null.
fn canonicalize_a_sanitizer_attribute(attribute: NameInput<'_>) -> SanitizerName {
    canonicalize_a_sanitizer_name(attribute, None)
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#canonicalize-a-sanitizer-element-list
fn canonicalize_a_sanitizer_element_list(list: &[SanitizerElement]) -> Vec<SanitizerName> {
    // 1. Let newList be « ».
    let mut new_list = Vec::with_capacity(list.len());

    // 2. For each item in list, append the result of canonicalizing item to newList.
    for item in list {
        new_list.push(canonicalize_a_sanitizer_element(item.into()));
    }

    // 3. Return newList.
    new_list
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#canonicalize-a-sanitizer-list
fn canonicalize_a_sanitizer_list(list: &[SanitizerAttribute]) -> Vec<SanitizerName> {
    // 1. Let newList be « ».
    let mut new_list = Vec::with_capacity(list.len());

    // 2. For each item in list, append the result of canonicalizing item to newList.
    for item in list {
        new_list.push(canonicalize_a_sanitizer_attribute(item.into()));
    }

    // 3. Return newList.
    new_list
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#canonicalize-a-processing-instruction-list
fn canonicalize_a_processing_instruction_list(list: &[SanitizerPI]) -> Vec<Utf16String> {
    // 1. Let newList be « ».
    let mut new_list = Vec::with_capacity(list.len());

    // 2. For each item in list, append the result of canonicalizing item to newList.
    for item in list {
        new_list.push(canonicalize_sanitizer_processing_instruction(item));
    }

    // 3. Return newList.
    new_list
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#canonicalize-a-sanitizer-element-with-attributes
fn canonicalize_a_sanitizer_element_with_attributes(
    element: &sanitizer::SanitizerElementWithAttributes,
) -> SanitizerElementWithAttributes {
    // 1. Let result be the result of canonicalizing element.
    let mut result = SanitizerElementWithAttributes {
        element: canonicalize_a_sanitizer_element(element.into()),
        attributes: None,
        remove_attributes: None,
    };

    // 2. If element is a dictionary:
    if let sanitizer::SanitizerElementWithAttributes::SanitizerElementNamespaceWithAttributes(dictionary) = element {
        // 1. If element["attributes"] exists, then set result["attributes"] to the result of canonicalizing
        //    element["attributes"].
        if let Some(attributes) = &dictionary.attributes {
            result.attributes = Some(canonicalize_a_sanitizer_list(attributes));
        }

        // 2. If element["removeAttributes"] exists, then set result["removeAttributes"] to the result of
        //    canonicalizing element["removeAttributes"].
        if let Some(attributes) = &dictionary.remove_attributes {
            result.remove_attributes = Some(canonicalize_a_sanitizer_list(attributes));
        }
    }

    // 3. If neither result["attributes"] nor result["removeAttributes"] exists, then set result["removeAttributes"]
    //    to an empty list.
    if result.attributes.is_none() && result.remove_attributes.is_none() {
        result.remove_attributes = Some(Vec::new());
    }

    // 4. Return result.
    result
}

pub(crate) fn canonicalize_sanitizer_element(element: &SanitizerElement) -> SanitizerName {
    canonicalize_a_sanitizer_element(element.into())
}
pub(crate) fn canonicalize_sanitizer_element_with_attributes(
    element: &sanitizer::SanitizerElementWithAttributes,
) -> SanitizerElementWithAttributes {
    canonicalize_a_sanitizer_element_with_attributes(element)
}
pub(crate) fn canonicalize_sanitizer_attribute(attribute: &SanitizerAttribute) -> SanitizerName {
    canonicalize_a_sanitizer_attribute(attribute.into())
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#canonicalize-a-processing-instruction
pub(crate) fn canonicalize_sanitizer_processing_instruction(pi: &SanitizerPI) -> Utf16String {
    match pi {
        // 1. If pi is a DOMString, then return «[ "target" → pi ]».
        SanitizerPI::DOMString(string) => string.clone(),

        // 2. Assert: pi is a dictionary and pi["target"] exists.
        // 3. Return «[ "target" → pi["target"] ]».
        SanitizerPI::SanitizerProcessingInstruction(dictionary) => dictionary.target.clone(),
    }
}

// NB: This performs the per-item canonicalization steps of "canonicalize the configuration" on a dictionary supplied
//     by script. The remaining steps are performed by canonicalize_the_configuration().
// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#canonicalize-the-configuration
pub(crate) fn canonicalize_sanitizer_config(configuration: &SanitizerConfig) -> SanitizerConfiguration {
    let mut result = SanitizerConfiguration::default();

    // 4. If configuration["elements"] exists:
    if let Some(elements) = &configuration.elements {
        // 1. Let newElements be « ».
        let mut new_elements = Vec::with_capacity(elements.len());

        // 2. For each element of configuration["elements"], append the result of canonicalizing element to
        //    newElements.
        for element in elements {
            new_elements.push(canonicalize_a_sanitizer_element_with_attributes(element));
        }

        // 3. Set configuration["elements"] to newElements.
        result.elements = Some(new_elements);
    }

    // 5. If configuration["removeElements"] exists, then set configuration["removeElements"] to the result of
    //    canonicalizing configuration["removeElements"].
    if let Some(list) = &configuration.remove_elements {
        result.remove_elements = Some(canonicalize_a_sanitizer_element_list(list));
    }

    // 6. If configuration["attributes"] exists, then set configuration["attributes"] to the result of canonicalizing
    //    configuration["attributes"].
    if let Some(list) = &configuration.attributes {
        result.attributes = Some(canonicalize_a_sanitizer_list(list));
    }

    // 7. If configuration["removeAttributes"] exists, then set configuration["removeAttributes"] to the result of
    //    canonicalizing configuration["removeAttributes"].
    if let Some(list) = &configuration.remove_attributes {
        result.remove_attributes = Some(canonicalize_a_sanitizer_list(list));
    }

    // 8. If configuration["replaceWithChildrenElements"] exists, then set configuration["replaceWithChildrenElements"]
    //    to the result of canonicalizing configuration["replaceWithChildrenElements"].
    if let Some(list) = &configuration.replace_with_children_elements {
        result.replace_with_children_elements = Some(canonicalize_a_sanitizer_element_list(list));
    }

    // 9. If configuration["processingInstructions"] exists, then set configuration["processingInstructions"] to the
    //    result of canonicalizing configuration["processingInstructions"].
    if let Some(list) = &configuration.processing_instructions {
        result.processing_instructions = Some(canonicalize_a_processing_instruction_list(list));
    }

    // 10. If configuration["removeProcessingInstructions"] exists, then set
    //     configuration["removeProcessingInstructions"] to the result of canonicalizing
    //     configuration["removeProcessingInstructions"].
    if let Some(list) = &configuration.remove_processing_instructions {
        result.remove_processing_instructions = Some(canonicalize_a_processing_instruction_list(list));
    }
    result.comments = configuration.comments;
    result.data_attributes = configuration.data_attributes;
    result.javascript_urls = configuration.javascript_ur_ls;
    result
}

// NB: This performs the steps of "canonicalize the configuration" that do not canonicalize individual items. Items of
//     a SanitizerConfiguration are always canonical, see canonicalize_sanitizer_config().
// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#canonicalize-the-configuration
fn canonicalize_the_configuration(configuration: &mut SanitizerConfiguration, permissive_defaults: PermissiveDefaults) {
    let permissive = permissive_defaults == PermissiveDefaults::Yes;

    // 1. If neither configuration["elements"] nor configuration["removeElements"] exists, then set
    //    configuration["removeElements"] to an empty list.
    if configuration.elements.is_none() && configuration.remove_elements.is_none() {
        configuration.remove_elements = Some(Vec::new());
    }

    // 2. If neither configuration["attributes"] nor configuration["removeAttributes"] exists, then set
    //    configuration["removeAttributes"] to an empty list.
    if configuration.attributes.is_none() && configuration.remove_attributes.is_none() {
        configuration.remove_attributes = Some(Vec::new());
    }

    // 3. If neither configuration["processingInstructions"] nor configuration["removeProcessingInstructions"] exists:
    if configuration.processing_instructions.is_none() && configuration.remove_processing_instructions.is_none() {
        // 1. If permissiveDefaults is true, then set configuration["removeProcessingInstructions"] to an empty list.
        if permissive {
            configuration.remove_processing_instructions = Some(Vec::new());
        }
        // 2. Otherwise, set configuration["processingInstructions"] to an empty list.
        else {
            configuration.processing_instructions = Some(Vec::new());
        }
    }
    // NB: Steps 4 to 10 canonicalize individual items, which canonicalize_sanitizer_config() has done.
    // 11. If configuration["comments"] does not exist, then set it to permissiveDefaults.
    if configuration.comments.is_none() {
        configuration.comments = Some(permissive);
    }

    // 12. If configuration["attributes"] exists and configuration["dataAttributes"] does not exist, then set it to
    //     permissiveDefaults.
    if configuration.attributes.is_some() && configuration.data_attributes.is_none() {
        configuration.data_attributes = Some(permissive);
    }

    // 13. If configuration["javascriptURLs"] does not exist, then set it to permissiveDefaults.
    if configuration.javascript_urls.is_none() {
        configuration.javascript_urls = Some(permissive);
    }
}

pub(crate) fn sanitizer_list_contains(list: Option<&[SanitizerName]>, item: &SanitizerName) -> bool {
    list.is_some_and(|list| list.contains(item))
}
// NB: Returns whether the list contained the item.
pub(crate) fn sanitizer_list_remove(list: &mut Option<Vec<SanitizerName>>, item: &SanitizerName) -> bool {
    let Some(list) = list else {
        return false;
    };
    let size = list.len();
    list.retain(|entry| entry != item);
    size != list.len()
}

fn find_element<'a>(
    elements: Option<&'a [SanitizerElementWithAttributes]>,
    local_name: &Utf16String,
    namespace_: Option<&Utf16String>,
) -> Option<&'a SanitizerElementWithAttributes> {
    elements?
        .iter()
        .find(|element| element.element.name == *local_name && element.element.namespace_.as_ref() == namespace_)
}
fn elements_contain(elements: Option<&[SanitizerElementWithAttributes]>, name: &SanitizerName) -> bool {
    find_element(elements, &name.name, name.namespace_.as_ref()).is_some()
}
fn elements_remove(elements: &mut Option<Vec<SanitizerElementWithAttributes>>, name: &SanitizerName) -> bool {
    let Some(elements) = elements else {
        return false;
    };
    let size = elements.len();
    elements.retain(|element| element.element != *name);
    size != elements.len()
}

// https://infra.spec.whatwg.org/#list-has-duplicates
fn has_duplicates<T: PartialEq>(list: &[T]) -> bool {
    for i in 0..list.len() {
        for j in i + 1..list.len() {
            if list[i] == list[j] {
                return true;
            }
        }
    }
    false
}

// https://infra.spec.whatwg.org/#set-intersection
fn intersection_is_empty<T: PartialEq>(a: &[T], b: &[T]) -> bool {
    for item in a {
        for other in b {
            if item == other {
                return false;
            }
        }
    }
    true
}

// https://infra.spec.whatwg.org/#set-subset
fn is_subset(subset: &[SanitizerName], superset: &[SanitizerName]) -> bool {
    for item in subset {
        if !superset.contains(item) {
            return false;
        }
    }
    true
}

// https://html.spec.whatwg.org/multipage/dom.html#custom-data-attribute
pub(crate) fn is_custom_data_attribute(attribute: &SanitizerName) -> bool {
    // A custom data attribute is an attribute in no namespace whose name starts with the string "data-", has at least
    // one character after the hyphen, is a valid attribute local name, and contains no ASCII upper alphas.
    if attribute.namespace_.is_some() {
        return false;
    }
    let name = Utf16View::of_string(&attribute.name);
    if !name.starts_with(Utf16View::Ascii(b"data-")) || name.length_in_code_units() <= 5 {
        return false;
    }
    if !crate::dom::element::is_valid_attribute_local_name(&attribute.name) {
        return false;
    }
    !name
        .code_units()
        .any(|unit| u8::try_from(unit).is_ok_and(|unit| unit.is_ascii_uppercase()))
}

fn contains_custom_data_attribute(list: &[SanitizerName]) -> bool {
    list.iter().any(is_custom_data_attribute)
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-sanitizerconfig-valid
pub(crate) fn sanitizer_configuration_is_valid(config: &SanitizerConfiguration) -> bool {
    // 1. Assert: config["elements"] exists or config["removeElements"] exists.
    assert!(config.elements.is_some() || config.remove_elements.is_some());

    // 2. If config["elements"] exists and config["removeElements"] exists, then return false.
    if config.elements.is_some() && config.remove_elements.is_some() {
        return false;
    }

    // 3. Assert: Either config["processingInstructions"] exists or config["removeProcessingInstructions"] exists.
    assert!(config.processing_instructions.is_some() || config.remove_processing_instructions.is_some());

    // 4. If config["processingInstructions"] exists and config["removeProcessingInstructions"] exists, then return
    //    false.
    if config.processing_instructions.is_some() && config.remove_processing_instructions.is_some() {
        return false;
    }

    // 5. Assert: Either config["attributes"] exists or config["removeAttributes"] exists.
    assert!(config.attributes.is_some() || config.remove_attributes.is_some());

    // 6. If config["attributes"] exists and config["removeAttributes"] exists, then return false.
    if config.attributes.is_some() && config.remove_attributes.is_some() {
        return false;
    }

    // 7. Assert: All SanitizerElementNamespaceWithAttributes, SanitizerElementNamespace,
    //    SanitizerProcessingInstruction, and SanitizerAttributeNamespace items in config are canonical, meaning they
    //    have been run through canonicalizing, as appropriate.
    // NB: The items of a SanitizerConfiguration are canonical by construction.
    // 8. If config["elements"] exists:
    if let Some(elements) = &config.elements {
        // 1. If config["elements"] has duplicates, then return false.
        for i in 0..elements.len() {
            for j in i + 1..elements.len() {
                if elements[i].element == elements[j].element {
                    return false;
                }
            }
        }
    }
    // 9. Otherwise:
    else {
        // 1. If config["removeElements"] has duplicates, then return false.
        if has_duplicates(config.remove_elements.as_ref().unwrap()) {
            return false;
        }
    }

    // 10. If config["replaceWithChildrenElements"] exists and has duplicates, then return false.
    if config
        .replace_with_children_elements
        .as_ref()
        .is_some_and(|list| has_duplicates(list))
    {
        return false;
    }

    // 11. If config["processingInstructions"] exists:
    if let Some(list) = &config.processing_instructions {
        // 1. If config["processingInstructions"] has duplicates, then return false.
        if has_duplicates(list) {
            return false;
        }
    }
    // 12. Otherwise:
    else {
        // 1. If config["removeProcessingInstructions"] has duplicates, then return false.
        if has_duplicates(config.remove_processing_instructions.as_ref().unwrap()) {
            return false;
        }
    }

    // 13. If config["attributes"] exists:
    if let Some(list) = &config.attributes {
        // 1. If config["attributes"] has duplicates, then return false.
        if has_duplicates(list) {
            return false;
        }
    }
    // 14. Otherwise:
    else {
        // 1. If config["removeAttributes"] has duplicates, then return false.
        if has_duplicates(config.remove_attributes.as_ref().unwrap()) {
            return false;
        }
    }

    // 15. If config["replaceWithChildrenElements"] exists:
    if let Some(replace) = &config.replace_with_children_elements {
        // 1. For each element of config["replaceWithChildrenElements"]:
        for element in replace {
            // 1. If the built-in non-replaceable elements list contains element, then return false.
            if built_in_non_replaceable_elements_list_contains(element) {
                return false;
            }
        }

        // 2. If config["elements"] exists:
        if let Some(elements) = &config.elements {
            // 1. If the intersection of config["elements"] and config["replaceWithChildrenElements"] is not empty, then
            //    return false.
            if elements.iter().any(|element| replace.contains(&element.element)) {
                return false;
            }
        }
        // 3. Otherwise:
        else {
            // 1. If the intersection of config["removeElements"] and config["replaceWithChildrenElements"] is not
            //    empty, then return false.
            if !intersection_is_empty(config.remove_elements.as_ref().unwrap(), replace) {
                return false;
            }
        }
    }

    // 16. If config["attributes"] exists:
    if let Some(attributes) = &config.attributes {
        // 1. Assert: config["dataAttributes"] exists.
        assert!(config.data_attributes.is_some());

        // 2. If config["elements"] exists:
        if let Some(elements) = &config.elements {
            // 1. For each element of config["elements"]:
            for element in elements {
                // 1. If element["attributes"] exists and element["attributes"] has duplicates, then return false.
                if element.attributes.as_ref().is_some_and(|list| has_duplicates(list)) {
                    return false;
                }

                // 2. If element["removeAttributes"] exists and element["removeAttributes"] has duplicates, then return
                //    false.
                if element
                    .remove_attributes
                    .as_ref()
                    .is_some_and(|list| has_duplicates(list))
                {
                    return false;
                }

                // 3. If the intersection of config["attributes"] and element["attributes"] with default « » is not
                //    empty, then return false.
                if element
                    .attributes
                    .as_ref()
                    .is_some_and(|list| !intersection_is_empty(attributes, list))
                {
                    return false;
                }

                // 4. If element["removeAttributes"] with default « » is not a subset of config["attributes"], then
                //    return false.
                if element
                    .remove_attributes
                    .as_ref()
                    .is_some_and(|list| !is_subset(list, attributes))
                {
                    return false;
                }

                // 5. If config["dataAttributes"] is true and element["attributes"] contains a custom data attribute,
                //    then return false.
                if config.data_attributes == Some(true)
                    && element
                        .attributes
                        .as_ref()
                        .is_some_and(|list| contains_custom_data_attribute(list))
                {
                    return false;
                }
            }
        }

        // 3. If config["dataAttributes"] is true and config["attributes"] contains a custom data attribute, then return
        //    false.
        if config.data_attributes == Some(true) && contains_custom_data_attribute(attributes) {
            return false;
        }
    }
    // 17. Otherwise:
    else {
        // 1. If config["elements"] exists:
        if let Some(elements) = &config.elements {
            // 1. For each element of config["elements"]:
            for element in elements {
                // 1. If element["attributes"] exists and element["removeAttributes"] exists, then return false.
                if element.attributes.is_some() && element.remove_attributes.is_some() {
                    return false;
                }

                // 2. If element["attributes"] exists and element["attributes"] has duplicates, then return false.
                if element.attributes.as_ref().is_some_and(|list| has_duplicates(list)) {
                    return false;
                }

                // 3. If element["removeAttributes"] exists and element["removeAttributes"] has duplicates, then return
                //    false.
                if element
                    .remove_attributes
                    .as_ref()
                    .is_some_and(|list| has_duplicates(list))
                {
                    return false;
                }

                // 4. If the intersection of config["removeAttributes"] and element["attributes"] with default « » is
                //    not empty, then return false.
                if element
                    .attributes
                    .as_ref()
                    .is_some_and(|list| !intersection_is_empty(config.remove_attributes.as_ref().unwrap(), list))
                {
                    return false;
                }

                // 5. If the intersection of config["removeAttributes"] and element["removeAttributes"] with default « »
                //    is not empty, then return false.
                if element
                    .remove_attributes
                    .as_ref()
                    .is_some_and(|list| !intersection_is_empty(config.remove_attributes.as_ref().unwrap(), list))
                {
                    return false;
                }
            }
        }

        // 2. If config["dataAttributes"] exists, then return false.
        if config.data_attributes.is_some() {
            return false;
        }
    }

    // 18. Return true.
    true
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#configure-a-sanitizer
pub(crate) fn configure_sanitizer_configuration(
    mut fresh_configuration: SanitizerConfiguration,
    permissive_defaults: PermissiveDefaults,
) -> WebIdlResult<SanitizerConfiguration> {
    // 1. Let freshConfiguration be a clone of configuration.
    // NB: The caller passes a clone.
    // 2. Canonicalize the configuration freshConfiguration with permissiveDefaults.
    canonicalize_the_configuration(&mut fresh_configuration, permissive_defaults);

    // 3. If freshConfiguration is not valid, then throw a TypeError.
    if !sanitizer_configuration_is_valid(&fresh_configuration) {
        return Err(Exception::type_error("Sanitizer configuration is not valid"));
    }

    // 4. Set sanitizer's configuration to freshConfiguration.
    // NB: The caller stores the result.
    Ok(fresh_configuration)
}

fn to_bindings_name(name: &SanitizerName) -> SanitizerElementNamespace {
    SanitizerElementNamespace {
        name: name.name.clone(),
        namespace: name.namespace_.clone(),
    }
}
fn to_bindings_attribute_list(list: &[SanitizerName]) -> Vec<SanitizerAttribute> {
    list.iter()
        .map(|name| {
            SanitizerAttribute::SanitizerAttributeNamespace(SanitizerAttributeNamespace {
                name: name.name.clone(),
                namespace: name.namespace_.clone(),
            })
        })
        .collect()
}
fn to_bindings_element_list(list: &[SanitizerName]) -> Vec<SanitizerElement> {
    list.iter()
        .map(|name| SanitizerElement::SanitizerElementNamespace(to_bindings_name(name)))
        .collect()
}
fn to_bindings_processing_instruction_list(list: &[Utf16String]) -> Vec<SanitizerPI> {
    list.iter()
        .map(|target| {
            SanitizerPI::SanitizerProcessingInstruction(SanitizerProcessingInstruction { target: target.clone() })
        })
        .collect()
}

pub(crate) fn sanitizer_configuration_to_bindings(configuration: &SanitizerConfiguration) -> SanitizerConfig {
    SanitizerConfig {
        elements: configuration.elements.as_ref().map(|elements| {
            elements
                .iter()
                .map(|element| {
                    sanitizer::SanitizerElementWithAttributes::SanitizerElementNamespaceWithAttributes(
                        SanitizerElementNamespaceWithAttributes {
                            parent: to_bindings_name(&element.element),
                            attributes: element.attributes.as_ref().map(|list| to_bindings_attribute_list(list)),
                            remove_attributes: element
                                .remove_attributes
                                .as_ref()
                                .map(|list| to_bindings_attribute_list(list)),
                        },
                    )
                })
                .collect()
        }),
        remove_elements: configuration
            .remove_elements
            .as_ref()
            .map(|list| to_bindings_element_list(list)),
        replace_with_children_elements: configuration
            .replace_with_children_elements
            .as_ref()
            .map(|list| to_bindings_element_list(list)),
        processing_instructions: configuration
            .processing_instructions
            .as_ref()
            .map(|list| to_bindings_processing_instruction_list(list)),
        remove_processing_instructions: configuration
            .remove_processing_instructions
            .as_ref()
            .map(|list| to_bindings_processing_instruction_list(list)),
        attributes: configuration
            .attributes
            .as_ref()
            .map(|list| to_bindings_attribute_list(list)),
        remove_attributes: configuration
            .remove_attributes
            .as_ref()
            .map(|list| to_bindings_attribute_list(list)),
        comments: configuration.comments,
        data_attributes: configuration.data_attributes,
        javascript_ur_ls: configuration.javascript_urls,
    }
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#remove-an-element-from-sanitizer-config
pub(crate) fn remove_an_element_from_sanitizer_config(
    configuration: &mut SanitizerConfiguration,
    element: &SanitizerName,
) -> bool {
    // 1. Assert: configuration is valid.
    // 2. Set element to the result of canonicalizing element.
    // NB: element is canonical.
    // 3. Let modified be the result of removing element from configuration["replaceWithChildrenElements"].
    let modified = sanitizer_list_remove(&mut configuration.replace_with_children_elements, element);

    // 4. If configuration["elements"] exists:
    if configuration.elements.is_some() {
        // 1. If configuration["elements"] contains element:
        if elements_contain(configuration.elements.as_deref(), element) {
            // 1. Remove element from configuration["elements"].
            elements_remove(&mut configuration.elements, element);

            // 2. Return true.
            return true;
        }

        // 2. Return modified.
        return modified;
    }

    // 5. Otherwise:
    // 1. If configuration["removeElements"] contains element, then return modified.
    if configuration.remove_elements.as_ref().unwrap().contains(element) {
        return modified;
    }

    // 2. Append element to configuration["removeElements"].
    configuration.remove_elements.as_mut().unwrap().push(element.clone());

    // 3. Return true.
    true
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#remove-an-attribute-from-sanitizer-config
pub(crate) fn remove_an_attribute_from_sanitizer_config(
    configuration: &mut SanitizerConfiguration,
    attribute: &SanitizerName,
) -> bool {
    // 1. Assert: configuration is valid.
    // 2. Set attribute to the result of canonicalizing attribute.
    // NB: attribute is canonical.
    // 3. If configuration["attributes"] exists:
    if configuration.attributes.is_some() {
        // 1. Let modified be the result of removing attribute from configuration["attributes"].
        let mut modified = sanitizer_list_remove(&mut configuration.attributes, attribute);

        // 2. If configuration["elements"] exists:
        if let Some(elements) = &mut configuration.elements {
            // 1. For each element of configuration["elements"]:
            for element in elements {
                // 1. If element["attributes"] with default « » contains attribute:
                if sanitizer_list_contains(element.attributes.as_deref(), attribute) {
                    // 1. Set modified to true.
                    modified = true;

                    // 2. Remove attribute from element["attributes"].
                    sanitizer_list_remove(&mut element.attributes, attribute);
                }

                // 2. If element["removeAttributes"] with default « » contains attribute:
                if sanitizer_list_contains(element.remove_attributes.as_deref(), attribute) {
                    // 1. Assert: modified is true.
                    assert!(modified);

                    // 2. Remove attribute from element["removeAttributes"].
                    sanitizer_list_remove(&mut element.remove_attributes, attribute);
                }
            }
        }

        // 3. Return modified.
        return modified;
    }

    // 4. Otherwise:
    // 1. If configuration["removeAttributes"] contains attribute, then return false.
    if configuration.remove_attributes.as_ref().unwrap().contains(attribute) {
        return false;
    }

    // 2. If configuration["elements"] exists:
    if let Some(elements) = &mut configuration.elements {
        // 1. For each element in configuration["elements"]:
        for element in elements {
            // 1. If element["attributes"] with default « » contains attribute, then remove attribute from
            //    element["attributes"].
            sanitizer_list_remove(&mut element.attributes, attribute);

            // 2. If element["removeAttributes"] with default « » contains attribute, then remove attribute from
            //    element["removeAttributes"].
            sanitizer_list_remove(&mut element.remove_attributes, attribute);
        }
    }

    // 3. Append attribute to configuration["removeAttributes"].
    configuration
        .remove_attributes
        .as_mut()
        .unwrap()
        .push(attribute.clone());

    // 4. Return true.
    true
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#remove-unsafe
pub(crate) fn remove_unsafe_from_sanitizer_config(configuration: &mut SanitizerConfiguration) -> bool {
    // 1. Assert: configuration is valid.
    // 2. Let result be false.
    let mut result = false;

    // 3. For each element in built-in safe baseline configuration["removeElements"]:
    for element in built_in_safe_baseline_configuration().remove_elements.as_ref().unwrap() {
        // 1. If removing element from configuration is true, then set result to true.
        if remove_an_element_from_sanitizer_config(configuration, element) {
            result = true;
        }
    }

    // 4. For each attribute in built-in safe baseline configuration["removeAttributes"]:
    for attribute in built_in_safe_baseline_configuration()
        .remove_attributes
        .as_ref()
        .unwrap()
    {
        // 1. If removing attribute from configuration is true, then set result to true.
        if remove_an_attribute_from_sanitizer_config(configuration, attribute) {
            result = true;
        }
    }

    // 5. For each attribute that is an event handler content attribute:
    for &attribute in event_handler_content_attributes() {
        // 1. If removing attribute from configuration is true, then set result to true.
        if remove_an_attribute_from_sanitizer_config(
            configuration,
            &SanitizerName {
                // SAFETY: C++ lends its immutable process-lifetime list of event handler names.
                name: unsafe { Utf16String::from_raw_borrowed(attribute) },
                namespace_: None,
            },
        ) {
            result = true;
        }
    }

    // 6. If configuration["javascriptURLs"] is true:
    if configuration.javascript_urls == Some(true) {
        // 1. Set result to true.
        result = true;

        // 2. Set configuration["javascriptURLs"] to false.
        configuration.javascript_urls = Some(false);
    }

    // 7. Return result.
    result
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#sanitizer-config-allows-comments
pub(crate) fn sanitizer_config_allows_comments(configuration: &SanitizerConfiguration) -> bool {
    // Return true if configuration["comments"] is true; otherwise false.
    configuration.comments == Some(true)
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#sanitizer-config-allows-processing-instruction-target
pub(crate) fn sanitizer_config_allows_processing_instruction_target(
    pi_target: Utf16View<'_>,
    configuration: &SanitizerConfiguration,
) -> bool {
    // 1. Let pi be «[ "target" → piTarget ]».
    let contains = |list: &[Utf16String]| {
        list.iter()
            .any(|target| Utf16View::of_string(target).code_units().eq(pi_target.code_units()))
    };

    // 2. If configuration["processingInstructions"] exists, then return whether
    //    configuration["processingInstructions"] contains pi.
    if let Some(list) = &configuration.processing_instructions {
        return contains(list);
    }

    // 3. If configuration["removeProcessingInstructions"] exists and configuration["removeProcessingInstructions"]
    //    contains pi, then return false.
    if configuration
        .remove_processing_instructions
        .as_ref()
        .is_some_and(|list| contains(list))
    {
        return false;
    }

    // 4. Return true.
    true
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#get-the-sanitizer-action-for-an-element-name
pub(crate) fn sanitizer_action_for_element_name(
    local_name: &Utf16String,
    namespace_: Option<&Utf16String>,
    configuration: &SanitizerConfiguration,
) -> SanitizerAction {
    // 1. Let elementName be «[ "name" → localName, "namespace" → namespace ]».
    let element_name = SanitizerName {
        name: local_name.clone(),
        namespace_: namespace_.cloned(),
    };

    // 2. If configuration["replaceWithChildrenElements"] exists and configuration["replaceWithChildrenElements"]
    //    contains elementName, then return "Replace with children".
    if sanitizer_list_contains(configuration.replace_with_children_elements.as_deref(), &element_name) {
        return SanitizerAction::ReplaceWithChildren;
    }

    // 3. If configuration["elements"] exists:
    if configuration.elements.is_some() {
        // 1. If there is no item in configuration["elements"] whose name member is localName and whose namespace
        //    member is namespace, then return "Remove".
        if find_element(configuration.elements.as_deref(), local_name, namespace_).is_none() {
            return SanitizerAction::Remove;
        }
    }
    // 4. Otherwise, if configuration["removeElements"] contains elementName, then return "Remove".
    else if sanitizer_list_contains(configuration.remove_elements.as_deref(), &element_name) {
        return SanitizerAction::Remove;
    }

    // 5. Return "Keep".
    SanitizerAction::Keep
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#contains-a-javascript:-url
fn contains_a_javascript_url(value: Utf16View<'_>) -> bool {
    // 1. Let url be the result of running the basic URL parser on value.
    let input = match value {
        Utf16View::Ascii(bytes) | Utf16View::Latin1(bytes) => UrlInput::Latin1(bytes),
        Utf16View::Utf16(units) => UrlInput::Utf16(units),
    };
    let url = basic_parse(input, BasicParseOptions::new());

    // 2. If url is failure, then return false.
    let Some(url) = url else {
        return false;
    };

    // 3. Return true if url's scheme is "javascript", and false otherwise.
    url.scheme() == "javascript"
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#built-in-navigating-url-attributes-list
fn built_in_navigating_url_attributes_list_matches(
    element_name: &SanitizerName,
    attribute_name: &SanitizerName,
) -> bool {
    // The built-in navigating URL attributes list corresponds to all HTML elements marked with navigating URL
    // attributes in their normative definitions, as well as the element-attribute pairs represented in the following
    // table.
    const LIST: &[(&str, &str, &str, Option<&str>)] = &[
        ("a", HTML, "href", None),
        ("area", HTML, "href", None),
        ("form", HTML, "action", None),
        ("input", HTML, "formaction", None),
        ("button", HTML, "formaction", None),
        ("a", SVG, "href", None),
        ("a", SVG, "href", Some(XLINK)),
    ];
    for &(element, namespace_, attribute, attribute_namespace) in LIST {
        if element_name.name != element
            || element_name
                .namespace_
                .as_ref()
                .is_none_or(|value| *value != namespace_)
        {
            continue;
        }
        if attribute_name.name != attribute {
            continue;
        }
        if let Some(namespace_) = attribute_namespace {
            if attribute_name
                .namespace_
                .as_ref()
                .is_some_and(|value| *value == namespace_)
            {
                return true;
            }
        } else if attribute_name.namespace_.is_none() {
            return true;
        }
    }
    false
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#built-in-animating-url-attributes-list
fn built_in_animating_url_attributes_list_contains(
    element_name: &SanitizerName,
    attribute_name: &SanitizerName,
) -> bool {
    if element_name.namespace_.as_ref().is_none_or(|value| *value != SVG) || attribute_name.namespace_.is_some() {
        return false;
    }
    if attribute_name.name != "attributeName" {
        return false;
    }
    ["animate", "animateTransform", "set"]
        .iter()
        .any(|name| element_name.name == *name)
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#sanitizer-config-allows-an-attribute
pub(crate) fn sanitizer_config_allows_attribute(
    local_name: &Utf16String,
    namespace_: Option<&Utf16String>,
    value: Utf16View<'_>,
    element_local_name: &Utf16String,
    element_namespace: Option<&Utf16String>,
    configuration: &SanitizerConfiguration,
) -> bool {
    // 1. Let attrName be «[ "name" → localName, "namespace" → namespace ]».
    let attribute_name = SanitizerName {
        name: local_name.clone(),
        namespace_: namespace_.cloned(),
    };

    // 2. Let elementName be «[ "name" → elementLocalName, "namespace" → elementNamespace ]».
    let element_name = SanitizerName {
        name: element_local_name.clone(),
        namespace_: element_namespace.cloned(),
    };

    // 3. Let elementWithLocalAttributes be «[ ]».
    // 4. If configuration["elements"] exists and there is an item element in configuration["elements"] whose name
    //    member is elementLocalName and whose namespace member is elementNamespace, then set
    //    elementWithLocalAttributes to element.
    let element_with_local_attributes =
        find_element(configuration.elements.as_deref(), element_local_name, element_namespace);
    let no_attributes = None;
    let local_attributes = element_with_local_attributes.map_or(&no_attributes, |element| &element.attributes);
    let local_remove_attributes =
        element_with_local_attributes.map_or(&no_attributes, |element| &element.remove_attributes);

    // 5. If elementWithLocalAttributes["removeAttributes"] exists and elementWithLocalAttributes["removeAttributes"]
    //    contains attrName, then return false.
    if sanitizer_list_contains(local_remove_attributes.as_deref(), &attribute_name) {
        return false;
    }
    // 6. Otherwise, if configuration["attributes"] exists:
    else if let Some(attributes) = &configuration.attributes {
        // 1. If configuration["attributes"] does not contain attrName and elementWithLocalAttributes["attributes"]
        //    with default « » does not contain attrName, and if "data-" is not a code unit prefix of localName or
        //    namespace is not null or configuration["dataAttributes"] is not true, then return false.
        if !attributes.contains(&attribute_name)
            && !sanitizer_list_contains(local_attributes.as_deref(), &attribute_name)
            && (!Utf16View::of_string(local_name).starts_with(Utf16View::Ascii(b"data-"))
                || namespace_.is_some()
                || configuration.data_attributes != Some(true))
        {
            return false;
        }
    }
    // 7. Otherwise:
    else {
        // 1. If elementWithLocalAttributes["attributes"] exists and elementWithLocalAttributes["attributes"] does not
        //    contain attrName, then return false.
        if local_attributes
            .as_ref()
            .is_some_and(|list| !list.contains(&attribute_name))
        {
            return false;
        }

        // 2. Otherwise, if configuration["removeAttributes"] exists and configuration["removeAttributes"] contains
        //    attrName, then return false.
        if sanitizer_list_contains(configuration.remove_attributes.as_deref(), &attribute_name) {
            return false;
        }
    }

    // 8. If configuration["javascriptURLs"] is true, then return true.
    if configuration.javascript_urls == Some(true) {
        return true;
    }

    // 9. If the pair (elementName, attrName) matches an entry in the built-in navigating URL attributes list, and if
    //    value contains a javascript: URL, then return false.
    if built_in_navigating_url_attributes_list_matches(&element_name, &attribute_name)
        && contains_a_javascript_url(value)
    {
        return false;
    }

    // 10. If elementNamespace is the MathML namespace, localName is "href", namespace is null or the XLink namespace,
    //     and value contains a javascript: URL, then return false.
    if element_namespace.is_some_and(|value| *value == MATHML)
        && *local_name == "href"
        && namespace_.is_none_or(|value| *value == XLINK)
        && contains_a_javascript_url(value)
    {
        return false;
    }

    // 11. If the built-in animating URL attributes list contains the pair (elementName, attrName), and value is "href"
    //     or "xlink:href", then return false.
    if built_in_animating_url_attributes_list_contains(&element_name, &attribute_name)
        && (value == "href" || value == "xlink:href")
    {
        return false;
    }

    // 12. Return true.
    true
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#built-in-non-replaceable-elements-list
pub(crate) fn built_in_non_replaceable_elements_list_contains(element: &SanitizerName) -> bool {
    let Some(namespace_) = &element.namespace_ else {
        return false;
    };
    (element.name == "html" && *namespace_ == HTML)
        || (element.name == "svg" && *namespace_ == SVG)
        || (element.name == "math" && *namespace_ == MATHML)
}

fn name_with_namespace(name: &str, namespace_: Option<&str>) -> SanitizerName {
    SanitizerName {
        name: Utf16String::from_utf8(name),
        namespace_: namespace_.map(Utf16String::from_utf8),
    }
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#built-in-safe-baseline-configuration
pub(crate) fn built_in_safe_baseline_configuration() -> &'static SanitizerConfiguration {
    // The built-in safe baseline configuration is a SanitizerConfig. Its removeElements list consists of all HTML
    // elements normatively marked as Unsafe within their individual definitions, along with the obsolete frame
    // element, and the SVG script and SVG use elements, and its removeAttributes list is empty.
    static CONFIGURATION: OnceLock<SanitizerConfiguration> = OnceLock::new();
    CONFIGURATION.get_or_init(|| SanitizerConfiguration {
        remove_elements: Some(vec![
            name_with_namespace("base", Some(HTML)),
            name_with_namespace("embed", Some(HTML)),
            name_with_namespace("frame", Some(HTML)),
            name_with_namespace("iframe", Some(HTML)),
            name_with_namespace("object", Some(HTML)),
            name_with_namespace("script", Some(HTML)),
            name_with_namespace("script", Some(SVG)),
            name_with_namespace("use", Some(SVG)),
        ]),
        remove_attributes: Some(Vec::new()),
        ..Default::default()
    })
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#built-in-safe-default-configuration
pub(crate) fn built_in_safe_default_configuration() -> &'static SanitizerConfiguration {
    static CONFIGURATION: OnceLock<SanitizerConfiguration> = OnceLock::new();
    CONFIGURATION.get_or_init(|| {
        let attribute_list = |attributes: &str| attributes.split(' ').filter(|name| !name.is_empty()).map(|name| name_with_namespace(name, None)).collect::<Vec<_>>();
        let mut configuration = SanitizerConfiguration::default();
        // elements
        //     All HTML elements normatively categorized as Default within their individual definitions, alongside the
        //     MathML and SVG elements listed in the table below. Attributes included in those definitions are included
        //     in each element's respective attributes list.
        const HTML_ELEMENTS: &[(&str, &str)] = &[
            ("html", ""),
            ("head", ""),
            ("title", ""),
            ("body", ""),
            ("article", ""),
            ("section", ""),
            ("nav", ""),
            ("aside", ""),
            ("h1", ""),
            ("h2", ""),
            ("h3", ""),
            ("h4", ""),
            ("h5", ""),
            ("h6", ""),
            ("hgroup", ""),
            ("header", ""),
            ("footer", ""),
            ("address", ""),
            ("p", ""),
            ("hr", ""),
            ("pre", ""),
            ("blockquote", "cite"),
            ("ol", "reversed start type"),
            ("ul", ""),
            ("menu", ""),
            ("li", "value"),
            ("dl", ""),
            ("dt", ""),
            ("dd", ""),
            ("figure", ""),
            ("figcaption", ""),
            ("main", ""),
            ("search", ""),
            ("div", ""),
            ("a", "href hreflang type"),
            ("em", ""),
            ("strong", ""),
            ("small", ""),
            ("s", ""),
            ("cite", ""),
            ("q", ""),
            ("dfn", ""),
            ("abbr", ""),
            ("ruby", ""),
            ("rt", ""),
            ("rp", ""),
            ("data", "value"),
            ("time", "datetime"),
            ("code", ""),
            ("var", ""),
            ("samp", ""),
            ("kbd", ""),
            ("sub", ""),
            ("sup", ""),
            ("i", ""),
            ("b", ""),
            ("u", ""),
            ("mark", ""),
            ("bdi", ""),
            ("bdo", ""),
            ("span", ""),
            ("br", ""),
            ("wbr", ""),
            ("ins", "cite datetime"),
            ("del", "cite datetime"),
            ("table", ""),
            ("caption", ""),
            ("colgroup", "span"),
            ("col", "span"),
            ("tbody", ""),
            ("thead", ""),
            ("tfoot", ""),
            ("tr", ""),
            ("td", "colspan headers rowspan"),
            ("th", "abbr colspan headers rowspan scope"),
        ];
        const MATHML_ELEMENTS: &[(&str, &str)] = &[
            ("a", "href hreflang type"),
            ("math", ""),
            ("merror", ""),
            ("mfrac", ""),
            ("mi", ""),
            ("mmultiscripts", ""),
            ("mn", ""),
            ("mo", "fence form largeop lspace maxsize minsize movablelimits rspace separator stretchy symmetric"),
            ("mover", "accent"),
            ("mpadded", "depth height lspace voffset width"),
            ("mphantom", ""),
            ("mprescripts", ""),
            ("mroot", ""),
            ("mrow", ""),
            ("ms", ""),
            ("mspace", "depth height width"),
            ("msqrt", ""),
            ("mstyle", ""),
            ("msub", ""),
            ("msubsup", ""),
            ("msup", ""),
            ("mtable", ""),
            ("mtd", "columnspan rowspan"),
            ("mtext", ""),
            ("mtr", ""),
            ("munder", "accentunder"),
            ("munderover", "accent accentunder"),
            ("semantics", ""),
        ];
        const SVG_ELEMENTS: &[(&str, &str)] = &[
            ("a", "href hreflang type"),
            ("circle", "cx cy pathLength r"),
            ("defs", ""),
            ("desc", ""),
            ("ellipse", "cx cy pathLength rx ry"),
            ("foreignObject", "height width x y"),
            ("g", ""),
            ("line", "pathLength x1 x2 y1 y2"),
            ("marker", "markerHeight markerUnits markerWidth orient preserveAspectRatio refX refY viewBox"),
            ("metadata", ""),
            ("path", "d pathLength"),
            ("polygon", "pathLength points"),
            ("polyline", "pathLength points"),
            ("rect", "height pathLength rx ry width x y"),
            ("svg", "height preserveAspectRatio viewBox width x y"),
            ("text", "dx dy lengthAdjust rotate textLength x y"),
            ("textPath", "lengthAdjust method path side spacing startOffset textLength"),
            ("title", ""),
            ("tspan", "dx dy lengthAdjust rotate textLength x y"),
        ];
        let mut elements = Vec::new();
        for (list, namespace_) in [(HTML_ELEMENTS, HTML), (MATHML_ELEMENTS, MATHML), (SVG_ELEMENTS, SVG)] {
            for &(name, attributes) in list {
                elements.push(SanitizerElementWithAttributes { element: name_with_namespace(name, Some(namespace_)), attributes: Some(attribute_list(attributes)), remove_attributes: None });
            }
        }
        configuration.elements = Some(elements);
        // processingInstructions
        //     « »
        configuration.processing_instructions = Some(Vec::new());
        // attributes
        configuration.attributes = Some(attribute_list("dir lang title alignment-baseline baseline-shift clip-path clip-rule color color-interpolation cursor direction display displaystyle dominant-baseline fill fill-opacity fill-rule font-family font-size font-size-adjust font-stretch font-style font-variant font-weight letter-spacing marker-end marker-mid marker-start mathbackground mathcolor mathsize opacity paint-order pointer-events scriptlevel shape-rendering stop-color stop-opacity stroke stroke-dasharray stroke-dashoffset stroke-linecap stroke-linejoin stroke-miterlimit stroke-opacity stroke-width text-anchor text-decoration text-overflow text-rendering transform transform-origin unicode-bidi vector-effect visibility white-space word-spacing writing-mode"));
        // comments
        //     false
        configuration.comments = Some(false);
        // dataAttributes
        //     false
        configuration.data_attributes = Some(false);
        // javascriptURLs
        //     false
        configuration.javascript_urls = Some(false);
        configuration
    })
}

#[derive(libjs_rust::api::Trace)]
#[gc(crate = libjs_rust)]
pub(crate) enum SanitizerInput {
    Sanitizer(Ref<Sanitizer>),
    Config(#[gc(untraced)] SanitizerConfig),
    Preset(#[gc(untraced)] SanitizerPresets),
}

#[derive(libjs_rust::api::Trace)]
#[gc(crate = libjs_rust)]
pub(crate) enum SanitizerOptions {
    Dictionary(Option<SanitizerInput>),
    Trusted(#[gc(untraced)] Option<Arc<SanitizerConfiguration>>),
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#get-a-sanitizer-config-from-options
pub(crate) fn get_a_sanitizer_config_from_options<'a>(
    options: &'a SanitizerOptions,
    safe: bool,
) -> WebIdlResult<Option<SanitizerConfiguration>> {
    enum ConfigurationInput<'a> {
        Empty,
        Sanitizer(&'a Sanitizer),
        Dictionary(&'a SanitizerConfig),
        Configuration(&'a SanitizerConfiguration),
    }

    // 1. Let sanitizerInput be null.
    let mut sanitizer_input = ConfigurationInput::Empty;
    let set_sanitizer_input = |sanitizer: &'a SanitizerInput| {
        match sanitizer {
            SanitizerInput::Config(config) => ConfigurationInput::Dictionary(config),
            SanitizerInput::Preset(preset) => {
                // 1. Assert: sanitizerInput is "default".
                assert!(*preset == SanitizerPresets::Default);

                // 2. Set sanitizerInput to the built-in safe default configuration.
                ConfigurationInput::Configuration(built_in_safe_default_configuration())
            }
            SanitizerInput::Sanitizer(sanitizer) => ConfigurationInput::Sanitizer(sanitizer),
        }
    };

    // 2. If options is a TrustedHTMLParserOptions:
    if let SanitizerOptions::Trusted(configuration) = options {
        // 1. If options's sanitizer configuration is null:
        if configuration.is_none() {
            // 1. Assert: safe is false.
            assert!(!safe);

            // 2. Return null.
            return Ok(None);
        }

        // 2. Set sanitizerInput to options's sanitizer configuration.
        sanitizer_input = ConfigurationInput::Configuration(configuration.as_ref().unwrap());
    }

    // 3. Otherwise, if options["sanitizer"] exists, then set sanitizerInput to options["sanitizer"].
    let has_sanitizer = match options {
        SanitizerOptions::Trusted(_) => true,
        SanitizerOptions::Dictionary(Some(input)) => {
            sanitizer_input = set_sanitizer_input(input);
            true
        }
        SanitizerOptions::Dictionary(None) => false,
    };
    if !has_sanitizer {
        // 4. Otherwise, if safe is false, then return null.
        if !safe {
            return Ok(None);
        }

        // 5. Otherwise, set sanitizerInput to "default".
        // NB: The following step sets sanitizerInput to the built-in safe default configuration.
        sanitizer_input = ConfigurationInput::Configuration(built_in_safe_default_configuration());
    }

    // 6. Assert: sanitizerInput is either a Sanitizer instance, a SanitizerPresets member, or a SanitizerConfig
    //    dictionary.
    // 7. If sanitizerInput is a string:
    //    1. Assert: sanitizerInput is "default".
    //    2. Set sanitizerInput to the built-in safe default configuration.
    // NB: This was done above.
    // 8. If sanitizerInput is a Sanitizer, then set sanitizerInput to sanitizerInput's configuration.
    // 9. Assert: sanitizerInput is a dictionary.
    let clone = match sanitizer_input {
        ConfigurationInput::Empty => unreachable!("sanitizer input has been selected"),
        ConfigurationInput::Sanitizer(sanitizer) => sanitizer.configuration(),
        ConfigurationInput::Dictionary(config) => canonicalize_sanitizer_config(config),
        ConfigurationInput::Configuration(config) => config.clone(),
    };

    // 10. Let sanitizer be a new Sanitizer object.
    // 11. Let permissiveDefaults be true if safe is false; false otherwise.
    let permissive_defaults = if safe {
        PermissiveDefaults::No
    } else {
        PermissiveDefaults::Yes
    };

    // 12. Configure sanitizer given sanitizerInput and permissiveDefaults.
    // 13. Let configuration be sanitizer's configuration.
    let mut configuration = configure_sanitizer_configuration(clone, permissive_defaults)?;

    // 14. If safe is true, then remove unsafe from configuration.
    if safe {
        remove_unsafe_from_sanitizer_config(&mut configuration);
    }

    // 15. Assert: configuration is valid.
    assert!(sanitizer_configuration_is_valid(&configuration));

    // 16. Return configuration.
    // NOTE: The returned SanitizerConfig is a fresh copy, so removing unsafe items from it never affects a Sanitizer
    //       object or SanitizerConfig dictionary that script can observe.
    Ok(Some(configuration))
}

// https://w3c.github.io/trusted-types/dist/spec/#get-a-sanitizer-from-dictionary
// NB: The steps of "get a sanitizer from dictionary" after sanitizerInput has been found to be non-null.
pub(crate) fn get_a_sanitizer_from_sanitizer_input(
    vm: &libjs_rust::api::Vm,
    sanitizer_input: &SanitizerInput,
) -> WebIdlResult<Ref<Sanitizer>> {
    // 3. Let configuration be null.
    let configuration = match sanitizer_input {
        // 4. If sanitizerInput is a SanitizerPresets:
        SanitizerInput::Preset(preset) => {
            // 1. Assert: sanitizerInput is "default".
            assert!(*preset == SanitizerPresets::Default);

            // 2. Set configuration to the built-in safe default configuration.
            built_in_safe_default_configuration().clone()
        }

        // 5. Otherwise, if sanitizerInput is a Sanitizer:
        //    1. Set configuration to sanitizerInput's configuration.
        SanitizerInput::Sanitizer(sanitizer) => sanitizer.configuration(),

        // 6. Otherwise, if sanitizerInput is a SanitizerConfig:
        //    1. Set configuration to sanitizerInput.
        SanitizerInput::Config(config) => canonicalize_sanitizer_config(config),
    };

    // 7. Assert: configuration is not null.
    // 8. Let sanitizer be a new Sanitizer in realm.
    // 9. Configure sanitizer given configuration and true.
    let canonical_configuration = configure_sanitizer_configuration(configuration, PermissiveDefaults::Yes)?;

    // 10. Return sanitizer.
    Ok(Sanitizer::create_with_configuration(vm, canonical_configuration))
}

// NB: SanitizerInputKind in SanitizerConfiguration.cpp matches this.
#[derive(Clone, Copy)]
#[repr(u8)]
#[allow(dead_code, reason = "C++ constructs the variants")]
pub(crate) enum SanitizerInputKind {
    None,
    DefaultPreset,
    Config,
    Sanitizer,
    TrustedOptions,
}

// NB: SanitizerConfigurationStatus in SanitizerConfiguration.cpp matches this.
#[repr(u8)]
pub(crate) enum SanitizerConfigurationStatus {
    Null,
    Configuration,
    Invalid,
}

// NB: SanitizerOptionalBoolean in SanitizerConfiguration.cpp matches this.
#[derive(Clone, Copy)]
#[repr(u8)]
#[allow(dead_code, reason = "C++ constructs the variants")]
pub(crate) enum OptionalBooleanAbi {
    Absent,
    False,
    True,
}

impl From<OptionalBooleanAbi> for Option<bool> {
    fn from(value: OptionalBooleanAbi) -> Self {
        match value {
            OptionalBooleanAbi::Absent => None,
            OptionalBooleanAbi::False => Some(false),
            OptionalBooleanAbi::True => Some(true),
        }
    }
}

#[repr(C)]
pub(crate) struct NameInputAbi {
    is_string: bool,
    name: usize,
    namespace_: usize,
}

#[repr(C)]
pub(crate) struct ListInputAbi<'a, T> {
    present: bool,
    values: ak::Span<'a, T>,
}

#[repr(C)]
pub(crate) struct ElementInputAbi<'a> {
    name: NameInputAbi,
    attributes: ListInputAbi<'a, NameInputAbi>,
    remove_attributes: ListInputAbi<'a, NameInputAbi>,
}

#[repr(C)]
pub(crate) struct ConfigInputAbi<'a> {
    elements: ListInputAbi<'a, ElementInputAbi<'a>>,
    remove_elements: ListInputAbi<'a, NameInputAbi>,
    replace_with_children_elements: ListInputAbi<'a, NameInputAbi>,
    processing_instructions: ListInputAbi<'a, NameInputAbi>,
    remove_processing_instructions: ListInputAbi<'a, NameInputAbi>,
    attributes: ListInputAbi<'a, NameInputAbi>,
    remove_attributes: ListInputAbi<'a, NameInputAbi>,
    comments: OptionalBooleanAbi,
    data_attributes: OptionalBooleanAbi,
    javascript_urls: OptionalBooleanAbi,
}

impl NameInputAbi {
    unsafe fn name(&self) -> Utf16String {
        // SAFETY: C++ lends a live string for the conversion.
        unsafe { Utf16String::from_raw_borrowed(self.name) }
    }
    unsafe fn namespace(&self) -> Option<Utf16String> {
        // SAFETY: A nonzero namespace is a live string lent by C++.
        (self.namespace_ != 0).then(|| unsafe { Utf16String::from_raw_borrowed(self.namespace_) })
    }
    unsafe fn element(&self) -> SanitizerElement {
        // SAFETY: C++ lends the strings for the conversion.
        let name = unsafe { self.name() };
        if self.is_string {
            SanitizerElement::DOMString(name)
        } else {
            SanitizerElement::SanitizerElementNamespace(SanitizerElementNamespace {
                name,
                // SAFETY: C++ lends the namespace for the conversion.
                namespace: unsafe { self.namespace() },
            })
        }
    }
    unsafe fn attribute(&self) -> SanitizerAttribute {
        // SAFETY: C++ lends the strings for the conversion.
        let name = unsafe { self.name() };
        if self.is_string {
            SanitizerAttribute::DOMString(name)
        } else {
            SanitizerAttribute::SanitizerAttributeNamespace(SanitizerAttributeNamespace {
                name,
                // SAFETY: C++ lends the namespace for the conversion.
                namespace: unsafe { self.namespace() },
            })
        }
    }
    unsafe fn pi(&self) -> SanitizerPI {
        // SAFETY: C++ lends the target for the conversion.
        let target = unsafe { self.name() };
        if self.is_string {
            SanitizerPI::DOMString(target)
        } else {
            SanitizerPI::SanitizerProcessingInstruction(SanitizerProcessingInstruction { target })
        }
    }
}

impl<T> ListInputAbi<'_, T> {
    fn convert<R>(&self, convert: impl FnMut(&T) -> R) -> Option<Vec<R>> {
        self.present
            .then(|| self.values.as_slice().iter().map(convert).collect())
    }
}

impl ConfigInputAbi<'_> {
    /// Copies the lent view into the Rust dictionary.
    ///
    /// # Safety
    ///
    /// Every list buffer and string of the view must be live for the call.
    unsafe fn config(&self) -> SanitizerConfig {
        // SAFETY: C++ lends all list buffers and strings until conversion completes.
        unsafe {
            SanitizerConfig {
                elements: self.elements.convert(|element| {
                    if element.name.is_string {
                        sanitizer::SanitizerElementWithAttributes::DOMString(element.name.name())
                    } else {
                        sanitizer::SanitizerElementWithAttributes::SanitizerElementNamespaceWithAttributes(
                            SanitizerElementNamespaceWithAttributes {
                                parent: SanitizerElementNamespace {
                                    name: element.name.name(),
                                    namespace: element.name.namespace(),
                                },
                                attributes: element.attributes.convert(|name| name.attribute()),
                                remove_attributes: element.remove_attributes.convert(|name| name.attribute()),
                            },
                        )
                    }
                }),
                remove_elements: self.remove_elements.convert(|name| name.element()),
                replace_with_children_elements: self.replace_with_children_elements.convert(|name| name.element()),
                processing_instructions: self.processing_instructions.convert(|name| name.pi()),
                remove_processing_instructions: self.remove_processing_instructions.convert(|name| name.pi()),
                attributes: self.attributes.convert(|name| name.attribute()),
                remove_attributes: self.remove_attributes.convert(|name| name.attribute()),
                comments: self.comments.into(),
                data_attributes: self.data_attributes.into(),
                javascript_ur_ls: self.javascript_urls.into(),
            }
        }
    }
}

unsafe fn input_from_abi(kind: SanitizerInputKind, input: *const c_void) -> SanitizerInput {
    match kind {
        SanitizerInputKind::DefaultPreset => SanitizerInput::Preset(SanitizerPresets::Default),
        // SAFETY: C++ passes a live view whose buffers and strings outlive this conversion.
        SanitizerInputKind::Config => {
            SanitizerInput::Config(unsafe { (&*input.cast::<ConfigInputAbi<'_>>()).config() })
        }
        // SAFETY: C++ passes a live Sanitizer cell.
        SanitizerInputKind::Sanitizer => SanitizerInput::Sanitizer(unsafe { cell_from_abi(input.cast_mut()) }),
        SanitizerInputKind::None | SanitizerInputKind::TrustedOptions => {
            unreachable!("C++ passes a sanitizer input kind")
        }
    }
}

/// Retains an immutable configuration snapshot.
///
/// # Safety
///
/// `configuration` must be a live Arc-owned SanitizerConfiguration.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_sanitizer_configuration_retain(configuration: *const SanitizerConfiguration) {
    // SAFETY: The caller keeps an Arc reference alive for this call.
    unsafe { Arc::increment_strong_count(configuration) };
}

/// Releases an immutable configuration snapshot.
///
/// # Safety
///
/// `configuration` must be null or an Arc-owned SanitizerConfiguration reference handed to the caller.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_sanitizer_configuration_release(configuration: *const SanitizerConfiguration) {
    if !configuration.is_null() {
        // SAFETY: C++ hands back its owned Arc reference.
        drop(unsafe { Arc::from_raw(configuration) });
    }
}

/// Obtains a configuration from converted HTML options.
///
/// # Safety
///
/// `input` must be the live configuration view, Sanitizer cell, or snapshot identified by `kind`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_get_a_sanitizer_config_from_options(
    kind: SanitizerInputKind,
    input: *const c_void,
    safe: bool,
    out_configuration: &mut *const SanitizerConfiguration,
) -> SanitizerConfigurationStatus {
    let options = match kind {
        SanitizerInputKind::TrustedOptions => {
            let snapshot = if input.is_null() {
                None
            } else {
                let pointer = input.cast::<SanitizerConfiguration>();
                // SAFETY: C++ holds a live Arc snapshot. This acquires a reference for the options.
                unsafe {
                    Arc::increment_strong_count(pointer);
                    Some(Arc::from_raw(pointer))
                }
            };
            SanitizerOptions::Trusted(snapshot)
        }
        SanitizerInputKind::None => SanitizerOptions::Dictionary(None),
        // SAFETY: The caller lends the indicated input until this function returns.
        _ => SanitizerOptions::Dictionary(Some(unsafe { input_from_abi(kind, input) })),
    };
    match get_a_sanitizer_config_from_options(&options, safe) {
        Ok(Some(configuration)) => {
            *out_configuration = Arc::into_raw(Arc::new(configuration));
            SanitizerConfigurationStatus::Configuration
        }
        Ok(None) => SanitizerConfigurationStatus::Null,
        Err(_) => SanitizerConfigurationStatus::Invalid,
    }
}

/// Obtains a Sanitizer from converted input.
///
/// # Safety
///
/// `input` must be the live configuration view or Sanitizer cell identified by `kind`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_get_a_sanitizer_from_sanitizer_input(
    vm: &libjs_rust::api::Vm,
    kind: SanitizerInputKind,
    input: *const c_void,
) -> *mut c_void {
    // SAFETY: The caller lends the indicated input until this function returns.
    let input = unsafe { input_from_abi(kind, input) };
    match get_a_sanitizer_from_sanitizer_input(vm, &input) {
        Ok(sanitizer) => cell_into_abi(sanitizer),
        Err(_) => core::ptr::null_mut(),
    }
}

#[repr(C)]
pub(crate) struct StringViewAbi<'a> {
    latin1: ak::Span<'a, u8>,
    utf16: ak::Span<'a, u16>,
}

impl StringViewAbi<'_> {
    fn view(&self) -> Utf16View<'_> {
        if self.utf16.as_slice().is_empty() {
            Utf16View::Latin1(self.latin1.as_slice())
        } else {
            Utf16View::Utf16(self.utf16.as_slice())
        }
    }
}

unsafe extern "C" {
    fn web_sanitizer_event_handler_content_attributes(out: &mut ak::Span<'static, usize>);
}

fn event_handler_content_attributes() -> &'static [usize] {
    let mut names = ak::Span::default();
    // SAFETY: C++ lends its immutable process-lifetime list of live string representations.
    unsafe { web_sanitizer_event_handler_content_attributes(&mut names) };
    names.as_slice()
}

/// Checks whether the configuration allows comments.
///
/// # Safety
///
/// `configuration` must point to a live SanitizerConfiguration.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_sanitizer_config_allows_comments(configuration: &SanitizerConfiguration) -> bool {
    sanitizer_config_allows_comments(configuration)
}

/// Checks whether the configuration allows a processing-instruction target.
///
/// # Safety
///
/// `configuration` and the target's buffers must be live for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_sanitizer_config_allows_processing_instruction_target(
    configuration: &SanitizerConfiguration,
    target: StringViewAbi<'_>,
) -> bool {
    sanitizer_config_allows_processing_instruction_target(target.view(), configuration)
}

/// Returns the sanitizer action for the element name.
///
/// # Safety
///
/// The configuration and nonzero string representations must be live for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_sanitizer_action_for_element_name(
    configuration: &SanitizerConfiguration,
    name: usize,
    namespace_: usize,
) -> SanitizerAction {
    // SAFETY: C++ lends the strings for the query.
    let (name, namespace_) = unsafe {
        (
            Utf16String::from_raw_borrowed(name),
            (namespace_ != 0).then(|| Utf16String::from_raw_borrowed(namespace_)),
        )
    };
    sanitizer_action_for_element_name(&name, namespace_.as_ref(), configuration)
}

/// Checks whether the configuration allows an attribute.
///
/// # Safety
///
/// The configuration, value buffers, and nonzero string representations must be live for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_sanitizer_config_allows_attribute(
    configuration: &SanitizerConfiguration,
    name: usize,
    namespace_: usize,
    value: StringViewAbi<'_>,
    element_name: usize,
    element_namespace: usize,
) -> bool {
    // SAFETY: C++ lends all strings for the query.
    let (name, namespace_, element_name, element_namespace) = unsafe {
        (
            Utf16String::from_raw_borrowed(name),
            (namespace_ != 0).then(|| Utf16String::from_raw_borrowed(namespace_)),
            Utf16String::from_raw_borrowed(element_name),
            (element_namespace != 0).then(|| Utf16String::from_raw_borrowed(element_namespace)),
        )
    };
    sanitizer_config_allows_attribute(
        &name,
        namespace_.as_ref(),
        value.view(),
        &element_name,
        element_namespace.as_ref(),
        configuration,
    )
}
