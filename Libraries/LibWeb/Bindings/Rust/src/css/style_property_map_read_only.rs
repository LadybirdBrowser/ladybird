/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! StylePropertyMapReadOnly, https://drafts.css-houdini.org/css-typed-om-1/#stylepropertymapreadonly

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{
    Array, DeferGc, EmbedderCell, EmbedderClass, EmbedderValue, Gc, MarkedVec, Object, Trace, Value, Vm,
};
use libweb_rust::typed_om::RetainedStyleValue;

use super::css_style_value::{AnyCSSStyleValue, CSSStyleValue};
use super::typed_om::{append_style_value, reify, to_rust_string};
use crate::bindings::conversions::{ToJs, UsvString};
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate, implementation_if, implementation_of};
use crate::bindings::iterable::{IterationKind, PairIterable};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::css::style_property_map::StylePropertyMap;

unsafe extern "C" {
    fn web_css_style_map_property(name: usize, id: *mut u16, canonical_name: *mut usize) -> bool;
    fn web_css_style_map_iterations(
        is_computed: bool,
        source: *mut c_void,
        name: usize,
        values: *mut c_void,
        append: unsafe extern "C" fn(*mut c_void, *const c_void),
    ) -> u8;
    fn web_css_style_map_has(is_computed: bool, source: *mut c_void, name: usize) -> bool;
    fn web_css_style_map_property_names(
        is_computed: bool,
        source: *mut c_void,
        names: *mut c_void,
        append: unsafe extern "C" fn(*mut c_void, usize),
    );
    fn web_css_style_map_property_count(is_computed: bool, source: *mut c_void) -> usize;
}

/// The numeric value of SerializationMode::Normal of C++.
pub(super) const SERIALIZATION_MODE_NORMAL: u8 = 0;
/// The numeric value of SerializationMode::ResolvedValue of C++.
const SERIALIZATION_MODE_RESOLVED_VALUE: u8 = 1;

/// What web_css_style_map_iterations() found.
const ITERATIONS_ABSENT: u8 = 0;
const ITERATIONS_SHORTHAND: u8 = 2;

/// A valid CSS property, https://drafts.css-houdini.org/css-typed-om-1/#valid-css-property
pub(crate) struct Property {
    pub(crate) id: u16,
    pub(crate) name: Utf16String,
}

impl Property {
    /// The steps that every method of a style map starts with:
    /// 1. If property is not a custom property name string, set property to property ASCII lowercased.
    /// 2. If property is not a valid CSS property, throw a TypeError.
    pub(crate) fn from_name(name: &Utf16String) -> WebIdlResult<Self> {
        let (mut id, mut canonical_name) = (0, 0);
        // SAFETY: This hands over a reference to the string, and the results are lent for the call.
        let is_valid =
            unsafe { web_css_style_map_property(name.clone().into_raw(), &raw mut id, &raw mut canonical_name) };
        if !is_valid {
            return Err(Exception::TypeError(Utf16String::from_utf8(&format!(
                "'{}' is not a valid CSS property",
                to_rust_string(name)
            ))));
        }
        Ok(Self {
            id,
            // SAFETY: C++ hands over a reference to a string.
            name: unsafe { Utf16String::from_raw_owned(canonical_name) },
        })
    }
}

/// https://drafts.css-houdini.org/css-typed-om-1/#dom-stylepropertymapreadonly-declarations-slot
/// NB: We just directly refer to our source, at least for now.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
enum Declarations {
    /// The Element whose computed style this reflects.
    ComputedStyle(CppCell),
    /// The CSSStyleDeclaration that this reflects.
    Declared(CppCell),
}

/// The value pairs of a style map when an iteration started.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ValuePairs {
    pairs: Vec<(Value, Value)>,
}

static VALUE_PAIRS_CLASS: EmbedderClass<ValuePairs> = EmbedderClass::new("StylePropertyMapReadOnly value pairs");

impl EmbedderValue for ValuePairs {
    const CLASS: &'static EmbedderClass<Self> = &VALUE_PAIRS_CLASS;
}

// https://drafts.css-houdini.org/css-typed-om-1/#stylepropertymapreadonly
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct StylePropertyMapReadOnly {
    declarations: Declarations,
}

impl Wrappable for StylePropertyMapReadOnly {}

impl Inherits<StylePropertyMapReadOnly> for StylePropertyMapReadOnly {
    fn parent(&self) -> &StylePropertyMapReadOnly {
        self
    }
}

impl PairIterable for StylePropertyMapReadOnly {
    type Key = Value;
    type Value = Value;
    type Target = Gc<EmbedderCell<ValuePairs>>;

    /// INTEROP: Snapshot the entry list when iteration starts, matching Chrome and Safari.
    fn iteration_target(cx: &CallContext, wrapper: Gc<Object>, kind: IterationKind) -> Self::Target {
        let pairs = if let Some(map) = implementation_if::<StylePropertyMap>(wrapper) {
            map.parent().value_pairs(cx, kind)
        } else {
            implementation_of::<Self>(wrapper).value_pairs(cx, kind)
        };
        VALUE_PAIRS_CLASS.allocate(cx.vm(), |defer_gc| ValuePairs {
            pairs: pairs.to_pairs(defer_gc),
        })
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#stylepropertymapreadonly
    fn value_pairs_to_iterate_over(target: &Self::Target) -> impl core::ops::Deref<Target = [(Value, Value)]> + '_ {
        target.pairs.as_slice()
    }
}

