/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! HighlightRegistry, https://drafts.csswg.org/css-highlight-api-1/#highlightregistry

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{Gc, GcRefCell, Map, MarkedVec, Trace, Value, Vm};

use super::css_rule::wrap_cpp_cell;
use super::highlight::Highlight;
use crate::bindings::conversions::{CppInterface, ToJs};
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::interface::{Ref, Wrappable, allocate, implementation_of, wrap};
use crate::bindings::maplike::context_of_map;
use crate::bindings::maplike::{Maplike, WritableMaplike};
use crate::bindings::setlike::WorldEntries;
use crate::bindings::{CallContext, WebIdlResult};

unsafe extern "C" {
    fn web_css_highlight_registry_point_is_in_viewport(document: *mut c_void, x: f32, y: f32) -> bool;
    fn web_css_highlight_registry_shadow_root_at_point(document: *mut c_void, x: f32, y: f32) -> *mut c_void;
    fn web_css_highlight_registry_range_is_hit(range: *mut c_void, x: f32, y: f32) -> bool;
    fn web_css_shadow_root_of_wrapper(wrapper: *mut c_void) -> *mut c_void;
}

/// The dictionary HighlightsFromPointOptions of CSS/HighlightRegistry.idl.
pub(crate) struct HighlightsFromPointOptions<'vm> {
    pub(crate) shadow_roots: MarkedVec<'vm, CppInterface>,
}

/// The dictionary HighlightHitResult of CSS/HighlightRegistry.idl.
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct HighlightHitResult {
    pub(crate) highlight: Option<Ref<Highlight>>,
    pub(crate) ranges: Option<Vec<CppInterface>>,
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
struct RegisteredHighlight {
    #[gc(untraced)]
    name: Utf16String,
    highlight: Ref<Highlight>,
}

// https://drafts.csswg.org/css-highlight-api-1/#highlightregistry
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct HighlightRegistry {
    document: CppCell,
    /// The map entries of this maplike object, in registration order.
    highlights: GcRefCell<Vec<RegisteredHighlight>>,
    map_entries: WorldEntries<Map>,
}

impl Wrappable for HighlightRegistry {}

fn set_map_entry(cx: &CallContext, map: Gc<Map>, name: &Utf16String, highlight: Ref<Highlight>) {
    let key = name.clone().to_js(cx).expect("a string converts to a JS value");
    map.map_set(cx.vm(), key, Value::from_object(wrap(cx, highlight)));
}

impl HighlightRegistry {
    fn registered_highlights<'vm>(&self, vm: &'vm Vm) -> MarkedVec<'vm, RegisteredHighlight> {
        let highlights = MarkedVec::new(vm);
        highlights.extend_from_slice(&self.highlights.borrow());
        highlights
    }

    pub(crate) fn create(vm: &Vm, document: CppCell) -> Ref<Self> {
        allocate(vm, |_| Self {
            document,
            highlights: GcRefCell::new(Vec::new()),
            map_entries: WorldEntries::default(),
        })
    }

    // https://drafts.csswg.org/css-highlight-api-1/#dom-highlightregistry-highlightsfrompoint
    pub(crate) fn highlights_from_point<'vm>(
        &self,
        cx: &mut CallContext<'vm>,
        x: f32,
        y: f32,
        options: HighlightsFromPointOptions<'_>,
    ) -> WebIdlResult<MarkedVec<'vm, HighlightHitResult>> {
        let vm = cx.vm();
        let results = MarkedVec::new(vm);
        let document = self.document.as_ptr();

        // 1. If any of the following are true, return the empty sequence:
        //    * x is negative
        //    * y is negative
        //    * x is greater than the viewport width excluding the size of a rendered scroll bar (if any)
        //    * y is greater than the viewport height excluding the size of a rendered scroll bar (if any)
        // SAFETY: The document is live.
        if !unsafe { web_css_highlight_registry_point_is_in_viewport(document, x, y) } {
            return Ok(results);
        }

        //    * The topmost box in the viewport in paint order that would be a target for hit testing at coordinates x,y
        //      when applying the transforms that apply to the descendants of the viewport, has an element associated to
        //      it that's in a shadow tree whose shadow root is not contained by options.shadowRoots.
        // SAFETY: The document is live.
        let shadow_root = unsafe { web_css_highlight_registry_shadow_root_at_point(document, x, y) };
        if !shadow_root.is_null() {
            let is_contained = options.shadow_roots.to_vec().iter().any(|allowed_shadow_root| {
                // SAFETY: The bindings checked that the wrapper is that of a ShadowRoot.
                unsafe { web_css_shadow_root_of_wrapper(allowed_shadow_root.wrapper().as_ptr().cast()) == shadow_root }
            });
            if !is_contained {
                return Ok(results);
            }
        }

        // 2. Otherwise, let results be an empty sequence.
        // NB: The results stay in registration order in a MarkedVec, as they hold new wrappers, with the sort keys next
        //     to them.
        let unsorted_results = MarkedVec::new(vm);
        let mut sort_keys: Vec<(i32, usize)> = Vec::new();

        // 3. For each Highlight highlight in this HighlightRegistry:
        for (registration_index, entry) in self.registered_highlights(vm).values().iter().enumerate() {
            let highlight = entry.highlight;

            // 1. Let result be a new HighlightHitResult with highlight set to highlight.
            let ranges = MarkedVec::new(vm);

            // 2. For each AbstractRange abstractRange in highlight:
            for abstract_range in highlight.ranges(vm).values().iter() {
                // 1. If abstractRange is an invalid StaticRange, then continue.
                // 2. Let range be a new Range whose start node and end node are set to abstractRange’s start node and
                //    end node respectively, and start offset and end offset are set to abstractRange’s start offset
                //    and end offset respectively.
                // 3. If the coordinates x,y fall inside at least one of the DOMRects returned by calling
                //    getClientRects() on range, then append abstractRange to result.ranges.
                // SAFETY: The range is live.
                if unsafe { web_css_highlight_registry_range_is_hit(abstract_range.as_ptr(), x, y) } {
                    ranges.push(wrap_cpp_cell(cx, abstract_range));
                }
            }

            // 3. If result.ranges is not empty, append result to results.
            if !ranges.is_empty() {
                sort_keys.push((highlight.current_priority(), registration_index));
                unsorted_results.push(HighlightHitResult {
                    highlight: Some(highlight),
                    ranges: Some(ranges.to_vec()),
                });
            }
        }

        // 4. Sort results by descending order of priority of its HighlightHitResults' highlight attributes.
        // NB: When two or more custom highlights have the same numerical priority, the one most recently registered has
        //     the higher effective priority.
        let mut order: Vec<usize> = (0..sort_keys.len()).collect();
        order.sort_by_key(|index| {
            let (priority, registration_index) = sort_keys[*index];
            (core::cmp::Reverse(priority), core::cmp::Reverse(registration_index))
        });

        // 5. Return results.
        for index in order {
            results.push(unsorted_results.get(index).expect("the index is in the results"));
        }
        Ok(results)
    }
}

