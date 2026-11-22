/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://dom.spec.whatwg.org/#interface-domtokenlist

use core::cell::{Cell, RefCell};
use core::ffi::c_void;

use ak::{Utf16FlyString, Utf16String};
use libjs_rust::api::{Trace, Utf16StringBuilder, Utf16View, Vm, utf16_string_external_memory_size};
use libwebcommon_rust::infra::character_types::is_ascii_whitespace;

use crate::bindings::interface::{Wrappable, allocate, cell_from_abi, cell_into_abi};
use crate::bindings::{CallContext, Exception, MustNotThrow, WebIdlResult};
use crate::dom::element::CppElement;

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct DOMTokenList {
    associated_element: CppElement,
    associated_attribute: Utf16FlyString,
    #[gc(untraced)]
    token_set: RefCell<Vec<Utf16String>>,
    is_running_update_steps: Cell<bool>,
}

impl Wrappable for DOMTokenList {
    const OWNS_EXTERNAL_MEMORY: bool = true;

    fn external_memory_size(&self) -> usize {
        let token_set = self.token_set.borrow();
        token_set
            .iter()
            .fold(token_set.capacity() * size_of::<Utf16String>(), |size, token| {
                size.saturating_add(utf16_string_external_memory_size(token))
            })
    }
}

impl DOMTokenList {
    // https://dom.spec.whatwg.org/#ref-for-domtokenlist%E2%91%A0%E2%91%A2
    fn new(associated_element: CppElement, associated_attribute: Utf16FlyString) -> Self {
        let list = Self {
            associated_element,
            associated_attribute,
            token_set: RefCell::new(Vec::new()),
            is_running_update_steps: Cell::new(false),
        };

        // When a DOMTokenList object set is created:
        // 1. Let element be set’s element.
        // 2. Let attributeName be set’s attribute name.
        // 3. Let value be the result of getting an attribute value given element and attributeName.
        let value = list
            .associated_element
            .get_attribute_ns(None, &list.associated_attribute);

        // 4. Run the attribute change steps for element, attributeName, value, value, and null.
        list.associated_attribute_changed(value);
        list
    }

    // https://dom.spec.whatwg.org/#ref-for-domtokenlist%E2%91%A0%E2%91%A1
    fn associated_attribute_changed(&self, value: Option<Utf16String>) {
        // 1. If localName is set’s attribute name, namespace is null, and value is null, then empty token set.
        // 2. Otherwise, if localName is set’s attribute name and namespace is null, then set set’s token set to value,
        //    parsed.
        // AD-HOC: The caller is responsible for checking the name and namespace.
        // OPTIMIZATION: The update steps just wrote this value from the token set, so there is nothing new to parse.
        if self.is_running_update_steps.get() {
            return;
        }
        let mut token_set = self.token_set.borrow_mut();
        *token_set = Vec::new();
        let Some(value) = value else {
            return;
        };
        *token_set = parse_ordered_set(Utf16View::of_string(&value));
    }

    pub(crate) fn length(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        Ok(u32::try_from(self.token_set.borrow().len()).expect("a token list has fewer than 2^32 tokens"))
    }

    // https://dom.spec.whatwg.org/#dom-domtokenlist-item
    pub(crate) fn item(&self, _: &mut CallContext, index: u32) -> WebIdlResult<Option<Utf16String>> {
        let token_set = self.token_set.borrow();
        let index = index as usize;

        // 1. If index is equal to or greater than this’s token set’s size, then return null.
        if index >= token_set.len() {
            return Ok(None);
        }

        // 2. Return this’s token set[index].
        Ok(Some(token_set[index].clone()))
    }

    // https://dom.spec.whatwg.org/#dom-domtokenlist-contains
    pub(crate) fn contains(&self, _: &mut CallContext, token: Utf16String) -> WebIdlResult<bool> {
        Ok(self.token_set.borrow().contains(&token))
    }

    pub(crate) fn add(&self, _: &mut CallContext, tokens: Vec<Utf16String>) -> WebIdlResult<()> {
        self.add_tokens(&tokens)
    }

    // https://dom.spec.whatwg.org/#dom-domtokenlist-add
    fn add_tokens(&self, tokens: &[Utf16String]) -> WebIdlResult<()> {
        // 1. For each token of tokens:
        for token in tokens {
            // a. If token is the empty string, then throw a "SyntaxError" DOMException.
            // b. If token contains any ASCII whitespace, then throw an "InvalidCharacterError" DOMException.
            validate_token(token)?;
        }

        // 2. For each token of tokens: append token to this’s token set.
        for token in tokens {
            append_to_ordered_set(&mut self.token_set.borrow_mut(), Utf16View::of_string(token));
        }

        // 3. Run the update steps.
        self.run_update_steps();
        Ok(())
    }

