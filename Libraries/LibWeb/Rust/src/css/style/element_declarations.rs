/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The declarations attached to each element: its inline style, presentational hints and SVG
//! presentation attributes, each with the values they were written with.

use super::fast_hash::FastSet as HashSet;
use super::index::StyleAtomID;
use super::input_types::ElementDeclarationKind;
use super::pipeline::rule_store::CustomDeclaration;
use super::tree::StyleNodeID;
use crate::cow_column::{ColumnSnapshot, CowColumn, CowVec};
use crate::css::declaration_block::{DeclarationEntry, SharedDeclarationBlock, prefetch};
use crate::css::style_value::RetainedStyleValueData;

type Checks = super::pipeline::compute::WrittenValueChecks;

/// One kind of an element's own declarations: what each covers, the value it was written with,
/// and what the drive checks of that value, borrowed from the element's row.
#[derive(Clone, Copy, Default)]
pub struct ElementDeclarations<'a> {
    declared: &'a [DeclarationEntry],
    written: &'a [RetainedStyleValueData],
    checks: &'a [Checks],
}

impl<'a> ElementDeclarations<'a> {
    /// A block of declarations of its parts, parallel to one another.
    pub(in crate::css::style) fn of_parts(
        declared: &'a [DeclarationEntry],
        written: &'a [RetainedStyleValueData],
        checks: &'a [Checks],
    ) -> Self {
        Self {
            declared,
            written,
            checks,
        }
    }

    /// The value the declaration at `index` of `declared` was written with.
    #[must_use]
    pub fn written(self, index: usize) -> &'a RetainedStyleValueData {
        &self.written[index]
    }

    /// The declared property at `index`, parallel to its written value.
    pub(in crate::css::style) fn declared_property(self, index: usize) -> Option<&'a DeclarationEntry> {
        self.declared.get(index)
    }

    pub(in crate::css::style) fn checks(self, index: usize) -> Checks {
        self.checks[index]
    }

    /// What each declaration covers.
    pub(in crate::css::style) fn declared(self) -> &'a [DeclarationEntry] {
        self.declared
    }

    pub fn iter(
        self,
    ) -> std::iter::Zip<std::slice::Iter<'a, DeclarationEntry>, std::slice::Iter<'a, RetainedStyleValueData>> {
        self.declared.iter().zip(self.written.iter())
    }
}

/// Declarations standing for their written values with the guaranteed-invalid one, for a test
/// that reads no value.
#[cfg(test)]
fn unwritten_declarations(declared: &[DeclarationEntry]) -> Vec<(DeclarationEntry, RetainedStyleValueData)> {
    declared
        .iter()
        .map(|&declared| {
            (
                declared,
                RetainedStyleValueData::from_owned(crate::css::style_value::StyleValueData::GuaranteedInvalid),
            )
        })
        .collect()
}

/// The custom properties an element's inline style declares, in declaration order, each with
/// the value it was written with, as its row holds them.
#[derive(Clone, Copy)]
pub(in crate::css::style) struct ElementCustomDeclarations<'a> {
    declared: &'a [CustomDeclaration],
    written: &'a [RetainedStyleValueData],
}

impl<'a> ElementCustomDeclarations<'a> {
    #[must_use]
    pub(in crate::css::style) fn declared(self) -> &'a [CustomDeclaration] {
        self.declared
    }

    /// The values the declarations were written with, parallel to them.
    pub(in crate::css::style) fn written_values(self) -> &'a [RetainedStyleValueData] {
        self.written
    }

    /// The declarations, copied out.
    fn copy(self) -> Vec<CustomDeclarationWritten> {
        self.declared
            .iter()
            .copied()
            .zip(self.written.iter().cloned())
            .collect()
    }
}

/// One custom property an element's inline style declares, with the value it was written with,
/// as a row is built from them.
type CustomDeclarationWritten = (CustomDeclaration, RetainedStyleValueData);

/// The start of an element's declaration row. Where the row holds custom declarations, a word
/// counting them follows the header, then two parallel arrays, the values they were written with
/// and their declarations. The declarations of every kind follow as three parallel arrays, the
/// values they were written with, their checks and their entries, each kind's declarations after
/// those of the kinds before it, the narrower arrays last so that none needs padding.
#[repr(C)]
struct RowHeader {
    /// The rows sharing this allocation: copies of a store, as a fork of the render state makes, share every row until
    /// one of them writes it.
    references: std::sync::atomic::AtomicU32,
    /// Where the declarations of each kind end, by `ElementDeclarationKind::index`, the first with
    /// `CUSTOM_SLOT` set where the custom declarations follow the header.
    ends: [u32; ElementDeclarationKind::COUNT],
}

const _: () = assert!(size_of::<RowHeader>() == 16);

/// The bit of a row's first end that says custom declarations follow the header. Only the `style`
/// attribute declares custom properties, so most rows have none.
const CUSTOM_SLOT: u32 = 1 << 31;

/// The word that counts a row's custom declarations, where it holds any.
#[repr(C)]
struct CustomCount {
    count: u32,
    _padding: u32,
}

