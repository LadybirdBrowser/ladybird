/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSStyleProperties, https://drafts.csswg.org/cssom/#cssstyleproperties

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{
    Attribute, Gc, NativeFunction, Object, PropertyAttributes, PropertyKey, Realm, ThrowCompletionOr, Trace, Value, Vm,
};
use libweb_rust::css_property_names;

use super::css_style_declaration::CSSStyleDeclaration;
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::interface::{Inherits, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};

/// What an accessor of a CSS property captures. NB: The name is static, so the collector has nothing to see in it.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSPropertyAccessor {
    #[gc(untraced)]
    property_name: &'static str,
}

impl CSSPropertyAccessor {
    /// The accessor's security check identifier, which differs from the attribute's only where that cannot matter.
    pub(crate) fn property_name(&self) -> &'static str {
        self.property_name
    }
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSStyleProperties {
    declaration: CSSStyleDeclaration,
}

impl Wrappable for CSSStyleProperties {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.declaration.as_cpp_parent()
    }
}

impl Inherits<CSSStyleDeclaration> for CSSStyleProperties {
    fn parent(&self) -> &CSSStyleDeclaration {
        &self.declaration
    }
}

/// https://drafts.csswg.org/cssom/#css-property-to-idl-attribute
fn css_property_to_idl_attribute(property: &str, lowercase_first: bool) -> String {
    // 1. Let output be the empty string.
    let mut output = String::new();

    // 2. Let uppercase next be unset.
    let mut uppercase_next = false;

    // 3. If the lowercase first flag is set, remove the first character from property.
    let property = if lowercase_first { &property[1..] } else { property };

    // 4. For each character c in property:
    for c in property.chars() {
        // 1. If c is "-", set uppercase next.
        if c == '-' {
            uppercase_next = true;
        }
        // 2. Otherwise, if uppercase next is set, unset uppercase next and append c converted to ASCII uppercase to
        //    output.
        else if uppercase_next {
            uppercase_next = false;
            output.push(c.to_ascii_uppercase());
        }
        // 3. Otherwise, append c to output.
        else {
            output.push(c);
        }
    }

    // 5. Return output.
    output
}

impl CSSStyleProperties {
    // https://drafts.csswg.org/cssom/#dom-cssstyleproperties-cssfloat
    pub(crate) fn css_float(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // The cssFloat attribute, on getting, must return the result of invoking getPropertyValue() with float as
        // argument.
        Ok(self.declaration.get("float"))
    }

    pub(crate) fn set_css_float(&self, _: &mut CallContext, value: Utf16String) -> WebIdlResult<()> {
        // On setting, the attribute must invoke setProperty() with float as first argument, as second argument the
        // given value, and no third argument. Any exceptions thrown must be re-thrown.
        self.declaration.set("float", value)
    }

    pub(crate) fn property_value(
        &self,
        _: &mut CallContext,
        property: &CSSPropertyAccessor,
    ) -> WebIdlResult<Utf16String> {
        Ok(self.declaration.get(property.property_name))
    }

    pub(crate) fn set_property_value(
        &self,
        _: &mut CallContext,
        property: &CSSPropertyAccessor,
        value: Utf16String,
    ) -> WebIdlResult<()> {
        self.declaration.set(property.property_name, value)
    }

    /// Defines the accessors of the CSS properties on the interface prototype object. They are not enumerable, so that
    /// enumerating the object only finds its indexed properties.
    pub(crate) fn define_property_accessors<Getter, Setter>(
        vm: &Vm,
        realm: Gc<Realm>,
        prototype: Gc<Object>,
        getter: Getter,
        setter: Setter,
    ) where
        Getter: Fn(&Vm, &CSSPropertyAccessor) -> ThrowCompletionOr<Value> + Copy + 'static,
        Setter: Fn(&Vm, &CSSPropertyAccessor) -> ThrowCompletionOr<Value> + Copy + 'static,
    {
        let attributes = PropertyAttributes::new(Attribute::CONFIGURABLE | Attribute::WRITABLE);
        let define = |attribute: &str, property_name: &'static str| {
            let key = PropertyKey::from_utf8(attribute);
            let capture = || CSSPropertyAccessor { property_name };
            let get = NativeFunction::create(vm, capture(), getter, 0, &key, Some(realm), Some("get"), None);
            let set = NativeFunction::create(vm, capture(), setter, 1, &key, Some(realm), Some("set"), None);
            prototype.define_direct_accessor(vm, &key, Some(get.upcast()), Some(set.upcast()), attributes);
        };
        for property in css_property_names() {
            // https://drafts.csswg.org/cssom/#dom-cssstyleproperties-camel_cased_attribute
            define(&css_property_to_idl_attribute(property, false), property);
            // https://drafts.csswg.org/cssom/#dom-cssstyleproperties-webkit_cased_attribute
            if property.starts_with("-webkit-") {
                define(&css_property_to_idl_attribute(property, true), property);
            }
            // https://drafts.csswg.org/cssom/#dom-cssstyleproperties-dashed_attribute
            if property.contains('-') {
                define(property, property);
            }
        }
    }
}

/// A new object that owns the declaration block, which LibWeb created.
///
/// # Safety
///
/// `vm` must be the live VM, and `declaration` the live cell of a Web::CSS::CSSStyleProperties.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_css_style_properties_create(vm: *mut c_void, declaration: *mut c_void) {
    assert!(!vm.is_null(), "C++ passes the VM");
    // SAFETY: The caller passes the live VM and a live cell.
    let (vm, declaration) = unsafe { (&*vm.cast::<Vm>(), CppCell::from_raw(declaration)) };
    let object = allocate(vm, |_| CSSStyleProperties {
        declaration: CSSStyleDeclaration::new(declaration),
    });
    object.declaration.set_owner(object);
}