    pub(crate) fn remove(&self, _: &mut CallContext, tokens: Vec<Utf16String>) -> WebIdlResult<()> {
        self.remove_tokens(&tokens)
    }

    // https://dom.spec.whatwg.org/#dom-domtokenlist-remove
    fn remove_tokens(&self, tokens: &[Utf16String]) -> WebIdlResult<()> {
        // 1. For each token of tokens:
        for token in tokens {
            // a. If token is the empty string, then throw a "SyntaxError" DOMException.
            // b. If token contains any ASCII whitespace, then throw an "InvalidCharacterError" DOMException.
            validate_token(token)?;
        }

        // 2. For each token of tokens: remove token from this’s token set.
        for token in tokens {
            remove_from_ordered_set(&mut self.token_set.borrow_mut(), Utf16View::of_string(token));
        }

        // 3. Run the update steps.
        self.run_update_steps();
        Ok(())
    }

    pub(crate) fn toggle(&self, _: &mut CallContext, token: Utf16String, force: Option<bool>) -> WebIdlResult<bool> {
        self.toggle_token(token, force)
    }

    // https://dom.spec.whatwg.org/#dom-domtokenlist-toggle
    fn toggle_token(&self, token: Utf16String, force: Option<bool>) -> WebIdlResult<bool> {
        // 1. If token is the empty string, then throw a "SyntaxError" DOMException.
        // 2. If token contains any ASCII whitespace, then throw an "InvalidCharacterError" DOMException.
        validate_token(&token)?;

        // 3. If this’s token set[token] exists:
        if self.token_set.borrow().contains(&token) {
            // a. If force is either not given or is false, then remove token from this’s token set, run the update steps and return false.
            if force != Some(true) {
                remove_from_ordered_set(&mut self.token_set.borrow_mut(), Utf16View::of_string(&token));
                self.run_update_steps();
                return Ok(false);
            }

            // b. Return true.
            return Ok(true);
        }

        // 4. Otherwise, if force not given or is true, append token to this’s token set, run the update steps, and return true.
        if force != Some(false) {
            append_to_ordered_set(&mut self.token_set.borrow_mut(), Utf16View::of_string(&token));
            self.run_update_steps();
            return Ok(true);
        }

        // 5. Return false.
        Ok(false)
    }

    // https://dom.spec.whatwg.org/#dom-domtokenlist-replace
    pub(crate) fn replace(
        &self,
        _: &mut CallContext,
        token: Utf16String,
        new_token: Utf16String,
    ) -> WebIdlResult<bool> {
        // 1. If either token or newToken is the empty string, then throw a "SyntaxError" DOMException.
        validate_token_not_empty(&token)?;
        validate_token_not_empty(&new_token)?;

        // 2. If either token or newToken contains any ASCII whitespace, then throw an "InvalidCharacterError" DOMException.
        validate_token_not_whitespace(&token)?;
        validate_token_not_whitespace(&new_token)?;

        // 3. If this’s token set does not contain token, then return false.
        if !self.token_set.borrow().contains(&token) {
            return Ok(false);
        }

        // 4. Replace token in this’s token set with newToken.
        replace_in_ordered_set(
            &mut self.token_set.borrow_mut(),
            Utf16View::of_string(&token),
            Utf16View::of_string(&new_token),
        );

        // 5. Run the update steps.
        self.run_update_steps();

        // 6. Return true.
        Ok(true)
    }

    // https://dom.spec.whatwg.org/#dom-domtokenlist-supports
    pub(crate) fn supports(&self, _: &mut CallContext, token: Utf16String) -> WebIdlResult<bool> {
        // 1. Let result be the return value of validation steps called with token.
        // 2. Return result.
        self.run_validation_steps(Utf16View::of_string(&token))
    }

