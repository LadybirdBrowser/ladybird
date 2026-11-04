/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_string::CssString;
use crate::css::ffi_support::{FfiUtf16View, OwnedBox};
use crate::css::property_metadata::{
    longhands_for_shorthand, property_is_logical_alias_including_shorthands, property_is_shorthand,
    property_logical_group, shorthand_has_leaf_longhand,
};
use crate::css::style::cascade::CascadeOperator;
use crate::css::style::pipeline::rule_store::BlockCascadeData;
use crate::css::style_compute::expand_shorthands_with;
use crate::css::style_value::ContentPayload;
use crate::css::style_value::{RetainedStyleValueData, StyleValueData};
use crate::css::style_value_arc::StyleValueArc;
use std::alloc::{Layout, alloc, dealloc, handle_alloc_error};
use std::cell::RefCell;
use std::ffi::c_void;
use std::ptr::NonNull;
use std::rc::Rc;
use std::sync::atomic::{AtomicPtr, AtomicU32, AtomicU64, Ordering, fence};

#[derive(Clone)]
pub(crate) struct DeclaredProperty {
    pub(crate) property_id: u16,
    pub(crate) important: bool,
    pub(crate) value: StyleValueArc,
}

impl DeclaredProperty {
    pub(crate) fn borrowed(&self) -> DeclarationRef<'_> {
        DeclarationRef {
            property_id: self.property_id,
            important: self.important,
            operator: declaration_operator(&self.value),
            value: &self.value,
        }
    }
}

/// How a declaration of `value` takes part in the cascade: as a value, or as the CSS-wide keyword
/// it is.
pub(crate) fn declaration_operator(value: &StyleValueData) -> CascadeOperator {
    use crate::css::style_compute::keyword;
    match value {
        StyleValueData::Keyword {
            keyword: keyword::INHERIT,
        } => CascadeOperator::Inherit,
        StyleValueData::Keyword {
            keyword: keyword::INITIAL,
        } => CascadeOperator::Initial,
        StyleValueData::Keyword {
            keyword: keyword::UNSET,
        } => CascadeOperator::Unset,
        StyleValueData::Keyword {
            keyword: keyword::REVERT,
        } => CascadeOperator::Revert,
        StyleValueData::Keyword {
            keyword: keyword::REVERT_LAYER,
        } => CascadeOperator::RevertLayer,
        _ => CascadeOperator::Declared,
    }
}

#[derive(Clone)]
pub(crate) struct CustomProperty {
    pub(crate) name: CssString,
    pub(crate) declaration: DeclaredProperty,
}

/// A declaration's property id in bits 0-10, whether it is important in bit 11, and its cascade
/// operator in bits 12-14.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[repr(transparent)]
pub(crate) struct DeclarationEntry(u16);

const _: () = assert!(size_of::<DeclarationEntry>() == 2);
const _: () = assert!(crate::css::property_metadata::LAST_LONGHAND_PROPERTY_ID < DeclarationEntry::IMPORTANT);
const _: () = assert!(crate::css::property_metadata::property_id::CUSTOM < DeclarationEntry::IMPORTANT);
const _: () = assert!((CascadeOperator::LAST as u16) < (1 << (16 - DeclarationEntry::OPERATOR_SHIFT)));

impl DeclarationEntry {
    const IMPORTANT: u16 = 1 << 11;
    const OPERATOR_SHIFT: u16 = 12;

    pub(crate) fn new(property_id: u16, important: bool, operator: CascadeOperator) -> Self {
        assert!(property_id < Self::IMPORTANT, "property id out of range");
        Self(property_id | if important { Self::IMPORTANT } else { 0 } | ((operator as u16) << Self::OPERATOR_SHIFT))
    }

    /// The property the declaration declares.
    pub(crate) fn property(self) -> u16 {
        self.0 & (Self::IMPORTANT - 1)
    }

    pub(crate) fn important(self) -> bool {
        self.0 & Self::IMPORTANT != 0
    }

    /// How the declaration takes part in the cascade.
    pub(crate) fn operator(self) -> CascadeOperator {
        CascadeOperator::from_bits((self.0 >> Self::OPERATOR_SHIFT) as u8)
    }

    fn declaration(self, value: &StyleValueArc) -> DeclarationRef<'_> {
        DeclarationRef {
            property_id: self.property(),
            important: self.important(),
            operator: self.operator(),
            value,
        }
    }
}

/// A declaration of a block, borrowed from the arrays it is kept in.
#[derive(Clone, Copy)]
pub(crate) struct DeclarationRef<'a> {
    pub(crate) property_id: u16,
    pub(crate) important: bool,
    pub(crate) operator: CascadeOperator,
    pub(crate) value: &'a StyleValueArc,
}

impl DeclarationRef<'_> {
    pub(crate) fn to_declared(self) -> DeclaredProperty {
        DeclaredProperty {
            property_id: self.property_id,
            important: self.important,
            value: self.value.clone(),
        }
    }

    fn view(self, name: FfiUtf16View<'_>) -> FfiDeclaredProperty<'_> {
        FfiDeclaredProperty {
            property_id: self.property_id,
            important: self.important,
            value: StyleValueArc::as_ptr(self.value).cast(),
            name,
        }
    }
}

pub(crate) type DeclaredPropertiesIter<'a> = std::iter::Map<
    std::iter::Zip<std::slice::Iter<'a, DeclarationEntry>, std::slice::Iter<'a, StyleValueArc>>,
    fn((&'a DeclarationEntry, &'a StyleValueArc)) -> DeclarationRef<'a>,
>;

/// The properties of a block: their values in one contiguous array, and their ids and
/// importance in another.
#[derive(Clone, Copy, Default)]
pub(crate) struct DeclaredProperties<'a> {
    values: &'a [StyleValueArc],
    entries: &'a [DeclarationEntry],
}

impl<'a> DeclaredProperties<'a> {
    #[cfg(test)]
    pub(crate) fn len(self) -> usize {
        self.values.len()
    }

    #[cfg(test)]
    pub(crate) fn is_empty(self) -> bool {
        self.values.is_empty()
    }

    #[cfg(test)]
    pub(crate) fn get(self, index: usize) -> Option<DeclarationRef<'a>> {
        Some(self.entries.get(index)?.declaration(&self.values[index]))
    }

    pub(crate) fn iter(self) -> DeclaredPropertiesIter<'a> {
        fn declaration<'a>((entry, value): (&'a DeclarationEntry, &'a StyleValueArc)) -> DeclarationRef<'a> {
            entry.declaration(value)
        }
        self.entries.iter().zip(self.values).map(declaration)
    }
}

