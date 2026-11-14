/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::RefCell;
use core::ffi::c_void;
use std::sync::Arc;

use ak::Utf16String;
use libjs_rust::api::{Trace, Utf16View, Vm};

use super::sanitizer_configuration::{
    PermissiveDefaults, SanitizerConfiguration, SanitizerName, built_in_non_replaceable_elements_list_contains,
    built_in_safe_default_configuration, canonicalize_sanitizer_attribute, canonicalize_sanitizer_config,
    canonicalize_sanitizer_element, canonicalize_sanitizer_element_with_attributes,
    canonicalize_sanitizer_processing_instruction, configure_sanitizer_configuration, is_custom_data_attribute,
    remove_an_attribute_from_sanitizer_config, remove_an_element_from_sanitizer_config,
    remove_unsafe_from_sanitizer_config, sanitizer_configuration_is_valid, sanitizer_configuration_to_bindings,
    sanitizer_list_contains, sanitizer_list_remove,
};
use crate::bindings::interface::{Ref, Wrappable, allocate, cell_from_abi};
use crate::bindings::{CallContext, WebIdlResult};

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum SanitizerPresets {
    Default,
}

#[derive(Clone)]
pub(crate) struct SanitizerElementNamespace {
    pub(crate) name: Utf16String,
    pub(crate) namespace: Option<Utf16String>,
}

#[derive(Clone)]
pub(crate) struct SanitizerElementNamespaceWithAttributes {
    pub(crate) parent: SanitizerElementNamespace,
    pub(crate) attributes: Option<Vec<SanitizerAttribute>>,
    pub(crate) remove_attributes: Option<Vec<SanitizerAttribute>>,
}

#[derive(Clone)]
pub(crate) struct SanitizerAttributeNamespace {
    pub(crate) name: Utf16String,
    pub(crate) namespace: Option<Utf16String>,
}

#[derive(Clone)]
pub(crate) struct SanitizerProcessingInstruction {
    pub(crate) target: Utf16String,
}

#[derive(Clone)]
pub(crate) enum SanitizerElement {
    DOMString(Utf16String),
    SanitizerElementNamespace(SanitizerElementNamespace),
}

#[derive(Clone)]
pub(crate) enum SanitizerElementWithAttributes {
    DOMString(Utf16String),
    SanitizerElementNamespaceWithAttributes(SanitizerElementNamespaceWithAttributes),
}

#[derive(Clone)]
pub(crate) enum SanitizerAttribute {
    DOMString(Utf16String),
    SanitizerAttributeNamespace(SanitizerAttributeNamespace),
}

#[derive(Clone)]
pub(crate) enum SanitizerPI {
    DOMString(Utf16String),
    SanitizerProcessingInstruction(SanitizerProcessingInstruction),
}

#[derive(Clone, Default)]
pub(crate) struct SanitizerConfig {
    pub(crate) elements: Option<Vec<SanitizerElementWithAttributes>>,
    pub(crate) remove_elements: Option<Vec<SanitizerElement>>,
    pub(crate) replace_with_children_elements: Option<Vec<SanitizerElement>>,
    pub(crate) processing_instructions: Option<Vec<SanitizerPI>>,
    pub(crate) remove_processing_instructions: Option<Vec<SanitizerPI>>,
    pub(crate) attributes: Option<Vec<SanitizerAttribute>>,
    pub(crate) remove_attributes: Option<Vec<SanitizerAttribute>>,
    pub(crate) comments: Option<bool>,
    pub(crate) data_attributes: Option<bool>,
    pub(crate) javascript_ur_ls: Option<bool>,
}

