/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! PerformanceObserverEntryList, https://w3c.github.io/performance-timeline/#performanceobserverentrylist-interface

use ak::Utf16String;
use libjs_rust::api::{Gc, MarkedVec, Realm, Trace, Vm};

use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use crate::performance_timeline::performance_entry::{AnyPerformanceEntry, filter_buffer_by_name_and_type};

// https://w3c.github.io/performance-timeline/#performanceobserverentrylist-interface
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct PerformanceObserverEntryList {
    realm: Gc<Realm>,
    // https://w3c.github.io/performance-timeline/#dfn-entry-list
    entry_list: Vec<AnyPerformanceEntry>,
}

impl Wrappable for PerformanceObserverEntryList {
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        Some(self.realm)
    }
}

impl PerformanceObserverEntryList {
    pub(crate) fn create(vm: &Vm, realm: Gc<Realm>, entry_list: &MarkedVec<'_, AnyPerformanceEntry>) -> Ref<Self> {
        allocate(vm, |_| Self {
            realm,
            entry_list: entry_list.to_vec(),
        })
    }

    // https://w3c.github.io/performance-timeline/#dom-performanceobserverentrylist-getentries
    pub(crate) fn get_entries<'vm>(
        &self,
        cx: &mut CallContext<'vm>,
    ) -> WebIdlResult<MarkedVec<'vm, AnyPerformanceEntry>> {
        // Returns a PerformanceEntryList object returned by filter buffer by name and type algorithm with this's entry list,
        // name and type set to null.
        let entries = filter_buffer_by_name_and_type(cx, &self.entry_list, None, None);
        Ok(entries)
    }

    // https://w3c.github.io/performance-timeline/#dom-performanceobserverentrylist-getentriesbytype
    pub(crate) fn get_entries_by_type<'vm>(
        &self,
        cx: &mut CallContext<'vm>,
        r#type: Utf16String,
    ) -> WebIdlResult<MarkedVec<'vm, AnyPerformanceEntry>> {
        // Returns a PerformanceEntryList object returned by filter buffer by name and type algorithm with this's entry list,
        // name set to null, and type set to the method's input type parameter.
        let entries = filter_buffer_by_name_and_type(cx, &self.entry_list, None, Some(&r#type));
        Ok(entries)
    }

    // https://w3c.github.io/performance-timeline/#dom-performanceobserverentrylist-getentriesbyname
    pub(crate) fn get_entries_by_name<'vm>(
        &self,
        cx: &mut CallContext<'vm>,
        name: Utf16String,
        r#type: Option<Utf16String>,
    ) -> WebIdlResult<MarkedVec<'vm, AnyPerformanceEntry>> {
        // Returns a PerformanceEntryList object returned by filter buffer by name and type algorithm with this's entry list,
        // name set to the method input name parameter, and type set to null if optional entryType is omitted, or set to the
        // method's input type parameter otherwise.
        let entries = filter_buffer_by_name_and_type(cx, &self.entry_list, Some(&name), r#type.as_ref());
        Ok(entries)
    }
}
