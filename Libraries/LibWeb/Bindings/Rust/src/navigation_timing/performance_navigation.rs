/*
 * Copyright (c) 2024, Colin Reeder <colin@vpzom.click>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! PerformanceNavigation, https://w3c.github.io/navigation-timing/#the-performancenavigation-interface

use libjs_rust::api::Trace;

use crate::bindings::global_object::CppWindow;
use crate::bindings::interface::{Ref, Wrappable};
use crate::bindings::{CallContext, WebIdlResult};
use crate::navigation_timing::performance_navigation_timing::{NavigationTimingType, PerformanceNavigationTiming};

const TYPE_NAVIGATE: u16 = 0;
const TYPE_RELOAD: u16 = 1;
const TYPE_BACK_FORWARD: u16 = 2;

/// https://w3c.github.io/navigation-timing/#the-performancenavigation-interface
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct PerformanceNavigation {
    window: CppWindow,
}

impl Wrappable for PerformanceNavigation {}

impl PerformanceNavigation {
    pub(crate) fn new(window: CppWindow) -> Self {
        Self { window }
    }

    fn navigation_timing_entry(&self) -> Option<Ref<PerformanceNavigationTiming>> {
        self.window.associated_document().navigation_timing_entry()
    }

    // https://w3c.github.io/navigation-timing/#dom-performancenavigation-type
    pub(crate) fn r#type(&self, cx: &mut CallContext) -> WebIdlResult<u16> {
        // This attribute must return the type of the last non-redirect navigation. It must have one of the following
        // navigation type values.
        let Some(entry) = self.navigation_timing_entry() else {
            return Ok(TYPE_NAVIGATE);
        };

        Ok(match entry.r#type(cx)? {
            NavigationTimingType::Navigate => TYPE_NAVIGATE,
            NavigationTimingType::Reload => TYPE_RELOAD,
            NavigationTimingType::BackForward => TYPE_BACK_FORWARD,
        })
    }

    // https://w3c.github.io/navigation-timing/#dom-performancenavigation-redirectcount
    pub(crate) fn redirect_count(&self, cx: &mut CallContext) -> WebIdlResult<u16> {
        // This attribute must return the number of redirects since the last non-redirect navigation. If there is no
        // redirect or there is any redirect that is not from the same origin as the destination document, this attribute
        // must return zero.
        let Some(entry) = self.navigation_timing_entry() else {
            return Ok(0);
        };

        entry.redirect_count(cx)
    }
}
