/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The core of an element: the state that names an element, whose layout is defined here, and which C++ embeds in
//! DOM::Element and reads and writes in place.
//!
//! Its fields are AK and DOM types to C++, which owns what they hold: C++ constructs, assigns and destroys every field
//! as the C++ type it sees, and Rust reads them in place. What Rust writes is the attribute list, whose attributes it
//! changes the values of and takes out (see attributes.rs), while C++ allocates the list and appends to it, the id and
//! the subtree name filter, and the classes, whose storage only Rust changes, for C++ too.

use super::node_core::ElementMember;
use super::node_core::NodePointer;
use super::node_core::ShadowRootMember;
use core::cell::Cell;
use core::cell::UnsafeCell;
use core::mem::offset_of;
use core::ptr::NonNull;

use super::qualified_name::QualifiedNameField;
use super::qualified_name::QualifiedNameImpl;
use super::strings::FlyStringField;
use super::strings::OptionalFlyStringField;
use super::strings::StringField;

/// Where DOM::Element embeds its core, in bytes from the start of the element's cell, which Element.cpp asserts.
pub const ELEMENT_CORE_OFFSET: usize = 144;

/// A field that holds the classes of an element, laid out as C++'s DOM::ClassNames, which is what C++ sees it as: one
/// class in place, or all of them in an outline buffer. C++ moves the field, and changes it, and drops it, through the
/// functions below, so that only Rust allocates and frees the buffer.
///
/// NB: This mirrors the C++ layout, which ClassNames.cpp asserts.
#[repr(C)]
pub struct ClassNamesField {
    size: UnsafeCell<u32>,
    /// The number of classes the outline buffer has room for, or 1, for the class in place, where there is none.
    capacity: UnsafeCell<u32>,
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

    /// Removes every class.
    ///
    /// # Safety
    ///
    /// Nothing may borrow the classes.
    pub(super) unsafe fn clear(&self) {
        // SAFETY: Guaranteed by the caller. The field owns a reference to each class it holds, and the outline buffer, if
        //         there is one, is a vector this field allocated with the capacity it keeps.
        unsafe {
            let size = core::ptr::replace(self.size.get(), 0) as usize;
            let capacity = core::ptr::replace(self.capacity.get(), 1) as usize;
            let outline_buffer = core::ptr::replace(self.outline_buffer.get(), core::ptr::null());
            drop(self.inline_class.replace(empty_fly_string()));
            if !outline_buffer.is_null() {
                let classes = Vec::from_raw_parts(outline_buffer.cast::<usize>().cast_mut(), size, capacity);
                for class in classes {
                    drop(ak::Utf16FlyString::from_raw_owned(class));
                }
            }
        }
    }

    /// Appends the class `class`.
    ///
    /// # Safety
    ///
    /// Nothing may borrow the classes.
    pub(super) unsafe fn append(&self, class: ak::Utf16FlyString) {
        // SAFETY: Guaranteed by the caller. The outline buffer, if there is one, is a vector this field allocated with the
        //         capacity it keeps, and holds `size` classes.
        unsafe {
            let size = *self.size.get();
            let outline_buffer = *self.outline_buffer.get();
            if outline_buffer.is_null() && size == 0 {
                drop(self.inline_class.replace(class));
                *self.size.get() = 1;
                return;
            }
            let mut classes = match outline_buffer.is_null() {
                true => vec![self.inline_class.replace(empty_fly_string()).into_raw()],
                false => Vec::from_raw_parts(
                    outline_buffer.cast::<usize>().cast_mut(),
                    size as usize,
                    *self.capacity.get() as usize,
                ),
            };
            if classes.len() == classes.capacity() {
                classes.reserve_exact(classes.capacity().max(4));
            }
            classes.push(class.into_raw());
            let mut classes = core::mem::ManuallyDrop::new(classes);
            *self.size.get() = u32::try_from(classes.len()).expect("an element has fewer than 2^32 classes");
            *self.capacity.get() = u32::try_from(classes.capacity()).expect("an element has fewer than 2^32 classes");
            *self.outline_buffer.get() = classes.as_mut_ptr().cast::<FlyStringField>().cast_const();
        }
    }
}

#[inline(always)]
fn empty_fly_string() -> ak::Utf16FlyString {
    ak::Utf16FlyString::from_utf8("")
}

/// Appends the class `class`, whose reference passes to the call, to the classes `classes`.
///
/// # Safety
///
/// `classes` must be a live DOM::ClassNames that nothing borrows the classes of, and `class` the raw word of a
/// reference to an AK::Utf16FlyString.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_class_names_append(classes: &ClassNamesField, class: usize) {
    // SAFETY: Guaranteed by the caller.
    unsafe { classes.append(ak::Utf16FlyString::from_raw_owned(class)) }
}

/// Removes every class from the classes `classes`, which frees their storage.
///
/// # Safety
///
/// `classes` must be a live DOM::ClassNames that nothing borrows the classes of.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_class_names_clear(classes: &ClassNamesField) {
    // SAFETY: Guaranteed by the caller.
    unsafe { classes.clear() }
}

