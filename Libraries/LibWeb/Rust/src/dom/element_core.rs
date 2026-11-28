/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The core of an element: the state that names an element, whose layout is defined here, and which C++ embeds in
//! DOM::Element and reads and writes in place.
//!
//! Its fields are AK and DOM types to C++, which owns what they hold: C++ constructs, assigns and destroys every field
//! as the C++ type it sees, and Rust only reads them, in place.

use super::node_core::NodePointer;
use super::node_core::ShadowRootMember;
use core::cell::Cell;
use core::cell::UnsafeCell;
use core::mem::offset_of;

use super::qualified_name::QualifiedNameField;
use super::strings::FlyStringField;
use super::strings::OptionalFlyStringField;
use super::strings::StringField;

/// Where DOM::Element embeds its core, in bytes from the start of the element's cell, which Element.cpp asserts.
pub const ELEMENT_CORE_OFFSET: usize = 136;

/// A field that holds the classes of an element, laid out as C++'s DOM::ClassNames, which is what C++ sees it as: one
/// class in place, or all of them in an outline buffer, which C++ allocates and frees.
///
/// NB: This mirrors the C++ layout, which ClassNames.cpp asserts.
#[repr(C)]
pub struct ClassNamesField {
    size: UnsafeCell<u32>,
    _capacity: UnsafeCell<u32>,
    outline_buffer: UnsafeCell<*const FlyStringField>,
    inline_class: FlyStringField,
}

impl ClassNamesField {
    /// The classes, in order.
    #[inline(always)]
    pub fn as_slice(&self) -> &[FlyStringField] {
        // SAFETY: C++ does not write the field while Rust borrows it. The classes are in the outline buffer if there is
        //         one, and in place otherwise, and the size counts them.
        unsafe {
            let outline_buffer = *self.outline_buffer.get();
            let classes = match outline_buffer.is_null() {
                true => &raw const self.inline_class,
                false => outline_buffer,
            };
            ak::slice_from_raw_parts(classes, *self.size.get() as usize)
        }
    }
}

const _: () = assert!(size_of::<ClassNamesField>() == 24 && align_of::<ClassNamesField>() == 8);
const _: () = assert!(offset_of!(ClassNamesField, size) == 0);
const _: () = assert!(offset_of!(ClassNamesField, _capacity) == 4);
const _: () = assert!(offset_of!(ClassNamesField, outline_buffer) == 8);
const _: () = assert!(offset_of!(ClassNamesField, inline_class) == 16);

/// An attribute of an element, laid out as C++'s DOM::AttributeList::Attribute, which C++ constructs, assigns and
/// destroys in the list that holds it.
/// https://dom.spec.whatwg.org/#concept-attribute
///
/// NB: This mirrors the C++ layout, which AttributeList.cpp asserts.
#[repr(C)]
pub struct Attribute {
    name: QualifiedNameField,
    value: StringField,
}

impl Attribute {
    #[inline(always)]
    pub fn name(&self) -> &QualifiedNameField {
        &self.name
    }

    /// https://dom.spec.whatwg.org/#concept-attribute-value
    #[inline(always)]
    pub fn value(&self) -> &StringField {
        &self.value
    }
}

const _: () = assert!(size_of::<Attribute>() == 16 && align_of::<Attribute>() == 8);
const _: () = assert!(offset_of!(Attribute, name) == 0);
const _: () = assert!(offset_of!(Attribute, value) == 8);

/// The start of a DOM::AttributeList, whose attributes follow it in its allocation.
///
/// NB: This mirrors the C++ layout, which AttributeList.cpp asserts.
#[repr(C)]
struct AttributeListHeader {
    size: UnsafeCell<u32>,
    _capacity: UnsafeCell<u32>,
}

const _: () = assert!(size_of::<AttributeListHeader>() == 8 && align_of::<Attribute>() == 8);
const _: () = assert!(offset_of!(AttributeListHeader, size) == 0);
const _: () = assert!(offset_of!(AttributeListHeader, _capacity) == 4);

/// A field that holds the attributes of an element, laid out as C++'s AK::OwnPtr<DOM::AttributeList>, which is what C++
/// sees it as: the list, which C++ allocates and frees, or null for an element that never had an attribute.
#[repr(transparent)]
pub struct AttributeListField(UnsafeCell<*const AttributeListHeader>);

impl AttributeListField {
    /// The attributes, in order.
    #[inline(always)]
    pub fn as_slice(&self) -> &[Attribute] {
        // SAFETY: C++ does not write the field, or the list it owns, while Rust borrows it. The size counts the
        //         attributes that follow the list's header.
        unsafe {
            let list = *self.0.get();
            if list.is_null() {
                return &[];
            }
            ak::slice_from_raw_parts(list.add(1).cast::<Attribute>(), *(*list).size.get() as usize)
        }
    }
}

const _: () = assert!(size_of::<AttributeListField>() == 8 && align_of::<AttributeListField>() == 8);

// The flags of an element's core, a bit each in its flags byte, which C++ sets where the state they stand for changes.

