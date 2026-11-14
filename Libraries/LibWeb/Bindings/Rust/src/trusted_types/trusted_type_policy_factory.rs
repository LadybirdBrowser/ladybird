/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! TrustedTypePolicyFactory, https://w3c.github.io/trusted-types/dist/spec/#trustedtypepolicyfactory

use core::cell::{Cell, RefCell};
use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{Gc, Object, Realm, Trace, Utf16View, Value, Vm, utf16_formatted};

use crate::bindings::global_object::relevant_global_object;
use crate::bindings::interface::{Ref, Wrappable, allocate, cell_from_abi, cell_into_abi, implementation_if_wrapper};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::html::global_event_handlers::is_event_handler_content_attribute;
use crate::trusted_types::require_trusted_types_for_directive::should_trusted_type_policy_creation_be_blocked_by_content_security_policy;
use crate::trusted_types::trusted_html::TrustedHTML;
use crate::trusted_types::trusted_script::TrustedScript;
use crate::trusted_types::trusted_script_url::TrustedScriptURL;
use crate::trusted_types::trusted_type_policy::{
    ElementInterface, TrustedTypeName, TrustedTypePolicy, TrustedTypePolicyOptions, element_interface,
};

pub(crate) const HTML_NAMESPACE: &str = "http://www.w3.org/1999/xhtml";
pub(crate) const SVG_NAMESPACE: &str = "http://www.w3.org/2000/svg";
pub(crate) const MATHML_NAMESPACE: &str = "http://www.w3.org/1998/Math/MathML";
const XLINK_NAMESPACE: &str = "http://www.w3.org/1999/xlink";

/// https://w3c.github.io/trusted-types/dist/spec/#trustedtypepolicyfactory
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct TrustedTypePolicyFactory {
    realm: Gc<Realm>,

    // https://w3c.github.io/trusted-types/dist/spec/#trustedtypepolicyfactory-created-policy-names
    #[gc(untraced)]
    created_policy_names: RefCell<Vec<Utf16String>>,

    // https://w3c.github.io/trusted-types/dist/spec/#trustedtypepolicyfactory-default-policy
    default_policy: Cell<Option<Ref<TrustedTypePolicy>>>,

    // https://w3c.github.io/trusted-types/dist/spec/#dom-trustedtypepolicyfactory-emptyhtml
    empty_html: Cell<Option<Ref<TrustedHTML>>>,

    // https://w3c.github.io/trusted-types/dist/spec/#dom-trustedtypepolicyfactory-emptyscript
    empty_script: Cell<Option<Ref<TrustedScript>>>,
}

impl Wrappable for TrustedTypePolicyFactory {
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        Some(self.realm)
    }
}

impl TrustedTypePolicyFactory {
    pub(crate) fn create(vm: &Vm, realm: Gc<Realm>) -> Ref<Self> {
        allocate(vm, |_| Self {
            realm,
            created_policy_names: RefCell::new(Vec::new()),
            default_policy: Cell::new(None),
            empty_html: Cell::new(None),
            empty_script: Cell::new(None),
        })
    }

    pub(crate) fn default_policy_value(&self) -> Option<Ref<TrustedTypePolicy>> {
        self.default_policy.get()
    }

    // https://w3c.github.io/trusted-types/dist/spec/#dom-trustedtypepolicyfactory-getattributetype
    pub(crate) fn get_attribute_type(
        &self,
        _: &mut CallContext,
        tag_name: Utf16String,
        attribute: Utf16String,
        element_ns: Option<Utf16String>,
        attr_ns: Option<Utf16String>,
    ) -> WebIdlResult<Option<Utf16String>> {
        // 1. Set localName to tagName in ASCII lowercase.
        let local_name = tag_name.to_ascii_lowercase();

        // 2. Set attribute to attribute in ASCII lowercase.
        let attribute_local_name = attribute.to_ascii_lowercase();

        // 3. If elementNs is null or an empty string, set elementNs to HTML namespace.
        let element_namespace = match element_ns {
            Some(element_ns) if !element_ns.is_empty() => element_ns,
            _ => Utf16String::from_utf8(HTML_NAMESPACE),
        };

        // 4. If attrNs is an empty string, set attrNs to null.
        let attr_ns = attr_ns.filter(|attr_ns| !attr_ns.is_empty());

        // 5. Let interface be the element interface for localName and elementNs.
        let interface = element_interface(&local_name, Some(&element_namespace));

        // 6. Let expectedType be null.
        let mut expected_type = None;

        // 7. Set attributeData to the result of Get Trusted Type data for attribute algorithm,
        // with the following arguments, interface as element, attribute, attrNs
        let attribute_data = get_trusted_type_data_for_attribute(&interface, &attribute_local_name, attr_ns.as_ref());

        // 8. If attributeData is not null, then set expectedType to the interface’s name of the value of the fourth member of attributeData.
        if let Some(attribute_data) = attribute_data {
            expected_type = Some(Utf16String::from_utf8(attribute_data.trusted_type.name()));
        }

        // 9. Return expectedType.
        Ok(expected_type)
    }