impl<'a> IntoIterator for DeclaredProperties<'a> {
    type Item = DeclarationRef<'a>;
    type IntoIter = DeclaredPropertiesIter<'a>;

    fn into_iter(self) -> Self::IntoIter {
        self.iter()
    }
}

impl<'a> DeclaredProperties<'a> {
    /// The entries of the properties, in their order.
    pub(crate) fn entries(self) -> &'a [DeclarationEntry] {
        self.entries
    }

    /// The values of the properties, in their order.
    pub(crate) fn values(self) -> &'a [StyleValueArc] {
        self.values
    }
}

/// A custom property of a block, borrowed from the arrays it is kept in.
#[derive(Clone, Copy)]
pub(crate) struct CustomPropertyRef<'a> {
    pub(crate) name: &'a CssString,
    pub(crate) declaration: DeclarationRef<'a>,
}

/// The custom properties of a block: their values, names and entries, each in an array of its
/// own.
#[derive(Clone, Copy, Default)]
pub(crate) struct CustomProperties<'a> {
    values: &'a [StyleValueArc],
    names: &'a [CssString],
    entries: &'a [DeclarationEntry],
}

impl<'a> CustomProperties<'a> {
    pub(crate) fn len(self) -> usize {
        self.names.len()
    }

    pub(crate) fn is_empty(self) -> bool {
        self.names.is_empty()
    }

    /// The values of the custom properties, in their order.
    pub(crate) fn values(self) -> &'a [StyleValueArc] {
        self.values
    }

    #[cfg(test)]
    pub(crate) fn get(self, index: usize) -> Option<CustomPropertyRef<'a>> {
        Some(CustomPropertyRef {
            name: self.names.get(index)?,
            declaration: self.entries[index].declaration(&self.values[index]),
        })
    }

    pub(crate) fn iter(self) -> impl ExactSizeIterator<Item = CustomPropertyRef<'a>> + Clone + 'a {
        (0..self.len()).map(move |index| CustomPropertyRef {
            name: &self.names[index],
            declaration: self.entries[index].declaration(&self.values[index]),
        })
    }
}

/// The start of a declaration block's one allocation: its reference count and the lengths of its
/// arrays. The values of its properties follow it, then those of its custom properties, then the
/// names of its custom properties, then the entries of both, last so that the arrays of pointers
/// need no padding: 16 + 10 bytes per property and 18 per custom property. C++ holds a block by a
/// pointer at this header, as an opaque type.
pub struct DeclarationBlockHeader {
    references: AtomicU32,
    /// Whether any declaration of the block can define a transition.
    declares_transitions: bool,
    /// Whether any value of the block reads an attribute.
    reads_attributes: bool,
    property_count: u32,
    custom_property_count: u32,
    /// What the style engine derives from the block, made by the first engine that publishes it,
    /// and null until then. It depends on the block's content alone.
    cascade: AtomicPtr<BlockCascadeData>,
}

const _: () = assert!(size_of::<DeclarationBlockHeader>() == 24);

/// Where the arrays of a block's allocation start.
struct BlockLayout {
    layout: Layout,
    values: usize,
    names: usize,
    entries: usize,
}

fn block_layout(property_count: usize, custom_property_count: usize) -> BlockLayout {
    let too_large = "declaration block too large";
    let declaration_count = property_count.checked_add(custom_property_count).expect(too_large);
    let (layout, values) = Layout::new::<DeclarationBlockHeader>()
        .extend(Layout::array::<StyleValueArc>(declaration_count).expect(too_large))
        .expect(too_large);
    let (layout, names) = layout
        .extend(Layout::array::<CssString>(custom_property_count).expect(too_large))
        .expect(too_large);
    let (layout, entries) = layout
        .extend(Layout::array::<DeclarationEntry>(declaration_count).expect(too_large))
        .expect(too_large);
    BlockLayout {
        layout,
        values,
        names,
        entries,
    }
}

impl DeclarationBlockHeader {
    fn layout(&self) -> BlockLayout {
        block_layout(self.property_count as usize, self.custom_property_count as usize)
    }

    fn array<T>(&self, offset: usize, start: u32, length: u32) -> &[T] {
        // SAFETY: A header is only reached through a block, whose allocation holds the arrays
        //         after it at the offsets its layout gives.
        unsafe {
            ak::slice_from_raw_parts(
                std::ptr::from_ref(self)
                    .cast::<u8>()
                    .add(offset)
                    .cast::<T>()
                    .add(start as usize),
                length as usize,
            )
        }
    }

    pub(crate) fn properties(&self) -> DeclaredProperties<'_> {
        let layout = self.layout();
        DeclaredProperties {
            values: self.array(layout.values, 0, self.property_count),
            entries: self.array(layout.entries, 0, self.property_count),
        }
    }

    pub(crate) fn custom_properties(&self) -> CustomProperties<'_> {
        let layout = self.layout();
        CustomProperties {
            values: self.array(layout.values, self.property_count, self.custom_property_count),
            names: self.array(layout.names, 0, self.custom_property_count),
            entries: self.array(layout.entries, self.property_count, self.custom_property_count),
        }
    }

    pub(crate) fn is_empty(&self) -> bool {
        self.property_count == 0 && self.custom_property_count == 0
    }

    /// What the style engine derives from the block, once an engine published it.
    pub(crate) fn cascade_data(&self) -> Option<&BlockCascadeData> {
        // SAFETY: The data, once made, lives as long as the block.
        unsafe { self.cascade.load(Ordering::Acquire).as_ref() }
    }

    /// What the style engine derives from the block, which `make` makes where no engine did yet,
    /// and whether it did.
    pub(crate) fn make_cascade_data(&self, make: impl FnOnce(&Self) -> NonNull<BlockCascadeData>) -> bool {
        if self.cascade_data().is_some() {
            return false;
        }
        let made = make(self);
        match self
            .cascade
            .compare_exchange(std::ptr::null_mut(), made.as_ptr(), Ordering::AcqRel, Ordering::Acquire)
        {
            Ok(_) => true,
            Err(_) => {
                // SAFETY: Another engine made the block's data first, and this copy is no one's.
                unsafe { BlockCascadeData::free(made) };
                false
            }
        }
    }

    /// Ask the processor to fetch the block's arrays, reading its header.
    pub(crate) fn prefetch_arrays(&self) {
        let layout = self.layout();
        let base = std::ptr::from_ref(self).cast::<u8>();
        prefetch(base.wrapping_add(layout.values));
        prefetch(base.wrapping_add(layout.entries));
        prefetch(self.cascade.load(Ordering::Relaxed).cast_const());
    }

    /// The bytes of the block's allocation and of the names of its custom properties.
    pub(crate) fn heap_bytes(&self) -> usize {
        let mut size = self.layout().layout.size();
        for property in self.custom_properties().iter() {
            size = size.saturating_add(size_of_val(property.name.units()));
        }
        size
    }

    pub(crate) fn to_builder(&self) -> DeclarationBlockBuilder {
        DeclarationBlockBuilder {
            properties: self.properties().iter().map(DeclarationRef::to_declared).collect(),
            custom_properties: self
                .custom_properties()
                .iter()
                .map(|property| CustomProperty {
                    name: property.name.clone(),
                    declaration: property.declaration.to_declared(),
                })
                .collect(),
        }
    }
}