pub(crate) enum SanitizerConfigOrSanitizerPresets {
    SanitizerConfig(SanitizerConfig),
    SanitizerPresets(SanitizerPresets),
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#sanitizer
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Sanitizer {
    // https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#sanitizer-configuration
    #[gc(untraced)]
    configuration: RefCell<SanitizerConfiguration>,
}

impl Wrappable for Sanitizer {}

impl Sanitizer {
    // https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-sanitizer-constructor
    pub(crate) fn construct(
        _: &mut CallContext,
        configuration: SanitizerConfigOrSanitizerPresets,
    ) -> WebIdlResult<Self> {
        // 1. If configuration is a SanitizerPresets string:
        //    1. Assert: configuration is "default".
        //    2. Set configuration to the built-in safe default configuration.
        let fresh_configuration = match configuration {
            SanitizerConfigOrSanitizerPresets::SanitizerConfig(config) => canonicalize_sanitizer_config(&config),
            SanitizerConfigOrSanitizerPresets::SanitizerPresets(preset) => {
                assert!(preset == SanitizerPresets::Default);
                built_in_safe_default_configuration().clone()
            }
        };

        // 2. Configure this given configuration and true.
        let canonical_configuration = configure_sanitizer_configuration(fresh_configuration, PermissiveDefaults::Yes)?;
        Ok(Self::new(canonical_configuration))
    }

    fn new(configuration: SanitizerConfiguration) -> Self {
        assert!(sanitizer_configuration_is_valid(&configuration));
        Self {
            configuration: RefCell::new(configuration),
        }
    }

    pub(crate) fn create_with_configuration(vm: &Vm, configuration: SanitizerConfiguration) -> Ref<Self> {
        allocate(vm, |_| Self::new(configuration))
    }