    // https://dom.spec.whatwg.org/#concept-domtokenlist-validation
    fn run_validation_steps(&self, token: Utf16View<'_>) -> WebIdlResult<bool> {
        let name = self.associated_element.local_name();
        let name = Utf16View::of_fly_string(&name);
        let attribute = Utf16View::of_fly_string(&self.associated_attribute);
        let supported_tokens: Option<&[&[u8]]> = match (name, attribute) {
            // https://html.spec.whatwg.org/multipage/links.html#linkTypes
            (name, attribute) if name == "link" && attribute == "rel" => Some(&[
                b"modulepreload",
                b"preload",
                b"preconnect",
                b"dns-prefetch",
                b"stylesheet",
                b"icon",
                b"alternate",
                b"prefetch",
                b"prerender",
                b"next",
                b"manifest",
                b"apple-touch-icon",
                b"apple-touch-icon-precomposed",
                b"canonical",
            ]),
            (name, attribute) if attribute == "rel" && (name == "a" || name == "area" || name == "form") => {
                Some(&[b"noreferrer", b"noopener", b"opener"])
            }

            // https://html.spec.whatwg.org/multipage/urls-and-fetching.html#blocking-attribute
            (name, attribute) if attribute == "blocking" && (name == "link" || name == "script" || name == "style") => {
                Some(&[b"render"])
            }

            // https://html.spec.whatwg.org/multipage/iframe-embed-object.html#attr-iframe-sandbox
            (name, attribute) if name == "iframe" && attribute == "sandbox" => Some(&[
                b"allow-downloads",
                b"allow-forms",
                b"allow-modals",
                b"allow-orientation-lock",
                b"allow-pointer-lock",
                b"allow-popups",
                b"allow-popups-to-escape-sandbox",
                b"allow-presentation",
                b"allow-same-origin",
                b"allow-scripts",
                b"allow-top-navigation",
                b"allow-top-navigation-by-user-activation",
                b"allow-top-navigation-to-custom-protocols",
            ]),
            _ => None,
        };

        // 1. If set’s element and attribute name does not define supported tokens, then throw a TypeError.
        let Some(supported_tokens) = supported_tokens else {
            let mut message = Utf16StringBuilder::new();
            message.append_ascii("Attribute ");
            message.append(attribute);
            message.append_ascii(" does not define any supported tokens");
            return Err(Exception::TypeError(message.to_utf16_string()));
        };

        // 2. Let lowercaseToken be token, in ASCII lowercase.
        // 3. If lowercaseToken is present in the supported tokens of set’s element and attribute name, then return true.
        if supported_tokens
            .iter()
            .any(|supported| token.equals_ignoring_ascii_case(Utf16View::Ascii(supported)))
        {
            return Ok(true);
        }

        // 4. Return false.
        Ok(false)
    }

    // https://dom.spec.whatwg.org/#dom-domtokenlist-value
    pub(crate) fn value(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self
            .associated_element
            .get_attribute_ns(None, &self.associated_attribute)
            .unwrap_or_default())
    }

    // https://dom.spec.whatwg.org/#ref-for-concept-element-attributes-set-value%E2%91%A2
    pub(crate) fn set_value(&self, _: &mut CallContext, value: Utf16String) -> WebIdlResult<()> {
        self.associated_element
            .set_attribute_value(&self.associated_attribute, value);
        Ok(())
    }

    // https://dom.spec.whatwg.org/#concept-dtl-update
    fn run_update_steps(&self) {
        // 1. If get an attribute by namespace and local name given null, set’s attribute name, and set’s element returns null and set’s token set is empty, then return.
        let attribute = self
            .associated_element
            .get_attribute_ns(None, &self.associated_attribute);
        if attribute.is_none() && self.token_set.borrow().is_empty() {
            return;
        }

        // 2. Set an attribute value given set’s element, set’s attribute name, and the result of running the ordered set
        //    serializer for set’s token set.
        let serialized = serialize_ordered_set(&self.token_set.borrow());
        let was_running_update_steps = self.is_running_update_steps.replace(true);
        self.associated_element
            .set_attribute_value(&self.associated_attribute, serialized);
        self.is_running_update_steps.set(was_running_update_steps);
    }
}

fn validate_token(token: &Utf16String) -> WebIdlResult<()> {
    validate_token_not_empty(token)?;
    validate_token_not_whitespace(token)
}

fn validate_token_not_empty(token: &Utf16String) -> WebIdlResult<()> {
    if token.is_empty() {
        return Err(Exception::syntax_error(Utf16String::from_utf8(
            "Non-empty DOM tokens are not allowed",
        )));
    }
    Ok(())
}

fn validate_token_not_whitespace(token: &Utf16String) -> WebIdlResult<()> {
    if Utf16View::of_string(token)
        .code_units()
        .any(|unit| is_ascii_whitespace(u32::from(unit)))
    {
        return Err(Exception::Dom(
            "InvalidCharacterError",
            Utf16String::from_utf8("DOM tokens containing ASCII whitespace are not allowed"),
        ));
    }
    Ok(())
}

// https://infra.spec.whatwg.org/#set-append
fn append_to_ordered_set(set: &mut Vec<Utf16String>, item: Utf16View<'_>) {
    if !set.iter().any(|value| Utf16View::of_string(value) == item) {
        set.push(item.to_utf16_string());
    }
}

