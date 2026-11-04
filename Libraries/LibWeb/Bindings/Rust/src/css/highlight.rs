/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Highlight, https://drafts.csswg.org/css-highlight-api-1/#highlight

use core::cell::{Cell, RefCell};
use core::ffi::c_void;
use std::collections::HashSet;

use libjs_rust::api::{Gc, GcRefCell, MarkedVec, Object, Set, Trace, Value};

use super::css_rule::wrap_cpp_cell;
use crate::bindings::conversions::CppInterface;
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::setlike::{Setlike, WorldEntries, WritableSetlike};
use crate::bindings::{CallContext, WebIdlResult};

unsafe extern "C" {
    fn web_css_abstract_range_of_wrapper(wrapper: *mut c_void) -> *mut c_void;
}

/// The enumeration HighlightType of CSS/Highlight.idl.
#[derive(Clone, Copy, Eq, PartialEq)]
pub(crate) enum HighlightType {
    Highlight,
    SpellingError,
    GrammarError,
}

/// The AbstractRange that a JS value of the value type of the setlike declaration wraps.
fn range_of_value(value: Value) -> CppCell {
    // SAFETY: The bindings checked that the value is a wrapper of an AbstractRange.
    unsafe { CppCell::from_raw(web_css_abstract_range_of_wrapper(value.as_object().as_ptr().cast())) }
}

fn range_wrapper(cx: &CallContext, range: &CppCell) -> Value {
    Value::from_object(wrap_cpp_cell(cx, range).wrapper())
}

/// The call context of the realm that a collection of a wrapper world lives in.
fn context_of_set<'vm>(cx: &CallContext<'vm>, set: Gc<Set>) -> CallContext<'vm> {
    CallContext::new(cx.vm(), set.upcast::<Object>().shape().realm())
}

// https://drafts.csswg.org/css-highlight-api-1/#highlight
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Highlight {
    /// The set entries of this setlike object, in insertion order.
    ranges: GcRefCell<Vec<CppCell>>,
    /// The addresses of the ranges, to find one without a walk over all of them.
    #[gc(untraced)]
    range_addresses: RefCell<HashSet<usize>>,
    #[gc(untraced)]
    priority: Cell<i32>,
    #[gc(untraced)]
    highlight_type: Cell<HighlightType>,
    set_entries: WorldEntries<Set>,
}

impl Wrappable for Highlight {}

impl Highlight {
    // https://drafts.csswg.org/css-highlight-api-1/#dom-highlight-highlight
    pub(crate) fn construct(
        cx: &mut CallContext,
        initial_ranges: MarkedVec<'_, CppInterface>,
    ) -> WebIdlResult<Ref<Self>> {
        // 1. Let highlight be the new Highlight object.
        // 2. Set highlight’s priority to 0.
        // 3. Set highlight’s type to highlight.
        let highlight = allocate(cx.vm(), |_| Self {
            ranges: GcRefCell::new(Vec::new()),
            range_addresses: RefCell::new(HashSet::new()),
            priority: Cell::new(0),
            highlight_type: Cell::new(HighlightType::Highlight),
            set_entries: WorldEntries::default(),
        });

        // 4. For each range of initialRanges, let rangeArg be the result of converting range to an ECMAScript value,
        //    then run the steps for a built-in setlike add function, with highlight as the this value, and rangeArg as
        //    the argument.
        // NB: highlight has no wrapper yet, so there are no set entries in any world to update.
        for range in initial_ranges.to_vec() {
            highlight.add_range(cx, range_of_value(Value::from_object(range.wrapper())));
        }

        // 5. Return highlight.
        Ok(highlight)
    }

    pub(crate) fn priority(&self, _: &mut CallContext) -> WebIdlResult<i32> {
        Ok(self.priority.get())
    }

    pub(crate) fn set_priority(&self, _: &mut CallContext, priority: i32) -> WebIdlResult<()> {
        self.priority.set(priority);
        Ok(())
    }

    pub(crate) fn r#type(&self, _: &mut CallContext) -> WebIdlResult<HighlightType> {
        Ok(self.highlight_type.get())
    }

    pub(crate) fn set_type(&self, _: &mut CallContext, highlight_type: HighlightType) -> WebIdlResult<()> {
        self.highlight_type.set(highlight_type);
        Ok(())
    }

    /// The priority that sorts the results of highlightsFromPoint().
    pub(crate) fn current_priority(&self) -> i32 {
        self.priority.get()
    }

    /// The ranges of the highlight, in insertion order.
    pub(crate) fn ranges(&self) -> Vec<CppCell> {
        self.ranges.borrow().clone()
    }

    fn add_range(&self, cx: &CallContext, range: CppCell) -> bool {
        if !self.range_addresses.borrow_mut().insert(range.as_ptr() as usize) {
            return false;
        }
        self.ranges.borrow_mut(cx.vm()).push(range);
        true
    }
}

impl Setlike for Highlight {
    fn set_entries(&self, cx: &CallContext) -> Gc<Set> {
        self.set_entries.get_or_create(cx, || {
            let set = Set::create(cx.vm(), cx.realm());
            for range in self.ranges() {
                set.set_add(cx.vm(), range_wrapper(cx, &range));
            }
            set
        })
    }
}

impl WritableSetlike for Highlight {
    fn set_add(&self, cx: &CallContext, value: Value) {
        let range = range_of_value(value);
        if !self.add_range(cx, range.clone()) {
            return;
        }
        for set in self.set_entries.live() {
            set.set_add(cx.vm(), range_wrapper(&context_of_set(cx, set), &range));
        }
        // FIXME: https://drafts.csswg.org/css-highlight-api-1/#repaint
        //        The addition or removal of a range in a registered custom highlight must cause the user agent to
        //        reevaluate the rendering, and to repaint if appropriate. We do not paint custom highlights yet.
    }

    fn set_remove(&self, cx: &CallContext, value: Value) -> bool {
        let range = range_of_value(value);
        if !self.range_addresses.borrow_mut().remove(&(range.as_ptr() as usize)) {
            return false;
        }
        self.ranges
            .borrow_mut(cx.vm())
            .retain(|existing| existing.as_ptr() != range.as_ptr());
        for set in self.set_entries.live() {
            set.set_remove(cx.vm(), range_wrapper(&context_of_set(cx, set), &range));
        }
        true
    }

    fn set_clear(&self, cx: &CallContext) {
        self.range_addresses.borrow_mut().clear();
        self.ranges.borrow_mut(cx.vm()).clear();
        for set in self.set_entries.live() {
            set.set_clear(cx.vm());
        }
    }
}