    pub(crate) fn configuration(&self) -> SanitizerConfiguration {
        self.configuration.borrow().clone()
    }
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#compare-sanitizer-items
fn compare_sanitizer_items(item_a: &SanitizerName, item_b: &SanitizerName) -> bool {
    // 1. Let namespaceA be itemA["namespace"].
    let namespace_a = &item_a.namespace_;

    // 2. Let namespaceB be itemB["namespace"].
    let namespace_b = &item_b.namespace_;

    // 3. If namespaceA is null:
    if namespace_a.is_none() {
        // 1. If namespaceB is not null, then return true.
        if namespace_b.is_some() {
            return true;
        }
    }
    // 4. Otherwise:
    else {
        // 1. If namespaceB is null, then return false.
        let Some(namespace_b) = namespace_b else {
            return false;
        };
        let namespace_a = namespace_a.as_ref().unwrap();

        // 2. If namespaceA is code unit less than namespaceB, then return true.
        if Utf16View::of_string(namespace_a)
            .code_units()
            .cmp(Utf16View::of_string(namespace_b).code_units())
            .is_lt()
        {
            return true;
        }

        // 3. If namespaceA is not namespaceB, then return false.
        if namespace_a != namespace_b {
            return false;
        }
    }

    // 5. If itemA["name"] is code unit less than itemB["name"], then return true.
    if Utf16View::of_string(&item_a.name)
        .code_units()
        .cmp(Utf16View::of_string(&item_b.name).code_units())
        .is_lt()
    {
        return true;
    }

    // 6. Return false.
    false
}

fn sort_sanitizer_items(list: &mut [SanitizerName]) {
    list.sort_unstable_by(sanitizer_item_order);
}
fn sanitizer_item_order(a: &SanitizerName, b: &SanitizerName) -> core::cmp::Ordering {
    if compare_sanitizer_items(a, b) {
        core::cmp::Ordering::Less
    } else if compare_sanitizer_items(b, a) {
        core::cmp::Ordering::Greater
    } else {
        core::cmp::Ordering::Equal
    }
}
fn sort_processing_instructions(list: &mut [Utf16String]) {
    list.sort_unstable_by(|a, b| {
        Utf16View::of_string(a)
            .code_units()
            .cmp(Utf16View::of_string(b).code_units())
    });
}

// https://infra.spec.whatwg.org/#create-a-set
fn create_a_set(list: &[SanitizerName]) -> Vec<SanitizerName> {
    let mut set = Vec::new();
    for item in list {
        if !set.contains(item) {
            set.push(item.clone());
        }
    }
    set
}
// https://infra.spec.whatwg.org/#set-difference
fn difference(a: &[SanitizerName], b: &[SanitizerName]) -> Vec<SanitizerName> {
    let mut result = Vec::new();
    for item in a {
        if !b.contains(item) {
            result.push(item.clone());
        }
    }
    result
}
// https://infra.spec.whatwg.org/#set-intersection
fn intersection(a: &[SanitizerName], b: &[SanitizerName]) -> Vec<SanitizerName> {
    let mut result = Vec::new();
    for item in a {
        if b.contains(item) {
            result.push(item.clone());
        }
    }
    result
}

impl Sanitizer {
    // https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-sanitizer-get
    pub(crate) fn get(&self, _: &mut CallContext) -> WebIdlResult<SanitizerConfig> {
        // NOTE: Outside of the get() method, the order of the Sanitizer's elements and attributes is unobservable. By
        //       explicitly sorting the result of this method, we give implementations the opportunity to optimize by,
        //       for example, using unordered sets internally.
        // 1. Let config be this's configuration.
        let mut config = self.configuration.borrow().clone();

        // 2. Assert: config is valid.
        assert!(sanitizer_configuration_is_valid(&config));

        // 3. If config["elements"] exists:
        if let Some(elements) = &mut config.elements {
            // 1. For each element of config["elements"]:
            for element in elements.iter_mut() {
                // 1. If element["attributes"] exists, then set element["attributes"] to the result of sorting
                //    element["attributes"], with compare sanitizer items.
                if let Some(list) = &mut element.attributes {
                    sort_sanitizer_items(list);
                }

                // 2. If element["removeAttributes"] exists, then set element["removeAttributes"] to the result of sorting
                //    element["removeAttributes"], with compare sanitizer items.
                if let Some(list) = &mut element.remove_attributes {
                    sort_sanitizer_items(list);
                }
            }

            // 2. Set config["elements"] to the result of sorting config["elements"], with compare sanitizer items.
            elements.sort_unstable_by(|a, b| sanitizer_item_order(&a.element, &b.element));
        }
        // 4. Otherwise:
        else {
            // 1. Set config["removeElements"] to the result of sorting config["removeElements"], with compare sanitizer
            //    items.
            sort_sanitizer_items(config.remove_elements.as_mut().unwrap());
        }

        // 5. If config["replaceWithChildrenElements"] exists, then set config["replaceWithChildrenElements"] to the result
        //    of sorting config["replaceWithChildrenElements"], with compare sanitizer items.
        if let Some(list) = &mut config.replace_with_children_elements {
            sort_sanitizer_items(list);
        }

        // 6. If config["processingInstructions"] exists, then set config["processingInstructions"] to the result of
        //    sorting config["processingInstructions"], with piA["target"] being code unit less than piB["target"].
        if let Some(list) = &mut config.processing_instructions {
            sort_processing_instructions(list);
        }
        // 7. Otherwise:
        else {
            // 1. Set config["removeProcessingInstructions"] to the result of sorting
            //    config["removeProcessingInstructions"], with piA["target"] being code unit less than piB["target"].
            sort_processing_instructions(config.remove_processing_instructions.as_mut().unwrap());
        }

        // 8. If config["attributes"] exists, then set config["attributes"] to the result of sorting config["attributes"]
        //    given compare sanitizer items.
        if let Some(list) = &mut config.attributes {
            sort_sanitizer_items(list);
        }
        // 9. Otherwise:
        else {
            // 1. Set config["removeAttributes"] to the result of sorting config["removeAttributes"] given compare
            //    sanitizer items.
            sort_sanitizer_items(config.remove_attributes.as_mut().unwrap());
        }

        // 10. Return config.
        Ok(sanitizer_configuration_to_bindings(&config))
    }