/// An immutable declaration block shared by the parse, the CSSOM and the style engine: one counted
/// allocation holding its header and both arrays of declarations.
#[repr(transparent)]
pub struct SharedDeclarationBlock {
    header: NonNull<DeclarationBlockHeader>,
}

const _: () = assert!(size_of::<SharedDeclarationBlock>() == 8);
const _: () = assert!(size_of::<Option<SharedDeclarationBlock>>() == 8);

// SAFETY: The block is immutable while shared, its count is atomic, and its declarations are
//         Send and Sync.
unsafe impl Send for SharedDeclarationBlock {}
unsafe impl Sync for SharedDeclarationBlock {}

const _: () = {
    const fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<DeclaredProperty>();
    assert_send_sync::<CustomProperty>();
};

/// The most references a block may have, beyond which counting aborts.
const MAXIMUM_BLOCK_REFERENCES: u32 = i32::MAX as u32;

/// Ask the processor to fetch the cache line at `address` without waiting for it.
#[inline]
pub(crate) fn prefetch<T>(address: *const T) {
    #[cfg(target_arch = "x86_64")]
    // SAFETY: A prefetch reads nothing and cannot fault, whatever the address.
    unsafe {
        std::arch::x86_64::_mm_prefetch::<{ std::arch::x86_64::_MM_HINT_T0 }>(address.cast());
    }
    #[cfg(not(target_arch = "x86_64"))]
    let _ = address;
}

impl SharedDeclarationBlock {
    pub(crate) fn as_ptr(&self) -> *const DeclarationBlockHeader {
        self.header.as_ptr()
    }

    #[cfg(test)]
    pub(crate) fn ptr_eq(first: &Self, second: &Self) -> bool {
        first.header == second.header
    }

    /// Hands the reference over as a pointer at the header, which `from_raw` takes back.
    pub(crate) fn into_raw(this: Self) -> *const DeclarationBlockHeader {
        let pointer = this.header.as_ptr();
        std::mem::forget(this);
        pointer
    }

    /// Takes back a reference `into_raw` handed over.
    ///
    /// # Safety
    /// `pointer` must come from `into_raw`, or carry a reference `increment_count` added.
    pub(crate) unsafe fn from_raw(pointer: *const DeclarationBlockHeader) -> Self {
        assert!(!pointer.is_null(), "a declaration block pointer is never null");
        Self {
            // SAFETY: Checked non-null above.
            header: unsafe { NonNull::new_unchecked(pointer.cast_mut()) },
        }
    }

    /// Adds a reference to the block `pointer` points at.
    ///
    /// # Safety
    /// `pointer` must point at the header of a live block.
    pub(crate) unsafe fn increment_count(pointer: *const DeclarationBlockHeader) {
        let previous = unsafe { &*pointer }.references.fetch_add(1, Ordering::Relaxed);
        if previous > MAXIMUM_BLOCK_REFERENCES {
            std::process::abort();
        }
    }

    /// A reference of its own to the live block `pointer` points at.
    ///
    /// # Safety
    /// `pointer` must point at the header of a live block.
    pub(crate) unsafe fn retain(pointer: *const DeclarationBlockHeader) -> Self {
        unsafe {
            Self::increment_count(pointer);
            Self::from_raw(pointer)
        }
    }

    /// Whether this is the only reference to the block.
    pub(crate) fn is_unique(&self) -> bool {
        self.references.load(Ordering::Acquire) == 1
    }

    #[cfg(test)]
    pub(crate) fn reference_count(&self) -> usize {
        self.references.load(Ordering::Relaxed) as usize
    }

    /// Whether any declaration of the block can define a transition.
    pub(crate) fn declares_transitions(&self) -> bool {
        self.declares_transitions
    }

    /// Whether any value of the block reads an attribute.
    pub(crate) fn reads_attributes(&self) -> bool {
        self.reads_attributes
    }
}

impl std::ops::Deref for SharedDeclarationBlock {
    type Target = DeclarationBlockHeader;

    fn deref(&self) -> &DeclarationBlockHeader {
        // SAFETY: The reference keeps the block alive.
        unsafe { self.header.as_ref() }
    }
}

impl Clone for SharedDeclarationBlock {
    fn clone(&self) -> Self {
        // SAFETY: This reference keeps the block alive.
        unsafe { Self::increment_count(self.as_ptr()) };
        Self { header: self.header }
    }
}

impl Drop for SharedDeclarationBlock {
    fn drop(&mut self) {
        if self.references.fetch_sub(1, Ordering::Release) != 1 {
            return;
        }
        fence(Ordering::Acquire);
        if let Some(cascade) = NonNull::new(self.cascade.load(Ordering::Acquire)) {
            // SAFETY: The block held the data, which it frees once.
            unsafe { BlockCascadeData::free(cascade) };
        }
        let layout = self.layout();
        let base = self.header.as_ptr().cast::<u8>();
        // SAFETY: The last reference owns the declarations and the allocation `finish` made.
        unsafe {
            std::ptr::drop_in_place(std::ptr::slice_from_raw_parts_mut(
                base.add(layout.values).cast::<StyleValueArc>(),
                self.property_count as usize + self.custom_property_count as usize,
            ));
            std::ptr::drop_in_place(std::ptr::slice_from_raw_parts_mut(
                base.add(layout.names).cast::<CssString>(),
                self.custom_property_count as usize,
            ));
            dealloc(base, layout.layout);
        }
    }
}

impl Default for SharedDeclarationBlock {
    fn default() -> Self {
        DeclarationBlockBuilder::default().finish()
    }
}

impl std::fmt::Debug for SharedDeclarationBlock {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str("SharedDeclarationBlock")
    }
}