/// The keys and values of value pairs, one after the other, which stay alive while more of them are created.
struct MarkedValuePairs<'vm>(MarkedVec<'vm, Value>);

impl MarkedValuePairs<'_> {
    fn to_pairs(&self, _: &DeferGc<'_>) -> Vec<(Value, Value)> {
        let values = self.0.to_vec();
        let (pairs, _) = values.as_chunks::<2>();
        pairs.iter().map(|&[key, value]| (key, value)).collect()
    }
}

impl StylePropertyMapReadOnly {
    pub(crate) fn new_declared(declaration: CppCell) -> Self {
        Self {
            declarations: Declarations::Declared(declaration),
        }
    }

    pub(crate) fn create_computed_style(vm: &Vm, element: CppCell) -> Ref<Self> {
        allocate(vm, |_| Self {
            declarations: Declarations::ComputedStyle(element),
        })
    }

    /// The CSSStyleDeclaration of a map that reflects one.
    pub(crate) fn declaration(&self) -> *mut c_void {
        match &self.declarations {
            Declarations::Declared(declaration) => declaration.as_ptr(),
            Declarations::ComputedStyle(_) => {
                unreachable!("writable StylePropertyMaps are backed by a CSSStyleDeclaration")
            }
        }
    }

    fn source(&self) -> (bool, *mut c_void) {
        match &self.declarations {
            Declarations::ComputedStyle(element) => (true, element.as_ptr()),
            Declarations::Declared(declaration) => (false, declaration.as_ptr()),
        }
    }

    /// INTEROP: Computed border-image values represent all four sides or both axes, even when their serialization
    ///          collapses to a single numeric value or keyword.
    fn reify_property_value(
        vm: &Vm,
        value: &RetainedStyleValue,
        property: &Property,
        is_computed: bool,
    ) -> AnyCSSStyleValue {
        let name = to_rust_string(&property.name);
        let is_border_image_value = matches!(
            name.as_str(),
            "border-image-width" | "border-image-outset" | "border-image-slice" | "border-image-repeat"
        );
        if is_computed && is_border_image_value {
            return AnyCSSStyleValue::CSSStyleValue(CSSStyleValue::create(
                vm,
                property.name.clone(),
                value.clone(),
                SERIALIZATION_MODE_NORMAL,
            ));
        }
        reify(vm, value, &property.name)
    }