const _: () = assert!(size_of::<ClassNamesField>() == 24 && align_of::<ClassNamesField>() == 8);
const _: () = assert!(offset_of!(ClassNamesField, size) == 0);
const _: () = assert!(offset_of!(ClassNamesField, capacity) == 4);
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

impl AttributeListField {
    /// The attributes after the list's header, and the count of them.
    #[inline(always)]
    fn raw_parts(&self) -> Option<(*mut Attribute, *mut u32)> {
        // SAFETY: C++ does not write the field while Rust reads it.
        let list = unsafe { *self.0.get() }.cast_mut();
        if list.is_null() {
            return None;
        }
        // SAFETY: The attributes follow the list's header, in its allocation.
        Some((unsafe { list.add(1).cast::<Attribute>() }, unsafe {
            (*list).size.get()
        }))
    }

    /// Stores `value` as the value of the attribute at `index`, and gives back the value it had, whose reference passes
    /// to the caller.
    ///
    /// # Safety
    ///
    /// Nothing may borrow the attribute's value.
    #[inline(always)]
    pub(super) unsafe fn replace_value(&self, index: usize, value: ak::Utf16String) -> ak::Utf16String {
        // SAFETY: Guaranteed by the caller, and the attribute at `index` is in the list, which as_slice() checks.
        unsafe { self.as_slice()[index].value.replace(value) }
    }

    /// Takes the attribute at `index` out of the list, moving the attributes after it down by one, as
    /// DOM::AttributeList::remove() does, and gives back its name and value, whose references pass to the caller.
    ///
    /// # Safety
    ///
    /// Nothing may borrow an attribute of the list.
    pub(super) unsafe fn take(&self, index: usize) -> (NonNull<QualifiedNameImpl>, ak::Utf16String) {
        let (attributes, size) = self
            .raw_parts()
            .expect("an attribute is taken out of a list that holds it");
        // SAFETY: Guaranteed by the caller: nothing borrows the list, whose size counts the attributes that follow its
        //         header. An attribute is a qualified name and a string, each a word that holds a reference, which
        //         moves with the word.
        unsafe {
            let count = *size as usize;
            assert!(index < count);
            let attribute = attributes.add(index);
            let name = attribute.cast::<NonNull<QualifiedNameImpl>>().read();
            let value = attribute.cast::<usize>().add(1).read();
            core::ptr::copy(attribute.add(1), attribute, count - index - 1);
            *size -= 1;
            (name, ak::Utf16String::from_raw_owned(value))
        }
    }
}

const _: () = assert!(size_of::<AttributeListField>() == 8 && align_of::<AttributeListField>() == 8);

// The flags of an element's core, a bit each in its flags byte, which C++ sets where the state they stand for changes.

/// Whether the element is a DOM::HTMLElement, which C++ sets as it constructs one, and never changes.
pub const ELEMENT_IS_HTML_ELEMENT: u8 = 1 << 0;
/// Whether the element has insertion steps, removing steps, moving steps or post-connection steps beyond those of every
/// element and every HTML element, as its class has, or as a form-associated, body or frameset element has. C++ sets it
/// as it constructs an element of such a class, and as a form-associated custom element definition is attached to an
/// element. It is never cleared.
pub const ELEMENT_HAS_OWN_TREE_STEPS: u8 = 1 << 1;
/// Whether the element is an inert HTML element.
/// https://html.spec.whatwg.org/multipage/interaction.html#inert
pub const ELEMENT_IS_INERT: u8 = 1 << 2;
/// Whether the element is an HTML element whose contenteditable attribute is in a state other than "inherit".
pub const ELEMENT_HAS_CONTENT_EDITABLE_STATE: u8 = 1 << 3;
/// Whether the element may have synthetic pseudo-elements, which C++ sets as it first makes one, and never clears.
pub const ELEMENT_MAY_HAVE_SYNTHETIC_PSEUDO_ELEMENTS: u8 = 1 << 4;
/// Whether the element is custom, which C++ keeps as the element's custom element state changes.
/// https://dom.spec.whatwg.org/#concept-element-custom
pub const ELEMENT_IS_CUSTOM: u8 = 1 << 5;
/// Whether the element has attribute change steps beyond those of every element and every HTML element, whose names
/// attributes.rs knows, as its class has, or as a form-associated element has. C++ sets it as it constructs an element
/// of such a class, and as a form-associated custom element definition is attached to an element. It is never
/// cleared.
pub const ELEMENT_HAS_OWN_ATTRIBUTE_CHANGE_STEPS: u8 = 1 << 6;
/// Whether the element may have Attr nodes, which C++ sets as it makes the element's NamedNodeMap, and never clears.
pub const ELEMENT_MAY_HAVE_ATTRIBUTE_NODES: u8 = 1 << 7;

// The ELEMENT_REFLECTION_* flags, of an element's reflection_flags.