/// Whether `value` is a pending-substitution value naming the shorthand `declaration` declares, as
/// written there, through any shorthand nested in it.
fn value_pends_shorthand(value: &StyleValueData, declaration: &DeclaredProperty) -> bool {
    let StyleValueData::PendingSubstitution {
        original_shorthand,
        original_shorthand_value,
    } = value
    else {
        return false;
    };
    (*original_shorthand == declaration.property_id && original_shorthand_value.data() == &*declaration.value)
        || value_pends_shorthand(original_shorthand_value.data(), declaration)
}

/// What a block is built from and edited as, frozen into a `SharedDeclarationBlock`.
#[derive(Clone, Default)]
pub(crate) struct DeclarationBlockBuilder {
    pub(crate) properties: Vec<DeclaredProperty>,
    pub(crate) custom_properties: Vec<CustomProperty>,
}

impl DeclarationBlockBuilder {
    pub(crate) fn finish(self) -> SharedDeclarationBlock {
        let property_count = u32::try_from(self.properties.len()).expect("declaration block too large");
        let custom_property_count = u32::try_from(self.custom_properties.len()).expect("declaration block too large");
        let layout = block_layout(self.properties.len(), self.custom_properties.len());
        // SAFETY: The layout is never zero-sized, and the header and each array element are
        //         written once.
        unsafe {
            let base = alloc(layout.layout);
            if base.is_null() {
                handle_alloc_error(layout.layout);
            }
            let declares_transitions = self
                .properties
                .iter()
                .any(|property| crate::css::property_metadata::property_defines_a_css_transition(property.property_id));
            let reads_attributes = self
                .properties
                .iter()
                .map(|property| &property.value)
                .chain(
                    self.custom_properties
                        .iter()
                        .map(|property| &property.declaration.value),
                )
                .any(|value| crate::css::style::value_reads_attributes(value));
            base.cast::<DeclarationBlockHeader>().write(DeclarationBlockHeader {
                references: AtomicU32::new(1),
                declares_transitions,
                reads_attributes,
                property_count,
                custom_property_count,
                cascade: AtomicPtr::new(std::ptr::null_mut()),
            });
            let values = base.add(layout.values).cast::<StyleValueArc>();
            let names = base.add(layout.names).cast::<CssString>();
            let entries = base.add(layout.entries).cast::<DeclarationEntry>();
            let custom_properties = self
                .custom_properties
                .into_iter()
                .map(|property| (Some(property.name), property.declaration));
            let declarations = self
                .properties
                .into_iter()
                .map(|property| (None, property))
                .chain(custom_properties);
            for (index, (name, declaration)) in declarations.enumerate() {
                let operator = declaration_operator(&declaration.value);
                entries.add(index).write(DeclarationEntry::new(
                    declaration.property_id,
                    declaration.important,
                    operator,
                ));
                values.add(index).write(declaration.value);
                if let Some(name) = name {
                    names.add(index - property_count as usize).write(name);
                }
            }
            SharedDeclarationBlock {
                header: NonNull::new_unchecked(base.cast()),
            }
        }
    }

    // https://drafts.csswg.org/cssom/#concept-declarations-specified-order
    pub(crate) fn append_in_specified_order(&mut self, declaration: DeclaredProperty) {
        // The specified order for declarations is the same as specified, but with shorthand properties expanded into their
        // longhand properties, in canonical order. If a property is specified more than once (after shorthand expansion), only
        // the one with greatest cascading order must be represented, at the same relative position as it was specified.
        expand_shorthands_with(
            declaration.property_id,
            &crate::css::style_value::RetainedStyleValueData::from_arc(declaration.value),
            false,
            &mut |property_id, value, _| {
                self.append_normalized_property(DeclaredProperty {
                    property_id,
                    important: declaration.important,
                    value: value.clone().into_arc(),
                });
            },
        );
    }

    fn append_normalized_property(&mut self, declaration: DeclaredProperty) {
        let property_id = declaration.property_id;
        if let Some(index) = self
            .properties
            .iter()
            .position(|property| property.property_id == property_id)
        {
            if self.properties[index].important && !declaration.important {
                self.drop_shorthands_no_longer_pending_in(property_id);
                return;
            }
            self.properties.remove(index);
        }
        self.properties.push(declaration);
        self.drop_shorthands_no_longer_pending_in(property_id);
    }

    /// AD-HOC: The spec keeps only longhands in a block, where we also keep a shorthand written with an arbitrary
    ///         substitution function, for the longhands pending it to name. That declaration stands for the shorthand
    ///         only while each of its longhands pends it, so a change to `longhand` that ends this drops it.
    fn drop_shorthands_no_longer_pending_in(&mut self, longhand: u16) {
        if property_is_shorthand(longhand)
            || !self
                .properties
                .iter()
                .any(|property| property_is_shorthand(property.property_id))
        {
            return;
        }
        let value = self
            .properties
            .iter()
            .find(|property| property.property_id == longhand)
            .map(|property| property.value.clone());
        self.properties.retain(|property| {
            !property_is_shorthand(property.property_id)
                || !shorthand_has_leaf_longhand(property.property_id, longhand)
                || value
                    .as_deref()
                    .is_some_and(|value| value_pends_shorthand(value, property))
        });
    }

    pub(crate) fn append_block(&mut self, other: &DeclarationBlockHeader) {
        for declaration in other.properties() {
            self.append_normalized_property(declaration.to_declared());
        }
        for property in other.custom_properties().iter() {
            self.append_custom_property(property.name.clone(), property.declaration.to_declared());
        }
    }

    pub(crate) fn append_custom_property(&mut self, name: CssString, declaration: DeclaredProperty) {
        if let Some(property) = self.custom_properties.iter_mut().find(|property| property.name == name) {
            if property.declaration.important && !declaration.important {
                return;
            }
            property.declaration = declaration;
        } else {
            self.custom_properties.push(CustomProperty { name, declaration });
        }
    }

    pub(crate) fn set_custom_property(&mut self, name: CssString, declaration: DeclaredProperty) {
        if let Some(property) = self.custom_properties.iter_mut().find(|property| property.name == name) {
            property.declaration = declaration;
        } else {
            self.custom_properties.push(CustomProperty { name, declaration });
        }
    }