// https://infra.spec.whatwg.org/#list-remove
fn remove_from_ordered_set(set: &mut Vec<Utf16String>, item: Utf16View<'_>) {
    if let Some(index) = set.iter().position(|value| Utf16View::of_string(value) == item) {
        set.remove(index);
    }
}

// https://infra.spec.whatwg.org/#set-replace
fn replace_in_ordered_set(set: &mut Vec<Utf16String>, item: Utf16View<'_>, replacement: Utf16View<'_>) {
    let item_index = set
        .iter()
        .position(|value| Utf16View::of_string(value) == item)
        .expect("the set contains the item");

    let Some(replacement_index) = set.iter().position(|value| Utf16View::of_string(value) == replacement) else {
        set[item_index] = replacement.to_utf16_string();
        return;
    };

    if item_index == replacement_index {
        return;
    }

    if replacement_index < item_index {
        set.remove(item_index);
        return;
    }

    set[item_index] = set.remove(replacement_index);
}

// https://dom.spec.whatwg.org/#concept-ordered-set-parser
fn parse_ordered_set(input: Utf16View<'_>) -> Vec<Utf16String> {
    // 1. Let inputTokens be the result of splitting input on ASCII whitespace.
    // 2. Let tokens be a new ordered set.
    let mut tokens = Vec::new();

    // 3. For each token of inputTokens: append token to tokens.
    let mut token_start = None;
    for (index, unit) in input.code_units().enumerate() {
        if is_ascii_whitespace(u32::from(unit)) {
            if let Some(start) = token_start.take() {
                append_to_ordered_set(&mut tokens, input.substring_view(start, index - start));
            }
            continue;
        }

        if token_start.is_none() {
            token_start = Some(index);
        }
    }
    if let Some(start) = token_start {
        append_to_ordered_set(
            &mut tokens,
            input.substring_view(start, input.length_in_code_units() - start),
        );
    }

    // 4. Return tokens.
    tokens
}

// https://dom.spec.whatwg.org/#concept-ordered-set-serializer
fn serialize_ordered_set(token_set: &[Utf16String]) -> Utf16String {
    // The ordered set serializer takes a set and returns the concatenation of set using U+0020 SPACE.
    let mut builder = Utf16StringBuilder::new();
    for token in token_set {
        if !builder.is_empty() {
            builder.append_ascii(" ");
        }
        builder.append(Utf16View::of_string(token));
    }
    builder.to_utf16_string()
}

/// # Safety
///
/// The caller passes a live VM and Element, and lends the interned attribute name.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_dom_token_list_create(vm: &Vm, element: *mut c_void, attribute: usize) -> *mut c_void {
    // SAFETY: The caller passes a live Element and a live, borrowed attribute name.
    let (element, attribute) = unsafe {
        (
            CppElement::from_raw(element),
            Utf16FlyString::from_raw_borrowed(attribute),
        )
    };
    cell_into_abi(allocate(vm, |_| DOMTokenList::new(element, attribute)))
}

/// # Safety
///
/// The caller passes a live DOMTokenList and transfers ownership of the new attribute value if it is nonzero.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_dom_token_list_attribute_changed(list: *mut c_void, value: usize) {
    // SAFETY: The caller passes a live DOMTokenList and transfers a reference for a nonzero value.
    let list = unsafe { cell_from_abi::<DOMTokenList>(list) };
    let value = (value != 0).then(|| unsafe { Utf16String::from_raw_owned(value) });
    list.associated_attribute_changed(value);
}

/// # Safety
///
/// The caller passes a live DOMTokenList and transfers ownership of a valid token.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_dom_token_list_add(list: *mut c_void, token: usize) {
    // SAFETY: The caller passes a live DOMTokenList and transfers a reference to the token.
    let (list, token) = unsafe { (cell_from_abi::<DOMTokenList>(list), Utf16String::from_raw_owned(token)) };
    list.add_tokens(&[token]).must();
}

/// # Safety
///
/// The caller passes a live DOMTokenList and transfers ownership of a valid token.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_dom_token_list_remove(list: *mut c_void, token: usize) {
    // SAFETY: The caller passes a live DOMTokenList and transfers a reference to the token.
    let (list, token) = unsafe { (cell_from_abi::<DOMTokenList>(list), Utf16String::from_raw_owned(token)) };
    list.remove_tokens(&[token]).must();
}

/// # Safety
///
/// The caller passes a live DOMTokenList and transfers ownership of a valid token.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_dom_token_list_toggle(list: *mut c_void, token: usize, force: bool) {
    // SAFETY: The caller passes a live DOMTokenList and transfers a reference to the token.
    let (list, token) = unsafe { (cell_from_abi::<DOMTokenList>(list), Utf16String::from_raw_owned(token)) };
    list.toggle_token(token, Some(force)).must();
}