/// Whether the element has a DOMTokenList for its class attribute, which C++ sets as it makes it, and never clears.
pub const ELEMENT_REFLECTION_HAS_CLASS_LIST: u8 = 1 << 0;

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
    /// whose bit is clear is on none of them. It only ever gains bits.
    subtree_name_filter: Cell<u64>,
    /// Whether script changed the declarations of the element's style attribute since C++ last wrote them to the
    /// attribute, which C++ does as the attribute is next read.
    style_attribute_is_dirty: Cell<bool>,
    /// The ELEMENT_* flags.
    flags: Cell<u8>,
    /// The ELEMENT_REFLECTION_* flags, of the objects that reflect the element's attributes.
    reflection_flags: Cell<u8>,
    /// https://dom.spec.whatwg.org/#concept-element-shadow-root
    shadow_root: ShadowRootMember,
    /// The generation of the parent's child list that `child_index` and `child_index_of_type` were counted in, or 0
    /// for none. See child_index.rs.
    parent_child_index_generation: Cell<u32>,
    /// The element's 1-based index among its element siblings, or 0 where it is not counted yet.
    child_index: Cell<u32>,
    /// The element's 1-based index among its element siblings with its local name and namespace, or 0 where it is not
    /// counted yet.
    child_index_of_type: Cell<u32>,
    /// https://dom.spec.whatwg.org/#slotable-assigned-slot
    assigned_slot: ElementMember,
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
    pub(super) fn attribute_list(&self) -> &AttributeListField {
        &self.attributes
    }

    #[inline(always)]
    pub fn subtree_name_filter(&self) -> u64 {
        self.subtree_name_filter.get()
    }

    #[inline(always)]
    pub(super) fn subtree_name_filter_cell(&self) -> &Cell<u64> {
        &self.subtree_name_filter
    }

    #[inline(always)]
    pub(super) fn id_field(&self) -> &OptionalFlyStringField {
        &self.id
    }

    #[inline(always)]
    pub(super) fn class_names(&self) -> &ClassNamesField {
        &self.classes
    }

    #[inline(always)]
    pub fn reflection_flags(&self) -> u8 {
        self.reflection_flags.get()
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

    /// https://dom.spec.whatwg.org/#slotable-assigned-slot
    #[inline(always)]
    pub fn assigned_slot(&self) -> Option<NodePointer> {
        self.assigned_slot.get()
    }

    #[inline(always)]
    pub(super) fn parent_child_index_generation(&self) -> &Cell<u32> {
        &self.parent_child_index_generation
    }

    #[inline(always)]
    pub(super) fn child_index(&self) -> &Cell<u32> {
        &self.child_index
    }

    #[inline(always)]
    pub(super) fn child_index_of_type(&self) -> &Cell<u32> {
        &self.child_index_of_type
    }
}

#[cfg(test)]
impl ClassNamesField {
    /// The classes `classes`, which the test leaks.
    pub(crate) fn for_test(classes: &[&str]) -> Self {
        let field = Self {
            size: UnsafeCell::new(0),
            capacity: UnsafeCell::new(1),
            outline_buffer: UnsafeCell::new(core::ptr::null()),
            inline_class: FlyStringField::for_test(""),
        };
        for class in classes {
            // SAFETY: Nothing borrows the classes yet.
            unsafe { field.append(ak::Utf16FlyString::from_utf8(class)) };
        }
        field
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
            reflection_flags: Cell::new(0),
            shadow_root: ShadowRootMember::null_for_test(),
            parent_child_index_generation: Cell::new(0),
            child_index: Cell::new(0),
            child_index_of_type: Cell::new(0),
            assigned_slot: ElementMember::null_for_test(),
        }
    }

    pub(crate) fn set_classes_for_test(&mut self, classes: &[&str]) {
        self.classes = ClassNamesField::for_test(classes);
    }

    pub(crate) fn set_shadow_root_for_test(&self, shadow_root: Option<NodePointer>) {
        self.shadow_root.set_for_test(shadow_root);
    }

    pub(crate) fn set_assigned_slot_for_test(&self, slot: Option<NodePointer>) {
        self.assigned_slot.set_for_test(slot);
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
const _: () = assert!(size_of::<ElementCore>() == 80 && align_of::<ElementCore>() == 8);
const _: () = assert!(offset_of!(ElementCore, qualified_name) == 0);
const _: () = assert!(offset_of!(ElementCore, id) == 8);
const _: () = assert!(offset_of!(ElementCore, classes) == 16);
const _: () = assert!(offset_of!(ElementCore, attributes) == 40);
const _: () = assert!(offset_of!(ElementCore, subtree_name_filter) == 48);
const _: () = assert!(offset_of!(ElementCore, style_attribute_is_dirty) == 56);
const _: () = assert!(offset_of!(ElementCore, flags) == 57);
const _: () = assert!(offset_of!(ElementCore, reflection_flags) == 58);
const _: () = assert!(offset_of!(ElementCore, shadow_root) == 60);
const _: () = assert!(offset_of!(ElementCore, parent_child_index_generation) == 64);
const _: () = assert!(offset_of!(ElementCore, child_index) == 68);
const _: () = assert!(offset_of!(ElementCore, child_index_of_type) == 72);
const _: () = assert!(offset_of!(ElementCore, assigned_slot) == 76);

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
