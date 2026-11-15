/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! GeolocationPosition, https://w3c.github.io/geolocation/#position_interface

use libjs_rust::api::{Trace, Vm};

use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use crate::geolocation::geolocation_coordinates::GeolocationCoordinates;

// https://w3c.github.io/geolocation/#dom-geolocationposition
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct GeolocationPosition {
    coords: Ref<GeolocationCoordinates>,
    #[gc(untraced)]
    timestamp: u64,

    // https://w3c.github.io/geolocation/#dfn-ishighaccuracy
    #[gc(untraced)]
    is_high_accuracy: bool,
}

impl Wrappable for GeolocationPosition {}

impl GeolocationPosition {
    pub(crate) fn create(
        vm: &Vm,
        coords: Ref<GeolocationCoordinates>,
        timestamp: u64,
        is_high_accuracy: bool,
    ) -> Ref<Self> {
        allocate(vm, |_| Self {
            coords,
            timestamp,
            is_high_accuracy,
        })
    }

    pub(crate) fn coords(&self, _: &mut CallContext) -> WebIdlResult<Ref<GeolocationCoordinates>> {
        Ok(self.coords)
    }

    pub(crate) fn timestamp(&self, _: &mut CallContext) -> WebIdlResult<u64> {
        Ok(self.timestamp)
    }

    pub(crate) fn timestamp_value(&self) -> u64 {
        self.timestamp
    }

    pub(crate) fn is_high_accuracy(&self) -> bool {
        self.is_high_accuracy
    }
}
