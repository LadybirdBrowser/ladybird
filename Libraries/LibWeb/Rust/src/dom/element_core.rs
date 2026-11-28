/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The core of an element: the state that names an element, whose layout is defined here, and which C++ embeds in
//! DOM::Element and reads and writes in place.
//!
//! Its fields are AK and DOM types to C++, which owns what they hold: C++ constructs, assigns and destroys every field
//! as the C++ type it sees, and Rust reads them in place. What Rust writes is the attribute list, which it allocates,
//! changes and frees, for C++ too (see attributes.rs), the id and the subtree name filter, and the classes, whose
//! storage only Rust changes, for C++ too.

use super::node_core::ElementMember;
use super::node_core::NodePointer;
use super::node_core::ShadowRootMember;
use core::alloc::Layout;
use core::cell::Cell;
use core::cell::UnsafeCell;
use core::mem::offset_of;
use core::ptr::NonNull;

use super::qualified_name;
use super::qualified_name::QualifiedNameField;
use super::qualified_name::QualifiedNameImpl;
use super::strings::FlyStringField;
use super::strings::OptionalFlyStringField;
use super::strings::StringField;
use crate::stage::MainThread;

crate::stage::main_thread_ffi_entries!(without_host);

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

/// An attribute of an element, laid out as C++'s DOM::AttributeList::Attribute, which only Rust constructs, changes and
/// destroys, in the list that holds it.
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

/// The start of an element's attribute list, whose attributes follow it in its allocation.
///
/// NB: AttributeList.cpp asserts the same layout.
#[repr(C)]
struct AttributeListHeader {
    size: UnsafeCell<u32>,
    /// The number of attributes the list has room for, with ATTRIBUTE_LIST_HAS_ATTRIBUTE_MAP set where an
    /// [`AttributeListPrefix`] comes before the header.
    capacity: UnsafeCell<u32>,
}

const _: () = assert!(size_of::<AttributeListHeader>() == 8 && align_of::<AttributeListHeader>() == 4);
const _: () = assert!(offset_of!(AttributeListHeader, size) == 0);
const _: () = assert!(offset_of!(AttributeListHeader, capacity) == 4);

/// The bit of an attribute list's capacity that says an [`AttributeListPrefix`] comes before its header.
pub const ATTRIBUTE_LIST_HAS_ATTRIBUTE_MAP: u32 = 1 << 31;

/// The size of an [`AttributeListPrefix`], which comes this many bytes before the header of a list that has one.
pub const ATTRIBUTE_LIST_PREFIX_SIZE: usize = 8;

/// What comes before the header of the attribute list of an element that script asked for the NamedNodeMap of: the
/// element's map, which the element visits in place as it visits its edges. Elements that never had one, which is
/// nearly all of them, keep no field for it.
#[repr(C)]
struct AttributeListPrefix {
    /// The element's NamedNodeMap, laid out as a GC::Member<DOM::NamedNodeMap>.
    attribute_map: Cell<u32>,
    _padding: u32,
}

const _: () = assert!(size_of::<AttributeListPrefix>() == ATTRIBUTE_LIST_PREFIX_SIZE);
const _: () = assert!(offset_of!(AttributeListPrefix, attribute_map) == 0);

/// A field that holds the attributes of an element, laid out as C++'s DOM::AttributeList, which is what C++ sees it as:
/// a pointer to the list's header, in an allocation Rust makes, grows and frees, or null for an element that never had
/// an attribute. C++ only reads the list.
#[repr(transparent)]
pub struct AttributeListField(UnsafeCell<*mut AttributeListHeader>);

impl AttributeListField {
    /// The attributes, in order.
    #[inline(always)]
    pub fn as_slice(&self) -> &[Attribute] {
        // SAFETY: Nothing writes the list while Rust borrows it. The size counts the attributes that follow the list's
        //         header.
        unsafe {
            let list = *self.0.get();
            if list.is_null() {
                return &[];
            }
            ak::slice_from_raw_parts(list.add(1).cast::<Attribute>(), *(*list).size.get() as usize)
        }
    }