/// Whether the element is a DOM::HTMLElement, which C++ sets as it constructs one, and never changes.
pub const ELEMENT_IS_HTML_ELEMENT: u8 = 1 << 0;
/// Whether the element has insertion steps, removing steps or post-connection steps beyond those of every element and
/// every HTML element, as its class has, or as a form-associated element has. C++ sets it as it constructs an element of
/// such a class, and as a form-associated custom element definition is attached to an element. It is never cleared.
pub const ELEMENT_HAS_OWN_TREE_STEPS: u8 = 1 << 1;
/// Whether the element is an inert HTML element.
/// https://html.spec.whatwg.org/multipage/interaction.html#inert
pub const ELEMENT_IS_INERT: u8 = 1 << 2;
/// Whether the element is an HTML element whose contenteditable attribute is in a state other than "inherit".
pub const ELEMENT_HAS_CONTENT_EDITABLE_STATE: u8 = 1 << 3;
/// Whether the element may have synthetic pseudo-elements, which C++ sets as it first makes one, and never clears.
pub const ELEMENT_MAY_HAVE_SYNTHETIC_PSEUDO_ELEMENTS: u8 = 1 << 4;

/// The state that names an element, which C++ embeds in DOM::Element as its only storage of it.
/// https://dom.spec.whatwg.org/#concept-element
#[repr(C)]
pub struct ElementCore {
    // NB: C++ sets the name as it constructs the element, and only changes its prefix after that.
    qualified_name: QualifiedNameField,
    /// The value of the element's id attribute, or none where it has none or an empty one.
    id: OptionalFlyStringField,
    /// The tokens of the element's class attribute.
    classes: ClassNamesField,
    /// https://dom.spec.whatwg.org/#concept-element-attribute
    attributes: AttributeListField,
    /// A filter over the local names of the attributes and the classes of this element and its descendants: a name
    /// whose bit is clear is on none of them. C++ only ever adds bits.
    subtree_name_filter: Cell<u64>,
    /// Whether script changed the declarations of the element's style attribute since C++ last wrote them to the
    /// attribute, which C++ does as the attribute is next read.
    style_attribute_is_dirty: Cell<bool>,
    /// The ELEMENT_* flags.
    flags: Cell<u8>,
    /// https://dom.spec.whatwg.org/#concept-element-shadow-root
    shadow_root: ShadowRootMember,
}

impl ElementCore {
    /// https://dom.spec.whatwg.org/#concept-element-qualified-name
    #[inline(always)]
    pub fn qualified_name(&self) -> &QualifiedNameField {
        &self.qualified_name
    }

    /// https://dom.spec.whatwg.org/#concept-id
    #[inline(always)]
    pub fn id(&self) -> Option<&FlyStringField> {
        self.id.get()
    }

    /// https://dom.spec.whatwg.org/#concept-class
    #[inline(always)]
    pub fn classes(&self) -> &[FlyStringField] {
        self.classes.as_slice()
    }

    /// https://dom.spec.whatwg.org/#concept-element-attribute
    #[inline(always)]
    pub fn attributes(&self) -> &[Attribute] {
        self.attributes.as_slice()
    }

    #[inline(always)]
    pub fn subtree_name_filter(&self) -> u64 {
        self.subtree_name_filter.get()
    }

    #[inline(always)]
    pub fn style_attribute_is_dirty(&self) -> bool {
        self.style_attribute_is_dirty.get()
    }

    #[inline(always)]
    pub fn flags(&self) -> u8 {
        self.flags.get()
    }

    /// https://dom.spec.whatwg.org/#concept-element-shadow-root
    #[inline(always)]
    pub fn shadow_root(&self) -> Option<NodePointer> {
        self.shadow_root.get()
    }
}

#[cfg(test)]
impl ClassNamesField {
    /// The classes `classes`, in place for one and in a buffer the test leaks for more.
    pub(crate) fn for_test(classes: &[&str]) -> Self {
        let mut fields: Vec<FlyStringField> = classes.iter().map(|class| FlyStringField::for_test(class)).collect();
        let inline_class = match classes.len() {
            1 => fields.pop().unwrap(),
            _ => FlyStringField::for_test(""),
        };
        let outline_buffer = match fields.is_empty() {
            true => core::ptr::null(),
            false => Vec::leak(fields).as_ptr(),
        };
        Self {
            size: UnsafeCell::new(classes.len() as u32),
            _capacity: UnsafeCell::new(classes.len().max(1) as u32),
            outline_buffer: UnsafeCell::new(outline_buffer),
            inline_class,
        }
    }
}

#[cfg(test)]
impl ElementCore {
    pub(crate) fn for_test(qualified_name: QualifiedNameField) -> Self {
        Self {
            qualified_name,
            id: OptionalFlyStringField::for_test(None),
            classes: ClassNamesField::for_test(&[]),
            attributes: AttributeListField(UnsafeCell::new(core::ptr::null())),
            subtree_name_filter: Cell::new(0),
            style_attribute_is_dirty: Cell::new(false),
            flags: Cell::new(0),
            shadow_root: ShadowRootMember::null_for_test(),
        }
    }