    // https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-sanitizer-allowelement
    pub(crate) fn allow_element(
        &self,
        _: &mut CallContext,
        element_input: SanitizerElementWithAttributes,
    ) -> WebIdlResult<bool> {
        // 1. Let configuration be this's configuration.
        let mut configuration = self.configuration.borrow_mut();

        // 2. Assert: configuration is valid.
        assert!(sanitizer_configuration_is_valid(&configuration));

        // 3. Set element to the result of canonicalizing element.
        let mut element = canonicalize_sanitizer_element_with_attributes(&element_input);

        // 4. If configuration["elements"] exists:
        if configuration.elements.is_some() {
            // 1. Let modified be the result of removing element from configuration["replaceWithChildrenElements"].
            let modified = sanitizer_list_remove(&mut configuration.replace_with_children_elements, &element.element);

            // 2. If configuration["attributes"] exists:
            if let Some(attributes) = &configuration.attributes {
                // 1. If element["attributes"] exists:
                if let Some(list) = &element.attributes {
                    // 1. Set element["attributes"] to the result of creating a set from element["attributes"].
                    element.attributes = Some(create_a_set(list));

                    // 2. Set element["attributes"] to the difference of element["attributes"] and
                    //    configuration["attributes"].
                    element.attributes = Some(difference(element.attributes.as_ref().unwrap(), attributes));

                    // 3. If configuration["dataAttributes"] is true, then remove all items item from element["attributes"]
                    //    where item is a custom data attribute.
                    if configuration.data_attributes == Some(true) {
                        element
                            .attributes
                            .as_mut()
                            .unwrap()
                            .retain(|item| !is_custom_data_attribute(item));
                    }
                }

                // 2. If element["removeAttributes"] exists:
                if let Some(list) = &element.remove_attributes {
                    // 1. Set element["removeAttributes"] to the result of creating a set from element["removeAttributes"].
                    element.remove_attributes = Some(create_a_set(list));

                    // 2. Set element["removeAttributes"] to the intersection of element["removeAttributes"] and
                    //    configuration["attributes"].
                    element.remove_attributes =
                        Some(intersection(element.remove_attributes.as_ref().unwrap(), attributes));
                }
            }
            // 3. Otherwise:
            else {
                // 1. If element["attributes"] exists:
                if let Some(list) = &element.attributes {
                    // 1. Set element["attributes"] to the result of creating a set from element["attributes"].
                    element.attributes = Some(create_a_set(list));

                    // 2. Set element["attributes"] to the difference of element["attributes"] and
                    //    element["removeAttributes"] with default « ».
                    element.attributes = Some(difference(
                        element.attributes.as_ref().unwrap(),
                        element.remove_attributes.as_deref().unwrap_or(&[]),
                    ));

                    // 3. Remove element["removeAttributes"].
                    element.remove_attributes = None;

                    // 4. Set element["attributes"] to the difference of element["attributes"] and
                    //    configuration["removeAttributes"].
                    element.attributes = Some(difference(
                        element.attributes.as_ref().unwrap(),
                        configuration.remove_attributes.as_ref().unwrap(),
                    ));
                }

                // 2. If element["removeAttributes"] exists:
                if let Some(list) = &element.remove_attributes {
                    // 1. Set element["removeAttributes"] to the result of creating a set from element["removeAttributes"].
                    element.remove_attributes = Some(create_a_set(list));

                    // 2. Set element["removeAttributes"] to the difference of element["removeAttributes"] and
                    //    configuration["removeAttributes"].
                    element.remove_attributes = Some(difference(
                        element.remove_attributes.as_ref().unwrap(),
                        configuration.remove_attributes.as_ref().unwrap(),
                    ));
                }
            }

            // 4. If configuration["elements"] does not contain element:
            let elements = configuration.elements.as_mut().unwrap();
            let current_element = elements.iter().position(|item| item.element == element.element);
            if current_element.is_none() {
                // 1. Append element to configuration["elements"].
                elements.push(element);

                // 2. Return true.
                return Ok(true);
            }

            // 5. Let currentElement be the item in configuration["elements"] whose name member is element's name member
            //    and whose namespace member is element's namespace member.
            // 6. If element is equal to currentElement, then return modified.
            let current_element = current_element.unwrap();
            if element == elements[current_element] {
                return Ok(modified);
            }

            // 7. Remove element from configuration["elements"].
            elements.remove(current_element);

            // 8. Append element to configuration["elements"].
            elements.push(element);

            // 9. Return true.
            return Ok(true);
        }

        // 5. Otherwise:
        // 1. If element["attributes"] exists or element["removeAttributes"] with default « » is not empty, then return
        //    false.
        if element.attributes.is_some() || element.remove_attributes.as_ref().is_some_and(|list| !list.is_empty()) {
            return Ok(false);
        }

        // 2. Let modified be the result of removing element from configuration["replaceWithChildrenElements"].
        let modified = sanitizer_list_remove(&mut configuration.replace_with_children_elements, &element.element);

        // 3. If configuration["removeElements"] does not contain element, then return modified.
        if !configuration
            .remove_elements
            .as_ref()
            .unwrap()
            .contains(&element.element)
        {
            return Ok(modified);
        }

        // 4. Remove element from configuration["removeElements"].
        sanitizer_list_remove(&mut configuration.remove_elements, &element.element);

        // 5. Return true.
        Ok(true)
    }