    // https://w3c.github.io/trusted-types/dist/spec/#dom-trustedtypepolicyfactory-getpropertytype
    pub(crate) fn get_property_type(
        &self,
        _: &mut CallContext,
        tag_name: Utf16String,
        property: Utf16String,
        element_ns: Option<Utf16String>,
    ) -> WebIdlResult<Option<Utf16String>> {
        // 1. Set localName to tagName in ASCII lowercase.
        let local_name = tag_name.to_ascii_lowercase();

        // 2. If elementNs is null or an empty string, set elementNs to HTML namespace.
        let element_namespace = match element_ns {
            Some(element_ns) if !element_ns.is_empty() => element_ns,
            _ => Utf16String::from_utf8(HTML_NAMESPACE),
        };

        // FIXME: We don't have a method in ElementFactory that can give us the interface name but these are all the cases
        // we care about in the table in get_trusted_type_data_for_attribute function
        // 3. Let interface be the element interface for localName and elementNs.
        let local_name = Utf16View::of_string(&local_name);
        let is_html = Utf16View::of_string(&element_namespace) == HTML_NAMESPACE;
        let interface = if local_name == "iframe" && is_html {
            "HTMLIFrameElement"
        } else if local_name == "script" && is_html {
            "HTMLScriptElement"
        } else {
            "Element"
        };

        // 4. Let expectedType be null.
        let mut expected_type = None;

        const TABLE: [(&str, &str, TrustedTypeName); 7] = [
            ("HTMLIFrameElement", "srcdoc", TrustedTypeName::TrustedHTML),
            ("HTMLScriptElement", "innerText", TrustedTypeName::TrustedScript),
            ("HTMLScriptElement", "src", TrustedTypeName::TrustedScriptURL),
            ("HTMLScriptElement", "text", TrustedTypeName::TrustedScript),
            ("HTMLScriptElement", "textContent", TrustedTypeName::TrustedScript),
            ("*", "innerHTML", TrustedTypeName::TrustedHTML),
            ("*", "outerHTML", TrustedTypeName::TrustedHTML),
        ];

        // 5. Find the row in the following table, where the first column is "*" or interface’s name, and property is in the second column.
        // If a matching row is found, set expectedType to the interface’s name of the value of the third column.
        let property = Utf16View::of_string(&property);
        let matching_row = TABLE.iter().find(|(row_interface, row_property, _)| {
            (*row_interface == interface || *row_interface == "*") && property == *row_property
        });

        if let Some((_, _, trusted_type)) = matching_row {
            expected_type = Some(Utf16String::from_utf8(trusted_type.name()));
        }

        // 6. Return expectedType.
        Ok(expected_type)
    }

    // https://w3c.github.io/trusted-types/dist/spec/#dom-trustedtypepolicyfactory-createpolicy
    pub(crate) fn create_policy(
        &self,
        cx: &mut CallContext,
        policy_name: Utf16String,
        policy_options: TrustedTypePolicyOptions,
    ) -> WebIdlResult<Ref<TrustedTypePolicy>> {
        // 1. Returns the result of executing a Create a Trusted Type Policy algorithm, with the following arguments:
        //      factory: this value
        //      policyName: policyName
        //      options: policyOptions
        //      global: this value’s relevant global object
        self.create_a_trusted_type_policy(cx, policy_name, policy_options, relevant_global_object(self))
    }

    // https://w3c.github.io/trusted-types/dist/spec/#dom-trustedtypepolicyfactory-ishtml
    pub(crate) fn is_html(&self, _: &mut CallContext, value: Value) -> WebIdlResult<bool> {
        // 1. Returns true if value is an instance of TrustedHTML and has an associated data value set, false otherwise.
        Ok(value.is_object() && implementation_if_wrapper::<TrustedHTML>(value.as_object()).is_some())
    }