    #[inline(always)]
    fn header(&self) -> *mut AttributeListHeader {
        // SAFETY: Nothing writes the field while Rust reads it.
        unsafe { *self.0.get() }
    }

    /// The number of attributes the list has room for.
    #[inline(always)]
    fn capacity(&self) -> usize {
        let list = self.header();
        if list.is_null() {
            return 0;
        }
        // SAFETY: The list is live.
        (unsafe { *(*list).capacity.get() } & !ATTRIBUTE_LIST_HAS_ATTRIBUTE_MAP) as usize
    }

    /// Whether an AttributeListPrefix comes before the list's header.
    #[inline(always)]
    fn has_prefix(&self) -> bool {
        let list = self.header();
        // SAFETY: The list is live.
        !list.is_null() && unsafe { *(*list).capacity.get() } & ATTRIBUTE_LIST_HAS_ATTRIBUTE_MAP != 0
    }

    /// The layout of a list with room for `capacity` attributes, with a prefix where `has_prefix` says so, and the
    /// offset of its header in it.
    #[inline(always)]
    fn layout(capacity: usize, has_prefix: bool) -> (Layout, usize) {
        let prefix_size = if has_prefix { ATTRIBUTE_LIST_PREFIX_SIZE } else { 0 };
        let size = size_of::<Attribute>()
            .checked_mul(capacity)
            .and_then(|size| size.checked_add(prefix_size + size_of::<AttributeListHeader>()))
            .expect("an attribute list is never that long");
        (
            Layout::from_size_align(size, align_of::<Attribute>()).expect("an attribute list is never that long"),
            prefix_size,
        )
    }

    /// Moves the list into an allocation with room for `capacity` attributes, at least as many as it holds, with a
    /// prefix where `has_prefix` says so, which keeps the element's map where the list had one.
    fn reallocate(&self, capacity: usize, has_prefix: bool) {
        let old_list = self.header();
        let size = self.as_slice().len();
        assert!(capacity >= size);
        let capacity_u32 = u32::try_from(capacity)
            .ok()
            .filter(|&capacity| capacity & ATTRIBUTE_LIST_HAS_ATTRIBUTE_MAP == 0)
            .expect("an element has fewer than 2^31 attributes");
        let old_has_prefix = self.has_prefix();
        let (new_layout, new_header_offset) = Self::layout(capacity, has_prefix);
        // SAFETY: The layout is not zero-sized.
        let allocation = unsafe { std::alloc::alloc(new_layout) };
        if allocation.is_null() {
            std::alloc::handle_alloc_error(new_layout);
        }
        // SAFETY: The new allocation has room for the header and `capacity` attributes after `new_header_offset`
        //         bytes, and the old one, if any, holds `size` attributes after its header, which move with their bytes,
        //         as an attribute is a qualified name and a string that each are a word holding a reference.
        unsafe {
            let new_list = allocation.add(new_header_offset).cast::<AttributeListHeader>();
            let attribute_map = match old_has_prefix {
                true => (*old_list
                    .byte_sub(ATTRIBUTE_LIST_PREFIX_SIZE)
                    .cast::<AttributeListPrefix>())
                .attribute_map
                .get(),
                false => 0,
            };
            if has_prefix {
                allocation.cast::<AttributeListPrefix>().write(AttributeListPrefix {
                    attribute_map: Cell::new(attribute_map),
                    _padding: 0,
                });
            } else {
                assert!(attribute_map == 0, "a list keeps the element's map");
            }
            new_list.write(AttributeListHeader {
                size: UnsafeCell::new(size as u32),
                capacity: UnsafeCell::new(match has_prefix {
                    true => capacity_u32 | ATTRIBUTE_LIST_HAS_ATTRIBUTE_MAP,
                    false => capacity_u32,
                }),
            });
            if !old_list.is_null() {
                core::ptr::copy_nonoverlapping(
                    old_list.add(1).cast::<Attribute>(),
                    new_list.add(1).cast::<Attribute>(),
                    size,
                );
                Self::deallocate(old_list, self.capacity(), old_has_prefix);
            }
            *self.0.get() = new_list;
        }
    }