// NB: The custom count and the arrays follow the header and one another with no padding between them.
const _: () = assert!(size_of::<RowHeader>().is_multiple_of(ROW_ALIGNMENT));
const _: () = assert!(size_of::<CustomCount>() == ROW_ALIGNMENT && align_of::<CustomCount>() <= ROW_ALIGNMENT);
const _: () = assert!(size_of::<CustomDeclaration>() == 8 && align_of::<CustomDeclaration>() <= ROW_ALIGNMENT);
const _: () = assert!(size_of::<DeclarationEntry>() == 2);
const _: () = assert!(size_of::<RetainedStyleValueData>() == 8 && align_of::<RetainedStyleValueData>() <= 8);
const _: () = assert!(size_of::<Checks>() == 4 && align_of::<Checks>() <= 8);

/// The alignment of a row's allocation, which every word after the header shares.
const ROW_ALIGNMENT: usize = 8;

/// The bytes one declaration takes in each of the three arrays together.
const DECLARATION_BYTES: usize =
    size_of::<DeclarationEntry>() + size_of::<RetainedStyleValueData>() + size_of::<Checks>();

/// The bytes one custom declaration takes in its two arrays together.
const CUSTOM_DECLARATION_BYTES: usize = size_of::<RetainedStyleValueData>() + size_of::<CustomDeclaration>();

/// The bytes a row's `custom_count` custom declarations take after its header, with their count.
fn custom_bytes(custom_count: usize) -> usize {
    match custom_count {
        0 => 0,
        _ => size_of::<CustomCount>() + CUSTOM_DECLARATION_BYTES * custom_count,
    }
}

fn row_layout(count: usize, custom_count: usize) -> std::alloc::Layout {
    std::alloc::Layout::from_size_align(
        size_of::<RowHeader>() + custom_bytes(custom_count) + DECLARATION_BYTES * count,
        ROW_ALIGNMENT,
    )
    .expect("an element's declarations fit in memory")
}

/// One of an element's own declarations, as a row is built from them.
pub(in crate::css::style) type ElementDeclaration = (DeclarationEntry, RetainedStyleValueData);

/// An element's own declarations, of every kind, with the custom properties its inline style
/// declares, in one allocation.
pub(in crate::css::style) struct ElementDeclarationRow(std::ptr::NonNull<RowHeader>);

// SAFETY: The row owns its allocation, whose values are shareable across threads.
unsafe impl Send for ElementDeclarationRow {}
// SAFETY: As above; nothing mutates a row through a shared reference.
unsafe impl Sync for ElementDeclarationRow {}

/// The custom properties of an element's inline declaration, taken out of its row while a private demand computes the
/// element as though it had none.
pub(super) struct HiddenInlineDeclarations {
    custom: Vec<CustomDeclarationWritten>,
}

impl ElementDeclarationRow {
    /// The declarations of each kind, by `ElementDeclarationKind::index`, with the custom
    /// properties the inline style declares.
    pub(in crate::css::style) fn new(
        by_kind: [Vec<ElementDeclaration>; ElementDeclarationKind::COUNT],
        custom: Vec<CustomDeclarationWritten>,
    ) -> Self {
        let mut ends = [0; ElementDeclarationKind::COUNT];
        let mut end = 0;
        for (kind, declarations) in by_kind.iter().enumerate() {
            end += declarations.len();
            ends[kind] = u32::try_from(end).expect("an element's declarations fit in u32");
        }
        Self::build(
            by_kind.into_iter().flatten().map(|(declared, written)| {
                let checks = Checks::prepare(declared.property(), &written);
                (declared, written, checks)
            }),
            ends,
            custom,
        )
    }

    /// A row of `declarations`, the declarations of each kind ending where `ends` says.
    fn build(
        declarations: impl Iterator<Item = (DeclarationEntry, RetainedStyleValueData, Checks)>,
        ends: [u32; ElementDeclarationKind::COUNT],
        custom: Vec<CustomDeclarationWritten>,
    ) -> Self {
        let count = ends[ElementDeclarationKind::COUNT - 1] as usize;
        assert!(
            ends[0] & CUSTOM_SLOT == 0,
            "an element's declarations are fewer than its row can count"
        );
        let custom_count = custom.len();
        let layout = row_layout(count, custom_count);
        // SAFETY: The layout is never zero-sized, as it holds the header.
        let pointer = unsafe { std::alloc::alloc(layout) }.cast::<RowHeader>();
        let Some(header) = std::ptr::NonNull::new(pointer) else {
            std::alloc::handle_alloc_error(layout);
        };
        // SAFETY: The allocation holds the header, the custom count and `custom_count` entries of
        //         each custom array, and `count` entries of each other array, which are written once
        //         each, at the alignment of their elements.
        unsafe {
            let mut stored_ends = ends;
            if custom_count != 0 {
                stored_ends[0] |= CUSTOM_SLOT;
            }
            header.write(RowHeader {
                references: std::sync::atomic::AtomicU32::new(1),
                ends: stored_ends,
            });
            // NB: The row owns its values once they are all written, so a panic while writing them
            //     leaks the allocation rather than dropping what was never written.
            let row = std::mem::ManuallyDrop::new(Self(header));
            if custom_count != 0 {
                header.add(1).cast::<CustomCount>().write(CustomCount {
                    count: u32::try_from(custom_count).expect("an element's custom declarations fit in u32"),
                    _padding: 0,
                });
                let (custom_written, custom_declared) = row.custom_arrays(custom_count);
                for (index, (declared, written)) in custom.into_iter().enumerate() {
                    custom_declared.cast_mut().add(index).write(declared);
                    custom_written.cast_mut().add(index).write(written);
                }
            }
            let (declared, written, checks) = row.arrays(count);
            let mut written_count = 0;
            for (index, entry) in declarations.enumerate() {
                assert!(index < count, "an element's declarations are as many as its ends say");
                declared.cast_mut().add(index).write(entry.0);
                written.cast_mut().add(index).write(entry.1);
                checks.cast_mut().add(index).write(entry.2);
                written_count += 1;
            }
            assert_eq!(
                written_count, count,
                "an element's declarations are as many as its ends say"
            );
            std::mem::ManuallyDrop::into_inner(row)
        }
    }