    // https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-sanitizer-removeelement
    pub(crate) fn remove_element(&self, _: &mut CallContext, element: SanitizerElement) -> WebIdlResult<bool> {
        // The removeElement(element) method steps are to return the result of removing element from this's
        // configuration.
        Ok(remove_an_element_from_sanitizer_config(
            &mut self.configuration.borrow_mut(),
            &canonicalize_sanitizer_element(&element),
        ))
    }

    // https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-sanitizer-replaceelementwithchildren
    pub(crate) fn replace_element_with_children(
        &self,
        _: &mut CallContext,
        element_input: SanitizerElement,
    ) -> WebIdlResult<bool> {
        // 1. Let configuration be this's configuration.
        let mut configuration = self.configuration.borrow_mut();

        // 2. Assert: configuration is valid.
        assert!(sanitizer_configuration_is_valid(&configuration));

        // 3. Set element to the result of canonicalizing element.
        let element = canonicalize_sanitizer_element(&element_input);

        // 4. If the built-in non-replaceable elements list contains element, then return false.
        if built_in_non_replaceable_elements_list_contains(&element) {
            return Ok(false);
        }

        // 5. Let modified be the result of removing element from configuration["elements"].
        let mut modified = false;
        if let Some(elements) = &mut configuration.elements {
            let size = elements.len();
            elements.retain(|item| item.element != element);
            modified = size != elements.len();
        }

        // 6. If removing element from configuration["removeElements"] is true, then set modified to true.
        if sanitizer_list_remove(&mut configuration.remove_elements, &element) {
            modified = true;
        }

        // 7. If configuration["replaceWithChildrenElements"] does not contain element:
        if !sanitizer_list_contains(configuration.replace_with_children_elements.as_deref(), &element) {
            // 1. Append element to configuration["replaceWithChildrenElements"].
            configuration
                .replace_with_children_elements
                .get_or_insert_with(Vec::new)
                .push(element);

            // 2. Return true.
            return Ok(true);
        }

        // 8. Return modified.
        Ok(modified)
    }

    // https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-sanitizer-allowattribute
    pub(crate) fn allow_attribute(
        &self,
        _: &mut CallContext,
        attribute_input: SanitizerAttribute,
    ) -> WebIdlResult<bool> {
        // 1. Let configuration be this's configuration.
        let mut configuration = self.configuration.borrow_mut();

        // 2. Assert: configuration is valid.
        assert!(sanitizer_configuration_is_valid(&configuration));

        // 3. Set attribute to the result of canonicalizing attribute.
        let attribute = canonicalize_sanitizer_attribute(&attribute_input);

        // 4. If configuration["attributes"] exists:
        if configuration.attributes.is_some() {
            // 1. If configuration["dataAttributes"] is true and attribute is a custom data attribute, then return false.
            if configuration.data_attributes == Some(true) && is_custom_data_attribute(&attribute) {
                return Ok(false);
            }

            // 2. If configuration["attributes"] contains attribute, then return false.
            if configuration.attributes.as_ref().unwrap().contains(&attribute) {
                return Ok(false);
            }

            // 3. If configuration["elements"] exists:
            if let Some(elements) = &mut configuration.elements {
                // 1. For each element in configuration["elements"]:
                for element in elements {
                    // 1. If element["attributes"] with default « » contains attribute, then remove attribute from
                    //    element["attributes"].
                    sanitizer_list_remove(&mut element.attributes, &attribute);
                }
            }

            // 4. Append attribute to configuration["attributes"].
            configuration.attributes.as_mut().unwrap().push(attribute);

            // 5. Return true.
            return Ok(true);
        }

        // 5. Otherwise:
        // 1. If configuration["removeAttributes"] does not contain attribute, then return false.
        if !configuration.remove_attributes.as_ref().unwrap().contains(&attribute) {
            return Ok(false);
        }

        // 2. Remove attribute from configuration["removeAttributes"].
        sanitizer_list_remove(&mut configuration.remove_attributes, &attribute);

        // 3. Return true.
        Ok(true)
    }