    /// Frees the allocation of the list whose header is `list`, which has room for `capacity` attributes and a prefix
    /// where `has_prefix` says so, and whose attributes have moved out or been dropped.
    ///
    /// # Safety
    ///
    /// `list` must be a list this field allocated with that capacity and prefix, which is not read again.
    unsafe fn deallocate(list: *mut AttributeListHeader, capacity: usize, has_prefix: bool) {
        let (layout, header_offset) = Self::layout(capacity, has_prefix);
        // SAFETY: Guaranteed by the caller.
        unsafe { std::alloc::dealloc(list.cast::<u8>().sub(header_offset), layout) };
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

    /// Makes room in the list for `count` more attributes than it holds.
    pub(super) fn reserve(&self, count: usize) {
        let needed = self.as_slice().len() + count;
        if count == 0 || needed <= self.capacity() {
            return;
        }
        self.reallocate(needed, self.has_prefix());
    }

    /// Appends an attribute named `name` whose value is `value`, whose references pass to the list.
    ///
    /// # Safety
    ///
    /// Nothing may borrow an attribute of the list.
    #[inline(always)]
    pub(super) unsafe fn append(&self, name: NonNull<QualifiedNameImpl>, value: ak::Utf16String) {
        let size = self.as_slice().len();
        if size == self.capacity() {
            self.grow();
        }
        let list = self.header();
        // SAFETY: Guaranteed by the caller. The list has room for one more attribute, a qualified name and a string,
        //         each a word that holds a reference, which moves into the slot after the last attribute.
        unsafe {
            let attribute = list.add(1).cast::<Attribute>().add(size);
            attribute.cast::<NonNull<QualifiedNameImpl>>().write(name);
            attribute.cast::<usize>().add(1).write(value.into_raw());
            *(*list).size.get() += 1;
        }
    }

    /// Moves the list into one with room for more attributes: one for a list that never had any, and twice as many
    /// as it holds, and at least four, after that.
    #[inline(never)]
    fn grow(&self) {
        let capacity = self.capacity();
        let new_capacity = match self.header().is_null() {
            true => 1,
            false => (capacity * 2).max(4),
        };
        self.reallocate(new_capacity, self.has_prefix());
    }

    /// Takes the attribute at `index` out of the list, moving the attributes after it down by one, and gives back its
    /// name and value, whose references pass to the caller.
    ///
    /// # Safety
    ///
    /// Nothing may borrow an attribute of the list.
    pub(super) unsafe fn take(&self, index: usize) -> (NonNull<QualifiedNameImpl>, ak::Utf16String) {
        let list = self.header();
        assert!(index < self.as_slice().len());
        // SAFETY: Guaranteed by the caller: nothing borrows the list, whose size counts the attributes that follow its
        //         header. An attribute is a qualified name and a string, each a word that holds a reference, which
        //         moves with the word.
        unsafe {
            let size = (*list).size.get();
            let count = *size as usize;
            let attribute = list.add(1).cast::<Attribute>().add(index);
            let name = attribute.cast::<NonNull<QualifiedNameImpl>>().read();
            let value = attribute.cast::<usize>().add(1).read();
            core::ptr::copy(attribute.add(1), attribute, count - index - 1);
            *size -= 1;
            (name, ak::Utf16String::from_raw_owned(value))
        }
    }

    /// Replaces the attribute at `index` with one named `name` whose value is `value`, whose references pass to the
    /// list, and gives back the name and value of the attribute it replaces, whose references pass to the caller.
    ///
    /// # Safety
    ///
    /// Nothing may borrow an attribute of the list.
    pub(super) unsafe fn replace(
        &self,
        index: usize,
        name: NonNull<QualifiedNameImpl>,
        value: ak::Utf16String,
    ) -> (NonNull<QualifiedNameImpl>, ak::Utf16String) {
        let list = self.header();
        assert!(index < self.as_slice().len());
        // SAFETY: As for take(), and the attribute at `index` takes over the references of `name` and `value`.
        unsafe {
            let attribute = list.add(1).cast::<Attribute>().add(index);
            let old_name = attribute.cast::<NonNull<QualifiedNameImpl>>().replace(name);
            let old_value = attribute.cast::<usize>().add(1).replace(value.into_raw());
            (old_name, ak::Utf16String::from_raw_owned(old_value))
        }
    }

    /// The element's NamedNodeMap, laid out as a GC::Member, where the list keeps one, see AttributeListPrefix.
    #[inline(always)]
    pub(super) fn attribute_map(&self) -> Option<&Cell<u32>> {
        if !self.has_prefix() {
            return None;
        }
        // SAFETY: A list with a prefix keeps it just before its header, in its allocation.
        Some(unsafe {
            &(*self
                .header()
                .byte_sub(ATTRIBUTE_LIST_PREFIX_SIZE)
                .cast::<AttributeListPrefix>())
            .attribute_map
        })
    }

    /// Gives the list a field for the element's NamedNodeMap, see AttributeListPrefix, and gives it back.
    pub(super) fn ensure_attribute_map(&self) -> &Cell<u32> {
        if !self.has_prefix() {
            self.reallocate(self.capacity(), true);
        }
        self.attribute_map().expect("the list has a prefix")
    }

    /// Drops every attribute and frees the list, which leaves the field null.
    ///
    /// # Safety
    ///
    /// Nothing may borrow an attribute of the list.
    unsafe fn destroy(&self, main_thread: &mut MainThread<'_>) {
        let list = self.header();
        if list.is_null() {
            return;
        }
        let (capacity, has_prefix) = (self.capacity(), self.has_prefix());
        // SAFETY: Guaranteed by the caller. The list holds a reference to the name and value of each of its attributes,
        //         and is not read again once the field is null.
        unsafe {
            let size = *(*list).size.get() as usize;
            let attributes = list.add(1).cast::<Attribute>();
            for index in 0..size {
                let attribute = attributes.add(index);
                let name = attribute.cast::<NonNull<QualifiedNameImpl>>().read();
                drop(ak::Utf16String::from_raw_owned(attribute.cast::<usize>().add(1).read()));
                qualified_name::release(main_thread, name);
            }
            *self.0.get() = core::ptr::null_mut();
            Self::deallocate(list, capacity, has_prefix);
        }
    }
}

/// Drops the attributes of an element's attribute list and frees it, for DOM::AttributeList as the element is
/// destroyed.
///
/// # Safety
///
/// `list` must be the attribute list of an element that is being destroyed, and the call must be made on the main
/// thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_attribute_list_destroy(list: &AttributeListField) {
    // SAFETY: Guaranteed by the caller.
    let mut main_thread = unsafe { main_thread() };
    // SAFETY: Nothing borrows the attributes of an element that is being destroyed.
    unsafe { list.destroy(&mut main_thread) }
}

const _: () = assert!(size_of::<AttributeListField>() == 8 && align_of::<AttributeListField>() == 8);

// The flags of an element's core, a bit each in its flags byte, which C++ sets where the state they stand for changes.

/// Whether the element is a form-associated custom element, which has the insertion steps, removing steps, moving
/// steps and attribute change steps of a form-associated element. C++ keeps it as the element's custom element
/// definition changes.
/// https://html.spec.whatwg.org/multipage/custom-elements.html#form-associated-custom-element
pub const ELEMENT_IS_FORM_ASSOCIATED_CUSTOM: u8 = 1 << 1;
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
            attributes: AttributeListField(UnsafeCell::new(core::ptr::null_mut())),
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
                capacity: UnsafeCell::new(count as u32),
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