    fn header(&self) -> &RowHeader {
        // SAFETY: The row owns its allocation, which starts with the header.
        unsafe { self.0.as_ref() }
    }

    /// Where the declarations of each kind end.
    fn ends(&self) -> [u32; ElementDeclarationKind::COUNT] {
        let mut ends = self.header().ends;
        ends[0] &= !CUSTOM_SLOT;
        ends
    }

    fn count(&self) -> usize {
        self.ends()[ElementDeclarationKind::COUNT - 1] as usize
    }

    /// How many custom declarations the row holds.
    fn custom_count(&self) -> usize {
        if self.header().ends[0] & CUSTOM_SLOT == 0 {
            return 0;
        }
        // SAFETY: The count follows the header where the header says so.
        unsafe { (*self.0.as_ptr().add(1).cast::<CustomCount>()).count as usize }
    }

    /// The starts of the two custom arrays of a row holding `custom_count` custom declarations.
    fn custom_arrays(&self, custom_count: usize) -> (*const RetainedStyleValueData, *const CustomDeclaration) {
        // SAFETY: The custom arrays lie within the allocation, one after the other past the header
        //         and the custom count.
        unsafe {
            let written = self
                .0
                .as_ptr()
                .add(1)
                .cast::<CustomCount>()
                .add(1)
                .cast::<RetainedStyleValueData>();
            let declared = written.add(custom_count).cast::<CustomDeclaration>();
            (written.cast_const(), declared.cast_const())
        }
    }

    /// The starts of the three arrays of a row holding `count` declarations.
    fn arrays(&self, count: usize) -> (*const DeclarationEntry, *const RetainedStyleValueData, *const Checks) {
        // SAFETY: The arrays lie within the allocation, one after another past the header and the
        //         custom declarations.
        unsafe {
            let written = self
                .0
                .as_ptr()
                .add(1)
                .cast::<u8>()
                .add(custom_bytes(self.custom_count()))
                .cast::<RetainedStyleValueData>();
            let checks = written.add(count).cast::<Checks>();
            let declared = checks.add(count).cast::<DeclarationEntry>();
            (declared, written, checks)
        }
    }

    /// Every declaration of the row, of every kind.
    fn all(&self) -> ElementDeclarations<'_> {
        let count = self.count();
        let (declared, written, checks) = self.arrays(count);
        // SAFETY: The row was built with `count` entries in each array, which live as it does.
        unsafe {
            ElementDeclarations {
                declared: ak::slice_from_raw_parts(declared, count),
                written: ak::slice_from_raw_parts(written, count),
                checks: ak::slice_from_raw_parts(checks, count),
            }
        }
    }

    /// The declarations of `kind`.
    pub(in crate::css::style) fn declarations(&self, kind: ElementDeclarationKind) -> ElementDeclarations<'_> {
        let ends = self.ends();
        let start = match kind.index() {
            0 => 0,
            index => ends[index - 1] as usize,
        };
        let range = start..ends[kind.index()] as usize;
        let all = self.all();
        ElementDeclarations {
            declared: &all.declared[range.clone()],
            written: &all.written[range.clone()],
            checks: &all.checks[range],
        }
    }

    /// The custom properties the inline style declares, if any.
    pub(in crate::css::style) fn custom(&self) -> Option<ElementCustomDeclarations<'_>> {
        let custom_count = self.custom_count();
        if custom_count == 0 {
            return None;
        }
        let (written, declared) = self.custom_arrays(custom_count);
        // SAFETY: The row was built with `custom_count` entries in each custom array, which live as it does.
        unsafe {
            Some(ElementCustomDeclarations {
                declared: ak::slice_from_raw_parts(declared, custom_count),
                written: ak::slice_from_raw_parts(written, custom_count),
            })
        }
    }

    /// The row's custom declarations, copied out.
    fn copy_custom(&self) -> Vec<CustomDeclarationWritten> {
        self.custom().map_or_else(Vec::new, ElementCustomDeclarations::copy)
    }

    /// This row with the declarations of `kind` replaced by `declarations`, and its custom
    /// declarations by `custom`.
    fn with(
        &self,
        kind: ElementDeclarationKind,
        declarations: impl ExactSizeIterator<Item = (DeclarationEntry, RetainedStyleValueData, Checks)>,
        custom: Vec<CustomDeclarationWritten>,
    ) -> Self {
        let mut ends = [0; ElementDeclarationKind::COUNT];
        let mut end = 0;
        for other in ElementDeclarationKind::ALL {
            end += match other == kind {
                true => declarations.len(),
                false => self.declarations(other).declared.len(),
            };
            ends[other.index()] = u32::try_from(end).expect("an element's declarations fit in u32");
        }
        let mut declarations = Some(declarations);
        let entries = ElementDeclarationKind::ALL.into_iter().flat_map(|other| {
            let (own, replaced) = match other == kind {
                true => (None, declarations.take()),
                false => (Some(self.declarations(other)), None),
            };
            let own = own.into_iter().flat_map(|own| {
                (0..own.declared.len())
                    .map(move |index| (own.declared[index], own.written[index].clone(), own.checks[index]))
            });
            own.chain(replaced.into_iter().flatten())
        });
        Self::build(entries, ends, custom)
    }

    fn storage_bytes(&self) -> usize {
        row_layout(self.count(), self.custom_count()).size()
    }

    fn is_empty(&self) -> bool {
        self.count() == 0 && self.custom().is_none()
    }

    /// The hash of what the row declares: its declarations of each kind, and its custom ones.
    fn content_hash(&self) -> u64 {
        use std::hash::{Hash, Hasher};
        let mut hasher = crate::fast_hash::fast_hasher();
        self.ends().hash(&mut hasher);
        let all = self.all();
        all.declared.hash(&mut hasher);
        let custom = self.custom();
        custom.map(ElementCustomDeclarations::declared).hash(&mut hasher);
        let custom_written = custom.map_or(&[][..], ElementCustomDeclarations::written_values);
        for value in all.written.iter().chain(custom_written) {
            crate::css::style_value::style_value_content_hash(value.optional_data()).hash(&mut hasher);
        }
        hasher.finish()
    }

    /// Whether the two rows declare the same, written with values of equal content.
    fn declares_the_same_as(&self, other: &Self) -> bool {
        fn custom_written(row: &ElementDeclarationRow) -> &[RetainedStyleValueData] {
            row.custom().map_or(&[], ElementCustomDeclarations::written_values)
        }
        self.0 == other.0
            || (self.ends() == other.ends()
                && self.all().declared == other.all().declared
                && self.all().written == other.all().written
                && self.custom().map(ElementCustomDeclarations::declared)
                    == other.custom().map(ElementCustomDeclarations::declared)
                && custom_written(self) == custom_written(other))
    }
}