    /// The reified iterations of props[property], up to the limit. There are none if props[property] does not exist.
    fn reified_iterations<'vm>(
        &self,
        vm: &'vm Vm,
        property: &Property,
        limit: usize,
    ) -> MarkedVec<'vm, AnyCSSStyleValue> {
        let (is_computed, source) = self.source();
        let mut iterations: Vec<RetainedStyleValue> = Vec::new();
        // SAFETY: The source is live, this hands over a reference to the string, and the values are lent for the call.
        let kind = unsafe {
            web_css_style_map_iterations(
                is_computed,
                source,
                property.name.clone().into_raw(),
                (&raw mut iterations).cast(),
                append_style_value,
            )
        };

        let results = MarkedVec::new(vm);
        if kind == ITERATIONS_ABSENT {
            return results;
        }
        if kind == ITERATIONS_SHORTHAND {
            results.push(AnyCSSStyleValue::CSSStyleValue(CSSStyleValue::create(
                vm,
                property.name.clone(),
                iterations.remove(0),
                SERIALIZATION_MODE_RESOLVED_VALUE,
            )));
            return results;
        }
        for value in iterations.iter().take(limit) {
            results.push(Self::reify_property_value(vm, value, property, is_computed));
        }
        results
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-stylepropertymapreadonly-get
    pub(crate) fn get(&self, cx: &mut CallContext, property: UsvString) -> WebIdlResult<Option<AnyCSSStyleValue>> {
        // The get(property) method, when called on a StylePropertyMapReadOnly this, must perform the following steps:

        // 1. If property is not a custom property name string, set property to property ASCII lowercased.
        // 2. If property is not a valid CSS property, throw a TypeError.
        let property = Property::from_name(&property.into_utf16_string())?;

        // 3. Let props be the value of this’s [[declarations]] internal slot.
        // 4. If props[property] exists, subdivide into iterations props[property], then reify the first item of the
        //    result and return it.
        // 5. Otherwise, return undefined.
        Ok(self.reified_iterations(cx.vm(), &property, 1).get(0))
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-stylepropertymapreadonly-getall
    pub(crate) fn get_all<'vm>(
        &self,
        cx: &mut CallContext<'vm>,
        property: UsvString,
    ) -> WebIdlResult<MarkedVec<'vm, AnyCSSStyleValue>> {
        // The getAll(property) method, when called on a StylePropertyMap this, must perform the following steps:

        // 1. If property is not a custom property name string, set property to property ASCII lowercased.
        // 2. If property is not a valid CSS property, throw a TypeError.
        let property = Property::from_name(&property.into_utf16_string())?;

        // 3. Let props be the value of this’s [[declarations]] internal slot.
        // 4. If props[property] exists, subdivide into iterations props[property], then reify each item of the result,
        //    and return the list.
        // 5. Otherwise, return an empty list.
        Ok(self.reified_iterations(cx.vm(), &property, usize::MAX))
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-stylepropertymapreadonly-has
    pub(crate) fn has(&self, _: &mut CallContext, property: UsvString) -> WebIdlResult<bool> {
        // The has(property) method, when called on a StylePropertyMapReadOnly this, must perform the following steps:

        // 1. If property is not a custom property name string, set property to property ASCII lowercased.
        // 2. If property is not a valid CSS property, throw a TypeError.
        let property = Property::from_name(&property.into_utf16_string())?;

        // 3. Let props be the value of this’s [[declarations]] internal slot.
        // 4. If props[property] exists, return true. Otherwise, return false.
        let (is_computed, source) = self.source();
        // SAFETY: The source is live, and this hands over a reference to the string.
        Ok(unsafe { web_css_style_map_has(is_computed, source, property.name.into_raw()) })
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-stylepropertymapreadonly-size
    pub(crate) fn size(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        // The size attribute, on getting from a StylePropertyMapReadOnly this, must perform the following steps:

        // 1. Return the size of the value of this’s [[declarations]] internal slot.
        let (is_computed, source) = self.source();
        // SAFETY: The source is live.
        let size = unsafe { web_css_style_map_property_count(is_computed, source) };
        Ok(u32::try_from(size).expect("a map has fewer than 2^32 properties"))
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-stylepropertymapreadonly-declarations-slot
    fn property_names(&self) -> Vec<Utf16String> {
        unsafe extern "C" fn append_name(names: *mut c_void, name: usize) {
            // SAFETY: The context is the vector below, and C++ hands over a reference to a string.
            unsafe { (*names.cast::<Vec<Utf16String>>()).push(Utf16String::from_raw_owned(name)) };
        }

        let (is_computed, source) = self.source();
        let mut names: Vec<Utf16String> = Vec::new();
        // SAFETY: The source is live, and the names are lent for the call.
        unsafe { web_css_style_map_property_names(is_computed, source, (&raw mut names).cast(), append_name) };

        // INTEROP: Inline and declared maps preserve declaration order.
        if is_computed {
            // Unless otherwise stated, the initial ordering of the [[declarations]] internal slot is based on the key
            // of each entry:
            // 1. Standardized properties (not custom properties or vendor-prefixed properties), ASCII lowercased and
            //    then sorted in increasing code-point order.
            // 2. Vendor-prefixed/experimental properties (those whose name starts with a single dash), ASCII lowercased
            //    and then sorted in increasing code-point order.
            // 3. Custom properties, sorted in increasing code-point order. (These are never lower-cased; they are
            //    preserved exactly as written.)
            names.sort_by_cached_key(|name| {
                let name = name.to_utf16();
                let category = match name.as_ref() {
                    [0x2d, 0x2d, ..] => 2,
                    [0x2d, ..] => 1,
                    _ => 0,
                };
                let code_points: Vec<u32> = char::decode_utf16(name.iter().copied())
                    .map(|unit| unit.map_or_else(|error| u32::from(error.unpaired_surrogate()), u32::from))
                    .collect();
                (category, code_points)
            });
        }
        names
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#stylepropertymapreadonly
    fn value_pairs<'vm>(&self, cx: &CallContext<'vm>, kind: IterationKind) -> MarkedValuePairs<'vm> {
        // The value pairs to iterate over for a StylePropertyMapReadOnly object this are obtained as follows:
        // 1. Let declarations be this’s [[declarations]] slot.
        // 2. Let value pairs be an empty list.
        let value_pairs = MarkedVec::new(cx.vm());

        // 3. For each prop → value in declarations:
        for name in self.property_names() {
            let key = name.clone().to_js(cx).expect("a string converts to a JS value");
            value_pairs.push(key);

            // NB: An iteration over the keys does not read the values.
            if matches!(kind, IterationKind::Key) {
                value_pairs.push(Value::UNDEFINED);
                continue;
            }

            // 1. Let iterations be the result of dividing into iterations value.
            // 2. Reify each item of iterations, and let objects be the result.
            let Ok(property) = Property::from_name(&name) else {
                unreachable!("the name of a declaration is a valid CSS property");
            };
            let objects = self.reified_iterations(cx.vm(), &property, usize::MAX);

            // 3. Append prop/objects to value pairs.
            let wrappers = MarkedVec::with_capacity(cx.vm(), objects.len());
            for object in objects.values().iter() {
                wrappers.push(object.to_js(cx).expect("an object converts to a JS value"));
            }
            let array = Array::create_from(cx.vm(), cx.realm(), &wrappers.to_vec());
            value_pairs.push(Value::from_object(array.upcast::<Object>()));
        }

        // 4. Return value pairs.
        MarkedValuePairs(value_pairs)
    }
}