    // https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-sanitizer-removeattribute
    pub(crate) fn remove_attribute(&self, _: &mut CallContext, attribute: SanitizerAttribute) -> WebIdlResult<bool> {
        // The removeAttribute(attribute) method steps are to return the result of removing attribute from this's
        // configuration.
        Ok(remove_an_attribute_from_sanitizer_config(
            &mut self.configuration.borrow_mut(),
            &canonicalize_sanitizer_attribute(&attribute),
        ))
    }

    // https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-sanitizer-setcomments
    pub(crate) fn set_comments(&self, _: &mut CallContext, allow: bool) -> WebIdlResult<bool> {
        // 1. Let configuration be this's configuration.
        let mut configuration = self.configuration.borrow_mut();

        // 2. Assert: configuration is valid.
        assert!(sanitizer_configuration_is_valid(&configuration));

        // 3. If configuration["comments"] is allow, then return false.
        if configuration.comments == Some(allow) {
            return Ok(false);
        }

        // 4. Set configuration["comments"] to allow.
        configuration.comments = Some(allow);

        // 5. Return true.
        Ok(true)
    }

    // https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-sanitizer-setdataattributes
    pub(crate) fn set_data_attributes(&self, _: &mut CallContext, allow: bool) -> WebIdlResult<bool> {
        // 1. Let configuration be this's configuration.
        let mut configuration = self.configuration.borrow_mut();

        // 2. Assert: configuration is valid.
        assert!(sanitizer_configuration_is_valid(&configuration));

        // 3. If configuration["attributes"] does not exist, then return false.
        if configuration.attributes.is_none() {
            return Ok(false);
        }

        // 4. If configuration["dataAttributes"] exists and is equal to allow, then return false.
        if configuration.data_attributes == Some(allow) {
            return Ok(false);
        }

        // 5. If allow is true:
        if allow {
            // 1. If configuration["elements"] exists:
            if let Some(elements) = &mut configuration.elements {
                // 1. For each element of configuration["elements"]:
                for element in elements {
                    // 1. If element["attributes"] exists, then remove all items item from element["attributes"] where item
                    //    is a custom data attribute.
                    if let Some(attributes) = &mut element.attributes {
                        attributes.retain(|item| !is_custom_data_attribute(item));
                    }
                }
            }

            // 2. Remove all items item from configuration["attributes"] where item is a custom data attribute.
            configuration
                .attributes
                .as_mut()
                .unwrap()
                .retain(|item| !is_custom_data_attribute(item));
        }

        // 6. Set configuration["dataAttributes"] to allow.
        configuration.data_attributes = Some(allow);

        // 7. Return true.
        Ok(true)
    }

    // https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-sanitizer-setjavascripturls
    pub(crate) fn set_javascript_urls(&self, _: &mut CallContext, allow: bool) -> WebIdlResult<bool> {
        // 1. Let configuration be this's configuration.
        let mut configuration = self.configuration.borrow_mut();

        // 2. Assert: configuration is valid.
        assert!(sanitizer_configuration_is_valid(&configuration));

        // 3. If configuration["javascriptURLs"] is allow, then return false.
        if configuration.javascript_urls == Some(allow) {
            return Ok(false);
        }

        // 4. Set configuration["javascriptURLs"] to allow.
        configuration.javascript_urls = Some(allow);

        // 5. Return true.
        Ok(true)
    }