impl Clone for ElementDeclarationRow {
    fn clone(&self) -> Self {
        let references = self
            .header()
            .references
            .fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        assert!(
            references < u32::MAX / 2,
            "element declaration row reference count overflow"
        );
        Self(self.0)
    }
}

impl Drop for ElementDeclarationRow {
    fn drop(&mut self) {
        if self
            .header()
            .references
            .fetch_sub(1, std::sync::atomic::Ordering::Release)
            != 1
        {
            return;
        }
        std::sync::atomic::fence(std::sync::atomic::Ordering::Acquire);
        let count = self.count();
        let (_, written, _) = self.arrays(count);
        let custom_count = self.custom_count();
        let (custom_written, _) = self.custom_arrays(custom_count);
        // SAFETY: This was the last reference to the allocation and every value in it; nothing reads them after this.
        unsafe {
            std::ptr::drop_in_place(std::ptr::slice_from_raw_parts_mut(written.cast_mut(), count));
            std::ptr::drop_in_place(std::ptr::slice_from_raw_parts_mut(
                custom_written.cast_mut(),
                custom_count,
            ));
            std::ptr::drop_in_place(self.0.as_ptr());
            std::alloc::dealloc(self.0.as_ptr().cast(), row_layout(count, custom_count));
        }
    }
}

impl ElementDeclarationRow {
    /// Replace the row's custom declarations with `custom`.
    fn set_custom(&mut self, custom: Vec<CustomDeclarationWritten>) {
        if custom.is_empty() && self.custom_count() == 0 {
            return;
        }
        let all = self.all();
        *self = Self::build(
            (0..all.declared.len()).map(|index| (all.declared[index], all.written[index].clone(), all.checks[index])),
            self.ends(),
            custom,
        );
    }
}

/// The block of an element's inline declarations, as the column of them holds it. Two are the same only as the same
/// block: a block's content is the host's to compare.
#[derive(Clone, Default)]
pub(in crate::css::style) struct InlineDeclarations(Option<SharedDeclarationBlock>);

impl PartialEq for InlineDeclarations {
    fn eq(&self, other: &Self) -> bool {
        match (&self.0, &other.0) {
            (Some(block), Some(other)) => std::ptr::eq(block.as_ptr(), other.as_ptr()),
            (None, None) => true,
            _ => false,
        }
    }
}

impl InlineDeclarations {
    pub(in crate::css::style) fn new(block: Option<SharedDeclarationBlock>) -> Self {
        Self(block)
    }

    pub(in crate::css::style) fn is_some(&self) -> bool {
        self.0.is_some()
    }

    pub(in crate::css::style) fn into_block(self) -> Option<SharedDeclarationBlock> {
        self.0
    }