    // https://w3c.github.io/trusted-types/dist/spec/#dom-trustedtypepolicyfactory-isscript
    pub(crate) fn is_script(&self, _: &mut CallContext, value: Value) -> WebIdlResult<bool> {
        // 1. Returns true if value is an instance of TrustedScript and has an associated data value set, false otherwise.
        Ok(value.is_object() && implementation_if_wrapper::<TrustedScript>(value.as_object()).is_some())
    }

    // https://w3c.github.io/trusted-types/dist/spec/#dom-trustedtypepolicyfactory-isscripturl
    pub(crate) fn is_script_url(&self, _: &mut CallContext, value: Value) -> WebIdlResult<bool> {
        // 1. Returns true if value is an instance of TrustedScriptURL and has an associated data value set, false otherwise.
        Ok(value.is_object() && implementation_if_wrapper::<TrustedScriptURL>(value.as_object()).is_some())
    }

    // https://w3c.github.io/trusted-types/dist/spec/#dom-trustedtypepolicyfactory-emptyhtml
    pub(crate) fn empty_html(&self, cx: &mut CallContext) -> WebIdlResult<Ref<TrustedHTML>> {
        if let Some(empty_html) = self.empty_html.get() {
            return Ok(empty_html);
        }
        let empty_html = TrustedHTML::create(cx.vm(), Utf16String::default());
        self.empty_html.set(Some(empty_html));
        Ok(empty_html)
    }

    // https://w3c.github.io/trusted-types/dist/spec/#dom-trustedtypepolicyfactory-emptyscript
    pub(crate) fn empty_script(&self, cx: &mut CallContext) -> WebIdlResult<Ref<TrustedScript>> {
        if let Some(empty_script) = self.empty_script.get() {
            return Ok(empty_script);
        }
        let empty_script = TrustedScript::create(cx.vm(), Utf16String::default());
        self.empty_script.set(Some(empty_script));
        Ok(empty_script)
    }

    // https://w3c.github.io/trusted-types/dist/spec/#dom-trustedtypepolicyfactory-defaultpolicy
    pub(crate) fn default_policy(&self, _: &mut CallContext) -> WebIdlResult<Option<Ref<TrustedTypePolicy>>> {
        Ok(self.default_policy.get())
    }

    // https://w3c.github.io/trusted-types/dist/spec/#create-trusted-type-policy-algorithm
    fn create_a_trusted_type_policy(
        &self,
        cx: &CallContext,
        policy_name: Utf16String,
        options: TrustedTypePolicyOptions,
        global: Gc<Object>,
    ) -> WebIdlResult<Ref<TrustedTypePolicy>> {
        // 1. Let allowedByCSP be the result of executing Should Trusted Type policy creation be blocked by Content Security Policy? algorithm with global, policyName and factory’s created policy names value.
        let blocked = should_trusted_type_policy_creation_be_blocked_by_content_security_policy(
            cx.realm(),
            global,
            &policy_name,
            &self.created_policy_names.borrow(),
        );

        // 2. If allowedByCSP is "Blocked", throw a TypeError and abort further steps.
        if blocked {
            return Err(Exception::TypeError(utf16_formatted(
                "Content Security Policy blocked the creation of the policy {}",
                &[&policy_name],
            )));
        }

        // 3. If policyName is default and the factory’s default policy value is not null, throw a TypeError and abort further steps.
        let is_default = Utf16View::of_string(&policy_name) == "default";
        if is_default && self.default_policy.get().is_some() {
            return Err(Exception::type_error(
                "Policy Factory already has a default value defined",
            ));
        }

        // 4. Let policy be a new TrustedTypePolicy object.
        // 5. Set policy’s name property value to policyName.
        // 6. Set policy’s options value to «[ "createHTML" -> options["createHTML", "createScript" -> options["createScript", "createScriptURL" -> options["createScriptURL" ]».
        let policy = allocate(cx.vm(), |_| {
            TrustedTypePolicy::new(self.realm, policy_name.clone(), options)
        });

        // 7. If the policyName is default, set the factory’s default policy value to policy.
        if is_default {
            self.default_policy.set(Some(policy));
        }

        // 8. Append policyName to factory’s created policy names.
        self.created_policy_names.borrow_mut().push(policy_name);

        // 9. Return policy.
        Ok(policy)
    }
}