    // https://drafts.csswg.org/cssom/#set-a-css-declaration
    fn set_declaration(&mut self, declaration: DeclaredProperty) -> bool {
        // NB: This follows the suggested algorithm at https://drafts.csswg.org/cssom/#example-a40690cb.
        // 1. If property is a case-sensitive match for a property name of a CSS declaration in declarations, follow these substeps:
        if let Some(index) = self
            .properties
            .iter()
            .position(|property| property.property_id == declaration.property_id)
        {
            // 1. Let target declaration be such CSS declaration.
            let target_declaration = &self.properties[index];
            // 2. Let needs append be false.
            let mut needs_append = false;
            if let Some(group) = property_logical_group(declaration.property_id) {
                let is_logical = property_is_logical_alias_including_shorthands(declaration.property_id);
                // 3. For each declaration in declarations after target declaration:
                for property in &self.properties[index + 1..] {
                    // 1. If declaration’s property name is not in the same logical property group as property, then continue.
                    if property_logical_group(property.property_id) != Some(group) {
                        continue;
                    }
                    // 2. If declaration’ property name has the same mapping logic as property, then continue.
                    if property_is_logical_alias_including_shorthands(property.property_id) == is_logical {
                        continue;
                    }
                    // 3. Let needs append be true.
                    needs_append = true;
                    // 4. Break.
                    break;
                }
            }
            // 4. If needs append is false, then:
            if !needs_append {
                // 1. Let needs update be false.
                // 2. If target declaration’s value is not equal to component value list, then let needs update be true.
                // 3. If target declaration’s important flag is not equal to whether important flag is set, then let needs update be true.
                let needs_update = target_declaration.value != declaration.value
                    || target_declaration.important != declaration.important;
                // 4. If needs update is false, then return false.
                if !needs_update {
                    return false;
                }
                // 5. Set target declaration’s value to component value list.
                // 6. If important flag is set, then set target declaration’s important flag, otherwise unset it.
                self.properties[index] = declaration;
                // 7. Return true.
                return true;
            }
            // 5. Otherwise, remove target declaration from declarations.
            self.properties.remove(index);
        }
        // 2. Append a new CSS declaration with property name property, value component value list, and important flag set
        //    if important flag is set to declarations.
        // AD-HOC: A shorthand written with an arbitrary substitution function comes before the longhands pending it,
        //         where it serializes for them.
        let position = self
            .properties
            .iter()
            .position(|property| shorthand_has_leaf_longhand(declaration.property_id, property.property_id))
            .unwrap_or(self.properties.len());
        self.properties.insert(position, declaration);
        // 3. Return true
        true
    }
}

#[repr(C)]
pub struct FfiDeclaredProperty<'a> {
    pub property_id: u16,
    pub important: bool,
    pub value: *const c_void,
    pub name: FfiUtf16View<'a>,
}

struct DeclarationBlockOwner {
    data: SharedDeclarationBlock,
    identity: u64,
    revision: u64,
}

static NEXT_DECLARATION_BLOCK_IDENTITY: AtomicU64 = AtomicU64::new(1);

#[cfg(test)]
thread_local! {
    pub(crate) static DECLARATION_OWNER_ALLOCATIONS: std::cell::Cell<u64> = const { std::cell::Cell::new(0) };
}

enum DeclarationBlockState {
    Immutable(SharedDeclarationBlock),
    Mutable(Rc<RefCell<DeclarationBlockOwner>>),
}

// Native reads use immutable data without an owner allocation. Retaining a live handle
// or editing a block promotes its document-thread owner.
// Shared blocks start independently from the current immutable data.
pub struct DeclarationBlock {
    state: RefCell<DeclarationBlockState>,
}

impl Clone for DeclarationBlock {
    fn clone(&self) -> Self {
        Self {
            state: RefCell::new(DeclarationBlockState::Mutable(self.owner())),
        }
    }
}

impl DeclarationBlock {
    pub(crate) fn new(data: SharedDeclarationBlock) -> Self {
        Self {
            state: RefCell::new(DeclarationBlockState::Immutable(data)),
        }
    }

    fn owner(&self) -> Rc<RefCell<DeclarationBlockOwner>> {
        let mut state = self.state.borrow_mut();
        if let DeclarationBlockState::Immutable(data) = &*state {
            #[cfg(test)]
            DECLARATION_OWNER_ALLOCATIONS.with(|count| count.set(count.get().checked_add(1).unwrap()));
            *state = DeclarationBlockState::Mutable(Rc::new(RefCell::new(DeclarationBlockOwner {
                data: data.clone(),
                identity: NEXT_DECLARATION_BLOCK_IDENTITY
                    .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |value| value.checked_add(1))
                    .expect("declaration block identity overflow"),
                revision: 0,
            })));
        }
        let DeclarationBlockState::Mutable(owner) = &*state else {
            unreachable!()
        };
        owner.clone()
    }

    pub(crate) fn data(&self) -> SharedDeclarationBlock {
        match &*self.state.borrow() {
            DeclarationBlockState::Immutable(data) => data.clone(),
            DeclarationBlockState::Mutable(owner) => owner.borrow().data.clone(),
        }
    }

    pub(crate) fn is_empty(&self) -> bool {
        match &*self.state.borrow() {
            DeclarationBlockState::Immutable(data) => data.is_empty(),
            DeclarationBlockState::Mutable(owner) => owner.borrow().data.is_empty(),
        }
    }

    /// Asks for the memory [`Self::is_empty`] reads past the block.
    fn prefetch_header(&self) {
        match &*self.state.borrow() {
            DeclarationBlockState::Immutable(data) => prefetch(data.as_ptr()),
            DeclarationBlockState::Mutable(owner) => prefetch(owner.borrow().data.as_ptr()),
        }
    }

    #[cfg(test)]
    pub(crate) fn is_immutable(&self) -> bool {
        matches!(&*self.state.borrow(), DeclarationBlockState::Immutable(_))
    }

    pub(crate) fn identity(&self) -> u64 {
        self.owner().borrow().identity
    }

    pub(crate) fn replace(&mut self, source: &Self) {
        let data = source.data();
        let owner = self.owner();
        let mut owner = owner.borrow_mut();
        owner.data = data;
        owner.revision = owner
            .revision
            .checked_add(1)
            .expect("declaration block revision overflow");
    }

    pub(crate) fn remove(&mut self, property_id: u16) -> bool {
        let Some(index) = self
            .data()
            .properties()
            .iter()
            .position(|property| property.property_id == property_id)
        else {
            return false;
        };
        self.mutate(|data| {
            data.properties.remove(index);
            data.drop_shorthands_no_longer_pending_in(property_id);
            true
        })
    }

    fn mutate(&mut self, mutation: impl FnOnce(&mut DeclarationBlockBuilder) -> bool) -> bool {
        let owner = self.owner();
        let mut owner = owner.borrow_mut();
        let mut builder = owner.data.to_builder();
        if !mutation(&mut builder) {
            return false;
        }
        owner.data = builder.finish();
        owner.revision = owner
            .revision
            .checked_add(1)
            .expect("declaration block revision overflow");
        true
    }
}