    /// The bytes of what the engine derived from the block and of the values it holds that `sizer` reaches for the first
    /// time. The block itself is its element's.
    pub(in crate::css::style) fn heap_size(&self, sizer: &mut crate::heap_size::HeapSizer) -> usize {
        use crate::heap_size::HeapSize;
        let Some(block) = &self.0 else {
            return 0;
        };
        if !sizer.first_sight(block.as_ptr()) {
            return 0;
        }
        block.cascade_data().map_or(0, |data| data.heap_bytes())
            + RetainedStyleValueData::slice_from_shared(block.properties().values()).heap_size(sizer)
            + RetainedStyleValueData::slice_from_shared(block.custom_properties().values()).heap_size(sizer)
    }
}

/// How many elements a chunk of the column of inline declarations holds.
const INLINE_DECLARATIONS_PER_CHUNK: usize = 64;

/// The inline declarations of a document's elements by slot, which its host writes as it writes their rows, made only as
/// far as the last element that held any.
pub(in crate::css::style) type InlineDeclarationColumn = CowColumn<InlineDeclarations, INLINE_DECLARATIONS_PER_CHUNK>;

/// The inline declarations of a document's elements as its host last published them.
pub(in crate::css::style) type InlineDeclarationSnapshot =
    ColumnSnapshot<InlineDeclarations, INLINE_DECLARATIONS_PER_CHUNK>;

const _: () = {
    const fn assert_shared_between_threads<T: Send + Sync>() {}
    assert_shared_between_threads::<InlineDeclarationSnapshot>();
};

/// Element-declaration rows addressed directly by dense element identity.
///
/// An element's inline declarations are read from the block the host published in its column. Each directory slot of the
/// rows costs one pointer, and a populated slot holds one allocation with the presentation attributes' declarations and
/// the custom properties of the inline declarations.
#[derive(Clone, Default)]
pub(super) struct ElementDeclarationStore {
    /// Each element's inline declarations as the host published them.
    inline: InlineDeclarationSnapshot,
    /// The element whose inline declarations read as none while a private demand computes it.
    hidden_inline: Option<StyleNodeID>,
    /// What a test wrote of the column in place of a host.
    #[cfg(test)]
    inline_written_for_test: InlineDeclarationColumn,
    /// Each element's row, by element index, copied on write in chunks so that a fork shares them.
    rows: CowVec<Option<ElementDeclarationRow>, ELEMENT_DECLARATION_ROWS_PER_CHUNK>,
    /// How many rows hold custom declarations: none means no inline style declares one.
    rows_with_custom_declarations: usize,
    /// The rows put last, which a row declaring the same is shared with.
    recent: RecentRows,
}

/// How many element slots a chunk of the store's rows holds.
const ELEMENT_DECLARATION_ROWS_PER_CHUNK: usize = 128;

const _: () = assert!(size_of::<Option<ElementDeclarationRow>>() == 8);

/// How many rows the store keeps to share, each in the slot its content hashes to.
const RECENT_ROWS: usize = 32;

/// The rows an element's declarations were put in last, by the hash of what they declare: an
/// element whose declarations are the same as one put recently holds that row, so elements
/// declaring alike share one. A fork shares the slots, as it does the rows.
#[derive(Clone, Default)]
struct RecentRows([Option<ElementDeclarationRow>; RECENT_ROWS]);

impl RecentRows {
    /// The row declaring what `row` declares that was put recently, or `row`, kept to share.
    fn share(&mut self, row: ElementDeclarationRow) -> ElementDeclarationRow {
        let slot = &mut self.0[(row.content_hash() % RECENT_ROWS as u64) as usize];
        if let Some(recent) = slot
            && recent.declares_the_same_as(&row)
        {
            return recent.clone();
        }
        *slot = Some(row.clone());
        row
    }
}

impl ElementDeclarationStore {
    /// The bytes of the store's rows and the declarations they hold, with the values those were
    /// written with counted aside.
    pub(super) fn heap_size(&self, sizer: &mut crate::heap_size::HeapSizer) -> usize {
        use crate::heap_size::HeapSize;
        let mut bytes =
            self.rows.heap_size(sizer, |_, _| 0) + self.inline.heap_size(sizer, InlineDeclarations::heap_size);
        for row in self.rows.iter().flatten() {
            if !sizer.first_sight(row.0.as_ptr()) {
                continue;
            }
            bytes += row.storage_bytes();
            row.all().written.heap_size(sizer);
            if let Some(custom) = row.custom() {
                custom.written.heap_size(sizer);
            }
        }
        bytes
    }

    /// Have the inline declarations of the elements read as the host published them.
    pub(super) fn adopt_inline(&mut self, inline: InlineDeclarationSnapshot) {
        self.inline = inline;
    }

    /// Have the inline declarations of `node` be `declarations`, as a host would publish them.
    #[cfg(test)]
    pub(super) fn set_inline_for_test(
        &mut self,
        node: StyleNodeID,
        declarations: Vec<(DeclarationEntry, RetainedStyleValueData)>,
    ) {
        let mut builder = crate::css::declaration_block::DeclarationBlockBuilder::default();
        builder.properties = declarations
            .into_iter()
            .map(|(declared, written)| crate::css::declaration_block::DeclaredProperty {
                property_id: declared.property(),
                important: declared.important(),
                value: written.into_arc(),
            })
            .collect();
        let block = builder.finish();
        let slot = node.element_slot();
        self.inline_written_for_test.grow_to(slot + 1);
        self.inline_written_for_test.set(slot, InlineDeclarations(Some(block)));
        self.inline = self.inline_written_for_test.publish();
    }

