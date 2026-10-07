/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ops::Deref;

use ak::Utf16String;
use libjs_runtime_macros::Trace;

use crate::gc::class::{Finalize, GcCell, define_cell};
use crate::gc::gc_ref_cell::GcRefCell;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::runtime::intl::segments::Segments;
use crate::runtime::object::MayInterfereWithIndexedPropertyAccess;
use crate::runtime::realm::Realm;
use crate::unicode::intl as unicode;
use crate::utf16::Utf16View;

/// 19.6 Segment Iterator Objects, https://tc39.es/ecma402/#sec-segment-iterator-objects
#[repr(C)]
#[derive(Trace)]
pub struct SegmentIterator {
    base: Object,
    #[gc(untraced)]
    iterating_segmenter: GcRefCell<Option<unicode::Segmenter>>, // [[IteratingSegmenter]]
    #[gc(untraced)]
    iterated_string: Utf16String, // [[IteratedString]]

    segments: Cell<Gc<Segments>>,
}

define_cell!(SegmentIterator, Object, extends: [Object], finalize: finalize);

impl Deref for SegmentIterator {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

impl Finalize for SegmentIterator {
    fn finalize(&self) {
        drop(self.iterating_segmenter.replace(None));
    }
}

impl SegmentIterator {
    // 19.6.1 CreateSegmentIterator ( segmenter, string ), https://tc39.es/ecma402/#sec-createsegmentiterator
    /// `segmenter` is a copy of the ICU segmenter of `segments`, which the Segment Iterator owns.
    pub fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        mut segmenter: unicode::Segmenter,
        string: Utf16String,
        segments: Gc<Segments>,
    ) -> Gc<SegmentIterator> {
        // 1. Let internalSlotsList be « [[IteratingSegmenter]], [[IteratedString]], [[IteratedStringNextSegmentCodeUnitIndex]] ».
        // 2. Let iterator be OrdinaryObjectCreate(%IntlSegmentIteratorPrototype%, internalSlotsList).
        // 3. Set iterator.[[IteratingSegmenter]] to segmenter.
        // 4. Set iterator.[[IteratedString]] to string.
        // 5. Set iterator.[[IteratedStringNextSegmentCodeUnitIndex]] to 0.
        // 6. Return iterator.
        segmenter.set_segmented_text(Utf16View::of_string(&string));
        realm.create_object(
            vm,
            SegmentIterator {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.intrinsics().intl_segment_iterator_prototype(),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
                iterating_segmenter: GcRefCell::new(Some(segmenter)),
                iterated_string: string,
                segments: Cell::new(segments),
            },
        )
    }

    pub fn iterating_segmenter(&self) -> &GcRefCell<Option<unicode::Segmenter>> {
        &self.iterating_segmenter
    }

    pub fn iterated_string(&self) -> Utf16String {
        self.iterated_string.clone()
    }

    pub fn iterated_string_next_segment_code_unit_index(&self) -> usize {
        self.iterating_segmenter
            .borrow_mut()
            .as_mut()
            .expect("a Segment Iterator has an ICU segmenter")
            .current_boundary()
    }

    pub fn segments(&self) -> Gc<Segments> {
        self.segments.get()
    }
}