    // https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-sanitizer-allowprocessinginstruction
    pub(crate) fn allow_processing_instruction(
        &self,
        _: &mut CallContext,
        pi_input: SanitizerPI,
    ) -> WebIdlResult<bool> {
        // 1. Let configuration be this's configuration.
        let mut configuration = self.configuration.borrow_mut();

        // 2. Assert: configuration is valid.
        assert!(sanitizer_configuration_is_valid(&configuration));

        // 3. Set pi to the result of canonicalizing pi.
        let pi = canonicalize_sanitizer_processing_instruction(&pi_input);

        // 4. If configuration["processingInstructions"] exists:
        if let Some(list) = &mut configuration.processing_instructions {
            // 1. If configuration["processingInstructions"] contains pi, then return false.
            if list.contains(&pi) {
                return Ok(false);
            }

            // 2. Append pi to configuration["processingInstructions"].
            list.push(pi);

            // 3. Return true.
            return Ok(true);
        }

        // 5. Otherwise:
        // 1. If configuration["removeProcessingInstructions"] contains pi:
        if configuration
            .remove_processing_instructions
            .as_ref()
            .unwrap()
            .contains(&pi)
        {
            // 1. Remove pi from configuration["removeProcessingInstructions"].
            configuration
                .remove_processing_instructions
                .as_mut()
                .unwrap()
                .retain(|item| *item != pi);

            // 2. Return true.
            return Ok(true);
        }

        // 2. Return false.
        Ok(false)
    }

    // https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-sanitizer-removeprocessinginstruction
    pub(crate) fn remove_processing_instruction(
        &self,
        _: &mut CallContext,
        pi_input: SanitizerPI,
    ) -> WebIdlResult<bool> {
        // 1. Let configuration be this's configuration.
        let mut configuration = self.configuration.borrow_mut();

        // 2. Assert: configuration is valid.
        assert!(sanitizer_configuration_is_valid(&configuration));

        // 3. Set pi to the result of canonicalizing pi.
        let pi = canonicalize_sanitizer_processing_instruction(&pi_input);

        // 4. If configuration["processingInstructions"] exists:
        if let Some(list) = &mut configuration.processing_instructions {
            // 1. If configuration["processingInstructions"] contains pi:
            if list.contains(&pi) {
                // 1. Remove pi from configuration["processingInstructions"].
                list.retain(|item| *item != pi);

                // 2. Return true.
                return Ok(true);
            }

            // 2. Return false.
            return Ok(false);
        }

        // 5. Otherwise:
        // 1. If configuration["removeProcessingInstructions"] contains pi, then return false.
        if configuration
            .remove_processing_instructions
            .as_ref()
            .unwrap()
            .contains(&pi)
        {
            return Ok(false);
        }

        // 2. Append pi to configuration["removeProcessingInstructions"].
        configuration.remove_processing_instructions.as_mut().unwrap().push(pi);

        // 3. Return true.
        Ok(true)
    }

    // https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-sanitizer-removeunsafe
    pub(crate) fn remove_unsafe(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        // The removeUnsafe() method steps are to return the result of removing unsafe from this's configuration.
        Ok(remove_unsafe_from_sanitizer_config(
            &mut self.configuration.borrow_mut(),
        ))
    }
}

/// Copies the Sanitizer’s configuration.
///
/// # Safety
///
/// `sanitizer` must be a live Sanitizer cell.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_sanitizer_configuration(sanitizer: *mut c_void) -> *mut c_void {
    // SAFETY: The caller passes a live Sanitizer.
    let sanitizer = unsafe { cell_from_abi::<Sanitizer>(sanitizer) };
    Arc::into_raw(Arc::new(sanitizer.configuration())).cast_mut().cast()
}