impl Maplike for HighlightRegistry {
    fn map_entries(&self, cx: &CallContext) -> Gc<Map> {
        self.map_entries.get_or_create(cx, || {
            let map = Map::create(cx.vm(), cx.realm());
            for entry in self.registered_highlights(cx.vm()).values().iter() {
                set_map_entry(cx, map, &entry.name, entry.highlight);
            }
            map
        })
    }
}

impl WritableMaplike for HighlightRegistry {
    fn map_set(&self, cx: &CallContext, key: Utf16String, value: Value) {
        // FIXME: https://drafts.csswg.org/css-highlight-api-1/#repaint
        //        The addition or removal of a custom highlight in the highlight registry must cause the user agent to
        //        reevaluate the rendering, and to repaint if appropriate. We do not paint custom highlights yet.
        let highlight = implementation_of::<Highlight>(value.as_object());
        {
            let mut highlights = self.highlights.borrow_mut(cx.vm());
            match highlights.iter_mut().find(|entry| entry.name == key) {
                Some(entry) => entry.highlight = highlight,
                None => highlights.push(RegisteredHighlight {
                    name: key.clone(),
                    highlight,
                }),
            }
        }
        self.map_entries.for_each_live(cx.vm(), |map| {
            set_map_entry(&context_of_map(cx, map), map, &key, highlight);
        });
    }

    fn map_remove(&self, cx: &CallContext, key: &Utf16String) -> bool {
        let position = self.highlights.borrow().iter().position(|entry| entry.name == *key);
        let Some(position) = position else {
            return false;
        };
        self.highlights.borrow_mut(cx.vm()).remove(position);
        self.map_entries.for_each_live(cx.vm(), |map| {
            let key = key.clone().to_js(cx).expect("a string converts to a JS value");
            map.map_remove(cx.vm(), key);
        });
        true
    }

    fn map_clear(&self, cx: &CallContext) {
        self.highlights.borrow_mut(cx.vm()).clear();
        self.map_entries.for_each_live(cx.vm(), |map| {
            map.map_clear(cx.vm());
        });
    }
}