    fn inline_block(&self, node: StyleNodeID) -> Option<&SharedDeclarationBlock> {
        node.element_index()?;
        if self.hidden_inline == Some(node) {
            return None;
        }
        self.inline.get(node.element_slot())?.0.as_ref()
    }

    fn inline_declarations(&self, node: StyleNodeID) -> Option<ElementDeclarations<'_>> {
        let block = self.inline_block(node)?;
        let properties = block.properties();
        // NB: The element's block is immutable, and what is derived from it is made by the first thread to read it, as
        //     an element nothing styles costs nothing here.
        block.make_cascade_data(super::pipeline::rule_store::BlockCascadeData::new);
        Some(ElementDeclarations {
            declared: properties.entries(),
            written: RetainedStyleValueData::slice_from_shared(properties.values()),
            checks: block
                .cascade_data()
                .expect("the block's cascade data was just made")
                .checks(),
        })
    }

    fn row(&self, node: StyleNodeID) -> Option<&ElementDeclarationRow> {
        let index = node.element_index()? as usize;
        self.rows.get(index)?.as_ref()
    }

    /// Ask the processor to fetch a node's row and inline declarations, which reading their addresses alone does not.
    fn prefetch_row(&self, node: StyleNodeID) {
        if let Some(block) = self.inline_block(node) {
            prefetch(block.as_ptr());
        }
        if let Some(row) = self.row(node) {
            prefetch(row.0.as_ptr());
        }
    }

    /// Ask the processor to fetch the values a node's declarations were written with, reading the
    /// row, the block and their declarations.
    fn prefetch_written_values(&self, node: StyleNodeID) {
        if let Some(inline) = self.inline_declarations(node) {
            for written in inline.written {
                prefetch(written.pointer());
            }
        }
        let Some(row) = self.row(node) else {
            return;
        };
        for written in row.all().written {
            prefetch(written.pointer());
        }
    }

    /// Ask the processor to fetch the declarations a node's row and block hold, reading those.
    fn prefetch_declarations(&self, node: StyleNodeID) {
        if let Some(block) = self.inline_block(node) {
            block.prefetch_arrays();
        }
        let Some(row) = self.row(node) else {
            return;
        };
        let all = row.all();
        if !all.declared.is_empty() {
            prefetch(all.written.as_ptr());
            prefetch(all.checks.as_ptr());
        }
    }

    /// The longhand properties one kind of the node's own declarations covers.
    #[must_use]
    pub(super) fn declared_properties(&self, node: StyleNodeID, kind: ElementDeclarationKind) -> &[DeclarationEntry] {
        self.declarations(node, kind)
            .map_or(&[], |declarations| declarations.declared)
    }

    /// One kind of the node's element declarations, with the values they were written with;
    /// `None` when it has none of that kind or of any other its row holds.
    #[must_use]
    pub(super) fn declarations(
        &self,
        node: StyleNodeID,
        kind: ElementDeclarationKind,
    ) -> Option<ElementDeclarations<'_>> {
        match kind {
            ElementDeclarationKind::InlineStyle => self.inline_declarations(node),
            _ => self.row(node).map(|row| row.declarations(kind)),
        }
    }

    /// Put `row` in the slot at `index`, or empty the slot where `row` holds nothing.
    /// `had_custom` says whether the row it replaces held custom declarations.
    fn put(&mut self, index: usize, row: Option<ElementDeclarationRow>, had_custom: bool) {
        let row = row.filter(|row| !row.is_empty()).map(|row| self.recent.share(row));
        let has_custom = row.as_ref().is_some_and(|row| row.custom().is_some());
        if index >= self.rows.len() {
            self.rows.resize_with_default(index + 1);
        }
        self.rows[index] = row;
        self.rows_with_custom_declarations =
            self.rows_with_custom_declarations + usize::from(has_custom) - usize::from(had_custom);
    }

    /// Record which longhand properties one of an element's own declarations covers, each with
    /// the value it was written with.
    pub(super) fn set(
        &mut self,
        node: StyleNodeID,
        kind: ElementDeclarationKind,
        declarations: Vec<(DeclarationEntry, RetainedStyleValueData)>,
    ) {
        let index = node.element_index().expect("only elements carry element declarations") as usize;
        debug_assert!(kind != ElementDeclarationKind::InlineStyle);
        let declarations = declarations.into_iter().map(|(declared, written)| {
            let checks = Checks::prepare(declared.property(), &written);
            (declared, written, checks)
        });
        self.replace_kind(index, kind, declarations);
    }

    /// Replace the declarations of `kind` in the row at `index`.
    fn replace_kind(
        &mut self,
        index: usize,
        kind: ElementDeclarationKind,
        declarations: impl ExactSizeIterator<Item = (DeclarationEntry, RetainedStyleValueData, Checks)>,
    ) {
        let (row, had_custom) = match self.rows.get_mut(index).and_then(Option::as_mut) {
            Some(row) => {
                let had_custom = row.custom().is_some();
                let custom = row.copy_custom();
                (row.with(kind, declarations, custom), had_custom)
            }
            None => {
                if declarations.len() == 0 {
                    return;
                }
                let mut ends = [0; ElementDeclarationKind::COUNT];
                let count = u32::try_from(declarations.len()).expect("an element's declarations fit in u32");
                for other in ElementDeclarationKind::ALL {
                    if other.index() >= kind.index() {
                        ends[other.index()] = count;
                    }
                }
                (ElementDeclarationRow::build(declarations, ends, Vec::new()), false)
            }
        };
        self.put(index, Some(row), had_custom);
    }

    /// The custom properties the node's inline style declares, with the values they were written
    /// with.
    #[must_use]
    pub(super) fn custom(&self, node: StyleNodeID) -> Option<ElementCustomDeclarations<'_>> {
        self.row(node)?.custom()
    }

    /// The custom properties the node's inline style declares, in declaration order.
    #[must_use]
    pub(super) fn custom_declarations(&self, node: StyleNodeID) -> &[CustomDeclaration] {
        self.custom(node).map_or(&[], ElementCustomDeclarations::declared)
    }

    /// Whether any inline style declares a custom property.
    #[must_use]
    pub(super) fn any_declares_custom_properties(&self) -> bool {
        self.rows_with_custom_declarations != 0
    }

    /// Record the custom properties the node's inline style declares, each with the value it was
    /// written with.
    pub(super) fn set_custom(&mut self, node: StyleNodeID, custom: Vec<CustomDeclarationWritten>) {
        let index = node.element_index().expect("only elements carry element declarations") as usize;
        let Some(row) = self.rows.get_mut(index).and_then(Option::as_mut) else {
            if !custom.is_empty() {
                let ends = [0; ElementDeclarationKind::COUNT];
                self.put(
                    index,
                    Some(ElementDeclarationRow::build(std::iter::empty(), ends, custom)),
                    false,
                );
            }
            return;
        };
        let had_custom = row.custom().is_some();
        let has_custom = !custom.is_empty();
        row.set_custom(custom);
        self.rows_with_custom_declarations =
            self.rows_with_custom_declarations + usize::from(has_custom) - usize::from(had_custom);
        if row.is_empty() {
            self.rows[index] = None;
        }
    }

    /// Drop the node's declarations, before its identity is handed to another element.
    pub(super) fn forget(&mut self, node: StyleNodeID) {
        let Some(index) = node.element_index().map(|index| index as usize) else {
            return;
        };
        if let Some(row) = self.rows.get_mut(index).and_then(Option::take)
            && row.custom().is_some()
        {
            self.rows_with_custom_declarations -= 1;
        }
    }

    /// Have the node's inline declaration read as none while a private demand computes the node as though it had none.
    /// The demand puts it back before it returns.
    pub(super) fn hide_inline_for_demand(&mut self, node: StyleNodeID) -> Option<HiddenInlineDeclarations> {
        node.element_index()?;
        let custom = self.row(node).map_or_else(Vec::new, ElementDeclarationRow::copy_custom);
        if !custom.is_empty() {
            self.set_custom(node, Vec::new());
        }
        self.hidden_inline = Some(node);
        Some(HiddenInlineDeclarations { custom })
    }

    pub(super) fn restore_inline_after_demand(&mut self, node: StyleNodeID, hidden: HiddenInlineDeclarations) {
        self.hidden_inline = None;
        if !hidden.custom.is_empty() {
            self.set_custom(node, hidden.custom);
        }
    }

    /// Ask the processor to fetch the own declarations of the next elements a traversal styles,
    /// each a cold pointer further from the dense column than the last: the values of the first,
    /// the declarations of the second and the row of the third, each of which the call before
    /// fetched what reading it reads.
    pub(super) fn prefetch(&self, next: [Option<StyleNodeID>; 3]) {
        if let Some(node) = next[0] {
            self.prefetch_written_values(node);
        }
        if let Some(node) = next[1] {
            self.prefetch_declarations(node);
        }
        if let Some(node) = next[2] {
            self.prefetch_row(node);
        }
    }

    /// Add the custom property names the declarations hold, returning how many rows and names
    /// were visited.
    pub(super) fn collect_atoms(&self, atoms: &mut HashSet<StyleAtomID>) -> u64 {
        let mut visited = 0_u64;
        for row in self.rows.iter().flatten() {
            visited += 1;
            for declaration in row.custom().into_iter().flat_map(ElementCustomDeclarations::declared) {
                visited += 1;
                atoms.insert(declaration.name);
            }
        }
        visited
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_fork_keeps_the_rows_it_had_when_the_store_writes() {
        let mut store = ElementDeclarationStore::default();
        let hints = ElementDeclarationKind::PresentationalHint;
        let declared =
            |property| DeclarationEntry::new(property, false, super::super::cascade::CascadeOperator::Declared);
        let node = StyleNodeID::element(5);
        store.set(node, hints, unwritten_declarations(&[declared(3)]));
        let fork = store.clone();
        store.set(node, hints, unwritten_declarations(&[declared(4)]));
        assert_eq!(fork.declared_properties(node, hints), &[declared(3)][..]);
        assert_eq!(store.declared_properties(node, hints), &[declared(4)][..]);
    }

    #[test]
    fn a_panic_while_a_row_is_written_drops_no_value() {
        let ends = [1; ElementDeclarationKind::COUNT];
        let panicking = std::iter::from_fn(|| -> Option<(DeclarationEntry, RetainedStyleValueData, Checks)> {
            panic!("a declaration fails to prepare");
        });
        let built = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            ElementDeclarationRow::build(panicking, ends, Vec::new())
        }));
        assert!(built.is_err());
    }

    #[test]
    fn elements_declaring_alike_share_one_row() {
        let mut store = ElementDeclarationStore::default();
        let hints = ElementDeclarationKind::PresentationalHint;
        let declared =
            |property| DeclarationEntry::new(property, false, super::super::cascade::CascadeOperator::Declared);
        let (first, second, third) = (
            StyleNodeID::element(1),
            StyleNodeID::element(2),
            StyleNodeID::element(3),
        );
        store.set(first, hints, unwritten_declarations(&[declared(3)]));
        store.set(second, hints, unwritten_declarations(&[declared(3)]));
        store.set(third, hints, unwritten_declarations(&[declared(5)]));
        let row = |node| store.row(node).expect("the element declares").0;
        assert_eq!(row(first), row(second));
        assert_ne!(row(first), row(third));

        // A write to one of them leaves the other's declarations as they were.
        store.set(second, hints, unwritten_declarations(&[declared(4)]));
        assert_eq!(store.declared_properties(first, hints), &[declared(3)][..]);
        assert_eq!(store.declared_properties(second, hints), &[declared(4)][..]);
    }

    #[test]
    fn a_row_holds_its_custom_declarations_before_the_others() {
        use super::super::cascade::CascadeOperator;
        let mut store = ElementDeclarationStore::default();
        let inline = ElementDeclarationKind::InlineStyle;
        let hints = ElementDeclarationKind::PresentationalHint;
        let declared = |property| DeclarationEntry::new(property, false, CascadeOperator::Declared);
        let custom = |name| CustomDeclaration::new(StyleAtomID(name), false, CascadeOperator::Declared);
        let written = |declarations: &[CustomDeclaration]| -> Vec<CustomDeclarationWritten> {
            declarations
                .iter()
                .map(|&declaration| {
                    (
                        declaration,
                        RetainedStyleValueData::from_owned(crate::css::style_value::StyleValueData::GuaranteedInvalid),
                    )
                })
                .collect()
        };
        let node = StyleNodeID::element(2);

        store.set(node, hints, unwritten_declarations(&[declared(3)]));
        store.set_custom(node, written(&[custom(7), custom(8)]));
        store.set_inline_for_test(node, unwritten_declarations(&[declared(1), declared(2)]));
        assert!(store.any_declares_custom_properties());
        assert_eq!(store.custom_declarations(node), &[custom(7), custom(8)][..]);
        assert_eq!(store.custom(node).map(|custom| custom.written_values().len()), Some(2));
        assert_eq!(store.declared_properties(node, inline), &[declared(1), declared(2)][..]);
        assert_eq!(store.declared_properties(node, hints), &[declared(3)][..]);
        let row = store.row(node).expect("the element declares");
        assert_eq!(row.storage_bytes(), 16 + 8 + 2 * 16 + 14);

        // A private demand takes the inline declarations, custom ones too, out and puts them back.
        let hidden = store.hide_inline_for_demand(node).expect("the element declares");
        assert!(!store.any_declares_custom_properties());
        assert!(store.custom_declarations(node).is_empty());
        assert_eq!(store.declared_properties(node, hints), &[declared(3)][..]);
        store.restore_inline_after_demand(node, hidden);
        assert_eq!(store.custom_declarations(node), &[custom(7), custom(8)][..]);
        assert_eq!(store.declared_properties(node, inline), &[declared(1), declared(2)][..]);

        store.set_custom(node, Vec::new());
        assert!(!store.any_declares_custom_properties());
        assert!(store.custom(node).is_none());
        assert_eq!(store.declared_properties(node, hints), &[declared(3)][..]);
    }

    #[test]
    fn element_declarations_follow_dense_element_identity_slots() {
        let mut store = ElementDeclarationStore::default();
        let first = StyleNodeID::element(3);
        let later = StyleNodeID::element(64);
        let inline = ElementDeclarationKind::InlineStyle;
        let hints = ElementDeclarationKind::PresentationalHint;
        let svg = ElementDeclarationKind::SvgPresentationAttribute;
        let declared = |property, important| {
            DeclarationEntry::new(property, important, super::super::cascade::CascadeOperator::Declared)
        };

        store.set_inline_for_test(first, unwritten_declarations(&[declared(1, false), declared(2, true)]));
        store.set(first, hints, unwritten_declarations(&[declared(3, false)]));
        store.set(later, svg, unwritten_declarations(&[declared(4, false)]));
        assert_eq!(
            store.declared_properties(first, inline),
            &[declared(1, false), declared(2, true)][..]
        );
        assert_eq!(store.declared_properties(first, hints), &[declared(3, false)][..]);
        assert_eq!(store.declared_properties(later, svg), &[declared(4, false)][..]);
        assert!(store.declared_properties(later, inline).is_empty());
        assert_eq!(store.rows.len(), 65);
        assert_eq!(store.declared_properties(first, hints), &[declared(3, false)][..]);

        // Retirement has to clear a row before the dense identity is reused.
        store.forget(first);
        assert!(store.declared_properties(first, hints).is_empty());
        assert_eq!(store.declared_properties(later, svg), &[declared(4, false)][..]);
    }
}