// Resource registration belongs to stylesheet attachment on the document thread,
// not parsing. Keep the traversal native and expose only image-bearing values.
pub(crate) fn visit_declaration_images(data: &DeclarationBlockHeader, visit: &mut impl FnMut(&StyleValueData)) {
    fn visit_images(value: &StyleValueData, visit: &mut impl FnMut(&StyleValueData)) {
        match value {
            // Leave image-set type filtering to the document's image decoder support.
            StyleValueData::Image { .. } | StyleValueData::ImageSet { .. } => visit(value),
            StyleValueData::ValueList { .. } | StyleValueData::Shorthand(_) => {
                for value in value.child_values().unwrap().as_slice() {
                    visit_images(value.data(), visit);
                }
            }
            StyleValueData::Content(content_payload) => {
                let ContentPayload { content, alt_text } = &**content_payload;
                visit_images(content.data(), visit);
                if let Some(alt_text) = alt_text.optional_data() {
                    visit_images(alt_text, visit);
                }
            }
            _ => {}
        }
    }
    for value in data.properties().values() {
        visit_images(value, visit);
    }
}

/// The block serialized, as a raw AK::Utf16String the caller adopts.
#[unsafe(no_mangle)]
pub extern "C" fn rust_declaration_block_serialize(block: &DeclarationBlock) -> usize {
    crate::css::serialize::sink_into_raw(crate::css::serialize::serialize_a_css_declaration_block(&block.data()))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_declaration_block_prefetch_header(block: &DeclarationBlock) {
    block.prefetch_header();
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_declaration_block_is_empty(block: &DeclarationBlock) -> bool {
    block.is_empty()
}

/// Whether a declaration of the block holds an image or a gradient, as its value or as an item of its list.
#[unsafe(no_mangle)]
pub extern "C" fn rust_declaration_block_has_abstract_image_values(block: &DeclarationBlock) -> bool {
    use crate::css::style_value::StyleValueData;
    let is_abstract_image = |value: &StyleValueData| {
        matches!(
            value,
            StyleValueData::Image { .. }
                | StyleValueData::ImageSet { .. }
                | StyleValueData::LinearGradient { .. }
                | StyleValueData::ConicGradient { .. }
                | StyleValueData::RadialGradient { .. }
        )
    };
    block.data().properties().values().iter().any(|value| match &**value {
        StyleValueData::ValueList { values, .. } => values
            .as_slice()
            .iter()
            .any(|item| item.optional_data().is_some_and(is_abstract_image)),
        value => is_abstract_image(value),
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_declaration_block_identity(block: &DeclarationBlock) -> u64 {
    block.identity()
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_declaration_block_revision(block: &DeclarationBlock) -> u64 {
    match &*block.state.borrow() {
        DeclarationBlockState::Immutable(_) => 0,
        DeclarationBlockState::Mutable(owner) => owner.borrow().revision,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_declaration_block_retain(block: &DeclarationBlock) -> *mut DeclarationBlock {
    Box::into_raw(Box::new(block.clone()))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_declaration_block_replace(block: &mut DeclarationBlock, source: &DeclarationBlock) {
    block.replace(source);
}

unsafe fn retain_value(value: *const c_void) -> StyleValueArc {
    let value = value.cast::<StyleValueData>();
    assert!(!value.is_null());
    unsafe {
        StyleValueArc::increment_strong_count(value);
        StyleValueArc::from_raw(value)
    }
}

pub(crate) unsafe fn declaration_from_view(property: &FfiDeclaredProperty) -> DeclaredProperty {
    DeclaredProperty {
        property_id: property.property_id,
        important: property.important,
        value: unsafe { retain_value(property.value) },
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_declaration_block_create(
    properties: *const FfiDeclaredProperty,
    property_count: usize,
    custom_properties: *const FfiDeclaredProperty,
    custom_property_count: usize,
) -> *mut DeclarationBlock {
    let mut data = DeclarationBlockBuilder::default();
    for index in 0..property_count {
        data.append_in_specified_order(unsafe { declaration_from_view(&*properties.add(index)) });
    }
    for index in 0..custom_property_count {
        let property = unsafe { &*custom_properties.add(index) };
        data.set_custom_property(CssString::from_utf16(&property.name.to_utf16().unwrap()), unsafe {
            declaration_from_view(property)
        });
    }
    Box::into_raw(Box::new(DeclarationBlock::new(data.finish())))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_declaration_block_share(block: &DeclarationBlock) -> *mut DeclarationBlock {
    Box::into_raw(Box::new(DeclarationBlock::new(block.data())))
}

/// Pin the current declarations without creating a mutable owner or property views.
#[unsafe(no_mangle)]
pub extern "C" fn rust_declaration_block_snapshot(block: &DeclarationBlock) -> *const DeclarationBlockHeader {
    SharedDeclarationBlock::into_raw(block.data())
}

/// Retain an immutable declaration snapshot without creating a live declaration owner.
///
/// # Safety
/// `data` must point at the header of a live block.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_declaration_data_retain(
    data: *const DeclarationBlockHeader,
) -> *const DeclarationBlockHeader {
    unsafe { SharedDeclarationBlock::increment_count(data) };
    data
}

/// Release an immutable declaration snapshot returned by the style engine or retained from a view.
///
/// # Safety
/// `data` must carry a reference to a live block that the caller gives up.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_declaration_data_release(data: *const DeclarationBlockHeader) {
    drop(unsafe { SharedDeclarationBlock::from_raw(data) });
}

/// Visit borrowed declaration values without materializing mutable owners or FFI arrays.
#[unsafe(no_mangle)]
pub extern "C" fn rust_declaration_data_visit(
    data: &DeclarationBlockHeader,
    context: *mut c_void,
    visit: extern "C" fn(*mut c_void, &FfiDeclaredProperty),
) {
    for property in data.properties() {
        visit(context, &property.view(FfiUtf16View::default()));
    }
    for property in data.custom_properties().iter() {
        visit(
            context,
            &property
                .declaration
                .view(FfiUtf16View::from_utf16(property.name.units())),
        );
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_declaration_block_destroy(block: OwnedBox<DeclarationBlock>) {
    drop(block);
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_declaration_block_visit(
    block: &DeclarationBlock,
    context: *mut c_void,
    visit: extern "C" fn(*mut c_void, &FfiDeclaredProperty),
) {
    rust_declaration_data_visit(&block.data(), context, visit);
}

/// The value of the shorthand whose longhands, in canonical order, have the values, or null if it serializes to the
/// empty string.
///
/// # Safety
/// `values` must point to `count` live style values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_shorthand_value_of_longhands(
    shorthand: u16,
    values: *const *const c_void,
    count: usize,
) -> *const c_void {
    let values = unsafe { ak::slice_from_raw_parts(values, count) }
        .iter()
        .map(|&value| RetainedStyleValueData::from_arc(unsafe { retain_value(value) }))
        .collect();
    crate::css::serialize::shorthand_value_of_longhands(shorthand, values)
        .map_or(std::ptr::null(), |value| StyleValueArc::into_raw(value).cast())
}

/// The declarations that the declaration stands for: itself for a longhand, and otherwise its longhands.
unsafe fn declarations_expanding_shorthands(property: &FfiDeclaredProperty) -> Vec<DeclaredProperty> {
    let important = property.important;
    let value = RetainedStyleValueData::from_arc(unsafe { retain_value(property.value) });
    let mut declarations = Vec::new();
    expand_shorthands_with(property.property_id, &value, false, &mut |property_id, value, _| {
        declarations.push(DeclaredProperty {
            property_id,
            important,
            value: value.clone().into_arc(),
        });
    });
    declarations
}

/// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-setproperty
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_declaration_block_set_expanding_shorthands(
    block: &mut DeclarationBlock,
    property: &FfiDeclaredProperty,
) -> bool {
    let declarations = unsafe { declarations_expanding_shorthands(property) };
    block.mutate(|data| {
        // 7. Let updated be false.
        let mut updated = false;

        // 8. If property is a shorthand property, then for each longhand property longhand that property maps to, in
        //    canonical order, follow these substeps:
        // 9. Otherwise, let updated be the result of set the CSS declaration property with value component value list,
        //    with the important flag set if priority is not the empty string, and unset otherwise, and with the list
        //    of declarations being the declarations.
        for declaration in declarations {
            let property_id = declaration.property_id;
            // 1. Let longhand result be the result of set the CSS declaration longhand with the appropriate value(s)
            //    from component value list, with the important flag set if priority is not the empty string, and unset
            //    otherwise, and with the list of declarations being the declarations.
            // 2. If longhand result is true, let updated be true.
            if data.set_declaration(declaration) {
                data.drop_shorthands_no_longer_pending_in(property_id);
                updated = true;
            }
        }
        updated
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_declaration_block_append_expanding_shorthands(
    block: &mut DeclarationBlock,
    property: &FfiDeclaredProperty,
) {
    let declarations = unsafe { declarations_expanding_shorthands(property) };
    block.mutate(|data| {
        for declaration in declarations {
            let property_id = declaration.property_id;
            data.properties.retain(|existing| existing.property_id != property_id);
            data.properties.push(declaration);
            data.drop_shorthands_no_longer_pending_in(property_id);
        }
        true
    });
}

/// https://drafts.csswg.org/cssom/#dom-cssstyledeclaration-removeproperty
#[unsafe(no_mangle)]
pub extern "C" fn rust_declaration_block_remove_expanding_shorthands(
    block: &mut DeclarationBlock,
    property_id: u16,
) -> bool {
    // 4. Let removed be false.
    let mut removed = false;

    // 5. If property is a shorthand property, for each longhand property longhand that property maps to:
    if property_is_shorthand(property_id) {
        for &longhand in longhands_for_shorthand(property_id) {
            // 1. If longhand is not a property name of a CSS declaration in the declarations, continue.
            // 2. Remove that CSS declaration and let removed be true.
            removed |= rust_declaration_block_remove_expanding_shorthands(block, longhand);
        }
    }
    // 6. Otherwise, if property is a case-sensitive match for a property name of a CSS declaration in the declarations,
    //    remove that CSS declaration and let removed be true.
    else {
        removed = block.remove(property_id);
    }
    removed
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_declaration_block_set_custom(
    block: &mut DeclarationBlock,
    property: &FfiDeclaredProperty,
) {
    let name = CssString::from_utf16(&property.name.to_utf16().unwrap());
    let declaration = unsafe { declaration_from_view(property) };
    block.mutate(|data| {
        data.set_custom_property(name, declaration);
        true
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_declaration_block_remove_custom(block: &mut DeclarationBlock, name: FfiUtf16View) -> bool {
    let name = name.to_utf16().unwrap();
    let Some(index) = block
        .data()
        .custom_properties()
        .iter()
        .position(|property| property.name.units() == name)
    else {
        return false;
    };
    block.mutate(|data| {
        data.custom_properties.remove(index);
        true
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_declaration_block_external_memory_size(block: &DeclarationBlock) -> usize {
    let data = block.data();
    let mut size = size_of::<DeclarationBlock>().saturating_add(data.heap_bytes());
    if matches!(*block.state.borrow(), DeclarationBlockState::Mutable(_)) {
        size = size.saturating_add(size_of::<DeclarationBlockOwner>());
    }
    size
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::parser::syntax_parser::parse_shared_stylesheet;
    use crate::css::parser::value_parser::ParseContext;
    use crate::css::property_metadata::property_id;
    use crate::css::rule::rust_rule_payload;

    fn parsed_declarations(text: &str) -> SharedDeclarationBlock {
        let units: Vec<_> = format!(".target {{ {text} }}").encode_utf16().collect();
        let input = crate::css::css_tokenizer::TokenizerInput::Utf16(&units);
        // All context fields are booleans, integers, or nullable pointers.
        let context = ParseContext::default();
        let parse = parse_shared_stylesheet(input, Some(&context));
        let rules = crate::css::rule::NativeRuleList::from_parsed(parse);
        let rule = &rules.rule_at(0);
        rule.cascade_declarations().unwrap()
    }

    #[test]
    fn declaration_snapshots_can_be_forked_and_mutated_on_a_worker() {
        let data = parsed_declarations("width: 13px; --色: green");
        let snapshot = data.clone();
        std::thread::spawn(move || {
            let mut block = DeclarationBlock::new(data);
            let current = block.data();
            let mut view = current.properties().get(0).unwrap().view(FfiUtf16View::default());
            view.important = true;
            assert!(unsafe { rust_declaration_block_set_expanding_shorthands(&mut block, &view) });
            let name = block.data().custom_properties().get(0).unwrap().name.clone();
            assert!(rust_declaration_block_remove_custom(
                &mut block,
                FfiUtf16View::from_utf16(name.units())
            ));
            assert!(block.data().properties().get(0).unwrap().important);
            assert!(block.data().custom_properties().is_empty());
            assert_eq!(crate::css::ffi_stats::CPP_CALLBACK_COUNT.get(), 0);
        })
        .join()
        .unwrap();
        assert!(!snapshot.properties().get(0).unwrap().important);
        assert_eq!(snapshot.custom_properties().len(), 1);
    }

    #[test]
    fn native_nested_declarations_promote_only_for_live_handles_or_edits() {
        let units: Vec<_> = ".親 { .子 {} width: 13px; --幅: 19px; }".encode_utf16().collect();
        let input = crate::css::css_tokenizer::TokenizerInput::Utf16(&units);
        // All context fields are booleans, integers, or nullable pointers.
        let context = ParseContext::default();
        let parse = parse_shared_stylesheet(input, Some(&context));
        let rules = crate::css::rule::NativeRuleList::from_parsed(parse);
        let rule = rules.rule_at(0);
        let nested = rule.child_rules().unwrap().rule_at(1);
        let block = unsafe { &*rust_rule_payload(&nested).declarations };
        let snapshot = nested.cascade_declarations().unwrap();
        let borrowed = block.data();
        assert_eq!(borrowed.properties().len(), 1);
        assert_eq!(borrowed.custom_properties().len(), 1);
        assert!(!rust_declaration_block_is_empty(block));
        assert_eq!(rust_declaration_block_revision(block), 0);
        assert!(rust_declaration_block_external_memory_size(block) > 0);
        let mut fork = unsafe { Box::from_raw(rust_declaration_block_share(block)) };
        assert!(!rust_declaration_block_remove_expanding_shorthands(
            &mut fork,
            property_id::HEIGHT
        ));
        assert!(matches!(*block.state.borrow(), DeclarationBlockState::Immutable(_)));
        assert!(matches!(*fork.state.borrow(), DeclarationBlockState::Immutable(_)));
        assert!(SharedDeclarationBlock::ptr_eq(&snapshot, &fork.data()));

        let mut retained = unsafe { Box::from_raw(rust_declaration_block_retain(block)) };
        assert!(Rc::ptr_eq(&block.owner(), &retained.owner()));
        assert!(rust_declaration_block_remove_expanding_shorthands(
            &mut retained,
            property_id::WIDTH
        ));
        assert!(block.data().properties().is_empty());
        assert_eq!(rust_declaration_block_revision(block), 1);
        // A different handle's mutation cannot invalidate a pinned snapshot.
        assert_eq!(borrowed.properties().get(0).unwrap().property_id, property_id::WIDTH);
        assert_eq!(
            borrowed.custom_properties().get(0).unwrap().name.units(),
            "--幅".encode_utf16().collect::<Vec<_>>()
        );
        assert_eq!(fork.data().properties().len(), 1);
        rust_declaration_block_replace(&mut retained, &fork);
        assert_eq!(block.data().properties().len(), 1);
        assert_eq!(rust_declaration_block_revision(block), 2);
        assert!(matches!(*fork.state.borrow(), DeclarationBlockState::Immutable(_)));
        drop((nested, rule, rules));
        assert!(rust_declaration_block_remove_expanding_shorthands(
            &mut fork,
            property_id::WIDTH
        ));
        assert!(matches!(*fork.state.borrow(), DeclarationBlockState::Mutable(_)));
        assert!(fork.data().properties().is_empty());
        assert_eq!(retained.data().properties().len(), 1);
        assert_eq!(snapshot.properties().len(), 1);
        drop(retained);
        drop(borrowed);
        assert_eq!(snapshot.reference_count(), 1);
        drop(snapshot);
    }
    #[test]
    fn font_synthesis_expands_existing_position_for_ordinary_values() {
        use crate::css::css_enums::keyword;
        let mut observed = Vec::new();
        for source in ["none", "weight", "style", "small-caps", "weight style small-caps"] {
            let data = parsed_declarations(&format!("font-synthesis-position: auto; font-synthesis: {source}"));
            let position = data
                .properties()
                .iter()
                .find(|property| property.property_id == property_id::FONT_SYNTHESIS_POSITION)
                .unwrap();
            let StyleValueData::Keyword { keyword: value } = position.value.as_ref() else {
                panic!("keyword expected")
            };
            observed.push((source, keyword::NAMES[*value as usize]));
        }
        assert_eq!(
            observed,
            vec![
                ("none", "none"),
                ("weight", "none"),
                ("style", "none"),
                ("small-caps", "none"),
                ("weight style small-caps", "none")
            ]
        );
    }

    #[test]
    fn font_synthesis_stylesheet_importance_covers_position() {
        use crate::css::css_enums::keyword;
        let kept = parsed_declarations("font-synthesis-position: auto !important; font-synthesis: none");
        let position = kept
            .properties()
            .iter()
            .find(|property| property.property_id == property_id::FONT_SYNTHESIS_POSITION)
            .unwrap();
        assert!(position.important);
        assert!(
            matches!(position.value.as_ref(), StyleValueData::Keyword { keyword: value } if *value == keyword::AUTO)
        );
        let changed =
            parsed_declarations("font-synthesis-position: auto !important; font-synthesis: weight !important");
        assert_eq!(changed.properties().len(), 4);
        for property in changed.properties() {
            assert!(property.important);
        }
        let position = changed
            .properties()
            .iter()
            .find(|property| property.property_id == property_id::FONT_SYNTHESIS_POSITION)
            .unwrap();
        assert!(
            matches!(position.value.as_ref(), StyleValueData::Keyword { keyword: value } if *value == keyword::NONE)
        );
    }

    #[test]
    fn font_synthesis_css_wide_and_pending_substitutions_cover_all_four() {
        use crate::css::css_enums::keyword;
        for (source, expected) in [
            ("initial", keyword::INITIAL),
            ("inherit", keyword::INHERIT),
            ("unset", keyword::UNSET),
            ("revert", keyword::REVERT),
            ("revert-layer", keyword::REVERT_LAYER),
        ] {
            let data = parsed_declarations(&format!("font-synthesis: {source} !important"));
            assert_eq!(data.properties().len(), 4, "{source}");
            assert!(
                data.properties()
                    .iter()
                    .any(|property| property.property_id == property_id::FONT_SYNTHESIS_POSITION)
            );
            for property in data.properties() {
                assert!(property.important);
                assert!(matches!(property.value.as_ref(), StyleValueData::Keyword { keyword } if *keyword == expected));
            }
        }
        let pending = parsed_declarations("font-synthesis: var(--s)");
        assert_eq!(pending.properties().len(), 5);
        assert_eq!(
            pending.properties().get(0).unwrap().property_id,
            property_id::FONT_SYNTHESIS
        );
        assert!(
            pending
                .properties()
                .iter()
                .skip(1)
                .any(|property| property.property_id == property_id::FONT_SYNTHESIS_POSITION)
        );
        assert!(
            pending
                .properties()
                .iter()
                .skip(1)
                .all(|property| matches!(property.value.as_ref(), StyleValueData::PendingSubstitution { .. }))
        );
    }
}