    pub(crate) fn set_shadow_root_for_test(&self, shadow_root: Option<NodePointer>) {
        self.shadow_root.set_for_test(shadow_root);
    }

    /// The core with the attributes `attributes`, in a list the test leaks.
    pub(crate) fn with_attributes_for_test(mut self, attributes: Vec<(QualifiedNameField, &str)>) -> Self {
        let count = attributes.len();
        let words = Vec::leak(vec![0u64; 1 + count * size_of::<Attribute>() / size_of::<u64>()]);
        let list = words.as_mut_ptr().cast::<AttributeListHeader>();
        // SAFETY: The words hold the header and the attributes that follow it, and are aligned for both.
        unsafe {
            list.write(AttributeListHeader {
                size: UnsafeCell::new(count as u32),
                _capacity: UnsafeCell::new(count as u32),
            });
            for (index, (name, value)) in attributes.into_iter().enumerate() {
                list.add(1).cast::<Attribute>().add(index).write(Attribute {
                    name,
                    value: StringField::for_test(value),
                });
            }
        }
        self.attributes = AttributeListField(UnsafeCell::new(list));
        self
    }

    pub(crate) fn with_flags_for_test(self, flags: u8) -> Self {
        self.flags.set(flags);
        self
    }

    pub(crate) fn with_subtree_name_filter_for_test(self, filter: u64) -> Self {
        self.subtree_name_filter.set(filter);
        self
    }

    pub(crate) fn with_id_for_test(mut self, id: &str) -> Self {
        self.id = OptionalFlyStringField::for_test(Some(id));
        self
    }

    pub(crate) fn with_classes_for_test(mut self, classes: &[&str]) -> Self {
        self.classes = ClassNamesField::for_test(classes);
        self
    }
}

// NB: Element.cpp asserts the same layout of the struct it includes.
const _: () = assert!(size_of::<ElementCore>() == 64 && align_of::<ElementCore>() == 8);
const _: () = assert!(offset_of!(ElementCore, qualified_name) == 0);
const _: () = assert!(offset_of!(ElementCore, id) == 8);
const _: () = assert!(offset_of!(ElementCore, classes) == 16);
const _: () = assert!(offset_of!(ElementCore, attributes) == 40);
const _: () = assert!(offset_of!(ElementCore, subtree_name_filter) == 48);
const _: () = assert!(offset_of!(ElementCore, style_attribute_is_dirty) == 56);
const _: () = assert!(offset_of!(ElementCore, flags) == 57);
const _: () = assert!(offset_of!(ElementCore, shadow_root) == 60);

#[cfg(test)]
mod tests {
    use super::*;

    fn identities(classes: &[FlyStringField]) -> Vec<usize> {
        classes.iter().map(FlyStringField::raw_identity).collect()
    }

    fn expected(classes: &[&str]) -> Vec<usize> {
        classes
            .iter()
            .map(|class| FlyStringField::for_test(class).raw_identity())
            .collect()
    }

    #[test]
    fn classes_are_read_in_place_or_from_the_outline_buffer() {
        assert!(ClassNamesField::for_test(&[]).as_slice().is_empty());
        assert_eq!(
            identities(ClassNamesField::for_test(&["a"]).as_slice()),
            expected(&["a"])
        );
        assert_eq!(
            identities(ClassNamesField::for_test(&["a", "b", "c"]).as_slice()),
            expected(&["a", "b", "c"])
        );
    }

    #[test]
    fn attributes_are_read_in_order_from_their_list() {
        let core = ElementCore::for_test(QualifiedNameField::for_test("a", None));
        assert!(core.attributes().is_empty());
        let core = core.with_attributes_for_test(vec![
            (QualifiedNameField::for_test("href", None), "https://example.com/"),
            (
                QualifiedNameField::for_test("href", Some("http://www.w3.org/1999/xlink")),
                "",
            ),
        ]);
        let attributes = core.attributes();
        assert_eq!(attributes.len(), 2);
        assert_eq!(
            attributes[0].name().local_name().raw_identity(),
            FlyStringField::for_test("href").raw_identity()
        );
        assert!(attributes[0].name().namespace().is_none());
        assert!(matches!(
            attributes[0].value().units(),
            ak::Utf16StringUnits::Ascii(b"https://example.com/")
        ));
        assert!(attributes[1].name().namespace().is_some());
        assert!(matches!(
            attributes[1].value().units(),
            ak::Utf16StringUnits::Ascii(b"")
        ));
    }

    #[test]
    fn the_id_is_none_until_set() {
        let core = ElementCore::for_test(QualifiedNameField::for_test("div", None));
        assert!(core.id().is_none());
        let core = core.with_id_for_test("main");
        assert_eq!(
            core.id().map(FlyStringField::raw_identity),
            Some(FlyStringField::for_test("main").raw_identity())
        );
    }
}