pub(crate) struct TrustedTypeData {
    pub(crate) trusted_type: TrustedTypeName,
    pub(crate) sink: Utf16String,
}

// https://w3c.github.io/trusted-types/dist/spec/#get-trusted-type-data-for-attribute
pub(crate) fn get_trusted_type_data_for_attribute(
    element: &ElementInterface,
    attribute: &Utf16String,
    attribute_ns: Option<&Utf16String>,
) -> Option<TrustedTypeData> {
    // 1. Let data be null.

    let element_name = element.element_name;
    let element_ns = element.element_ns.as_ref().map(Utf16View::of_string);
    let attribute_view = Utf16View::of_string(attribute);

    // 2. If attributeNs is null, « HTML namespace, SVG namespace, MathML namespace » contains element’s namespace, and attribute is the name of an event handler content attribute:
    if attribute_view.starts_with(Utf16View::Ascii(b"on"))
        && attribute_ns.is_none()
        && element_ns.is_some_and(|element_ns| {
            element_ns == HTML_NAMESPACE || element_ns == SVG_NAMESPACE || element_ns == MATHML_NAMESPACE
        })
        && is_event_handler_content_attribute(attribute)
    {
        // 1. Return (Element, null, attribute, TrustedScript, "Element " + attribute).
        return Some(TrustedTypeData {
            trusted_type: TrustedTypeName::TrustedScript,
            sink: utf16_formatted("Element {}", &[attribute]),
        });
    }

    // OPTIMIZATION: Every remaining row has one of these three attribute names.
    if !(attribute_view == "srcdoc" || attribute_view == "src" || attribute_view == "href") {
        return None;
    }

    const TABLE: [(&str, Option<&str>, &str, TrustedTypeName, &str); 4] = [
        (
            "HTMLIFrameElement",
            None,
            "srcdoc",
            TrustedTypeName::TrustedHTML,
            "HTMLIFrameElement srcdoc",
        ),
        (
            "HTMLScriptElement",
            None,
            "src",
            TrustedTypeName::TrustedScriptURL,
            "HTMLScriptElement src",
        ),
        (
            "SVGScriptElement",
            None,
            "href",
            TrustedTypeName::TrustedScriptURL,
            "SVGScriptElement href",
        ),
        (
            "SVGScriptElement",
            Some(XLINK_NAMESPACE),
            "href",
            TrustedTypeName::TrustedScriptURL,
            "SVGScriptElement href",
        ),
    ];

    // 3. Find the row in the following table, where element is in the first column, attributeNs is in the second column,
    // and attribute is in the third column. If a matching row is found, set data to that row.
    let data = TABLE
        .iter()
        .find(|(row_element, row_attribute_ns, row_attribute, _, _)| {
            *row_element == element_name
                && match (row_attribute_ns, attribute_ns) {
                    (None, None) => true,
                    (Some(row_attribute_ns), Some(attribute_ns)) => {
                        Utf16View::of_string(attribute_ns) == *row_attribute_ns
                    }
                    _ => false,
                }
                && attribute_view == *row_attribute
        });

    // 4. Return data
    data.map(|(_, _, _, trusted_type, sink)| TrustedTypeData {
        trusted_type: *trusted_type,
        sink: Utf16String::from_utf8(sink),
    })
}

/// A new TrustedTypePolicyFactory for the global object of the realm.
///
/// # Safety
///
/// `vm` must be the live VM, and `realm` the live realm of a Window or a WorkerGlobalScope.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_trusted_types_create_trusted_type_policy_factory(
    vm: *mut c_void,
    realm: *mut c_void,
) -> *mut c_void {
    // SAFETY: The caller passes the live VM and a live realm.
    let (vm, realm) = unsafe { crate::bindings::interface::realm_arguments(vm, realm) };
    cell_into_abi(TrustedTypePolicyFactory::create(vm, realm))
}

/// The factory's default policy, or null.
///
/// # Safety
///
/// `factory` must be the live cell of a TrustedTypePolicyFactory.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_trusted_types_trusted_type_policy_factory_default_policy(
    factory: *mut c_void,
) -> *mut c_void {
    // SAFETY: The caller passes a live cell.
    unsafe { cell_from_abi::<TrustedTypePolicyFactory>(factory) }
        .default_policy_value()
        .map_or(core::ptr::null_mut(), cell_into_abi)
}
