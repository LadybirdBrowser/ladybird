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
use crate::runtime::object::MayInterfereWithIndexedPropertyAccess;
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::realm::Realm;
use crate::unicode::intl as unicode;
use crate::utf16::Utf16View;

/// 19.5 Segments Objects, https://tc39.es/ecma402/#sec-segments-objects
#[repr(C)]
#[derive(Trace)]
pub struct Segments {
    base: Object,
    #[gc(untraced)]
    segments_segmenter: GcRefCell<Option<unicode::Segmenter>>, // [[SegmentsSegmenter]]
    segments_string_value: Cell<Gc<PrimitiveString>>, // [[SegmentsStringValue]]
    #[gc(untraced)]
    segments_string: Utf16String,  // [[SegmentsString]]
}

define_cell!(Segments, Object, extends: [Object], finalize: finalize);

impl Deref for Segments {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

impl Finalize for Segments {
    fn finalize(&self) {
        drop(self.segments_segmenter.replace(None));
    }
}

impl Segments {
    // 19.5.1 CreateSegmentsObject ( segmenter, string ), https://tc39.es/ecma402/#sec-createsegmentsobject
    /// `segmenter` is a copy of the ICU segmenter of the Intl.Segmenter, which the Segments object owns.
    pub fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        mut segmenter: unicode::Segmenter,
        string: Gc<PrimitiveString>,
    ) -> Gc<Segments> {
        // 1. Let internalSlotsList be « [[SegmentsSegmenter]], [[SegmentsString]] ».
        // 2. Let segments be OrdinaryObjectCreate(%IntlSegmentsPrototype%, internalSlotsList).
        // 3. Set segments.[[SegmentsSegmenter]] to segmenter.
        // 4. Set segments.[[SegmentsString]] to string.
        // 5. Return segments.
        let segments_string = string.utf16_string();
        segmenter.set_segmented_text(Utf16View::of_string(&segments_string));
        realm.create_object(
            vm,
            Segments {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.intrinsics().intl_segments_prototype(),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
                segments_segmenter: GcRefCell::new(Some(segmenter)),
                segments_string_value: Cell::new(string),
                segments_string,
            },
        )
    }

    pub fn segments_segmenter(&self) -> &GcRefCell<Option<unicode::Segmenter>> {
        &self.segments_segmenter
    }

    /// A copy of the ICU segmenter, as Unicode::Segmenter::clone() makes it.
    pub fn clone_segments_segmenter(&self) -> unicode::Segmenter {
        self.segments_segmenter
            .borrow()
            .as_ref()
            .expect("a Segments object has an ICU segmenter")
            .clone_segmenter()
    }

    pub fn segments_primitive_string(&self) -> Gc<PrimitiveString> {
        self.segments_string_value.get()
    }

    pub fn segments_string(&self) -> Utf16String {
        self.segments_string.clone()
    }
}
