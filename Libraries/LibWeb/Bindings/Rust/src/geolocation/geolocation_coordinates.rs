/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! GeolocationCoordinates, https://w3c.github.io/geolocation/#coordinates_interface

use core::ffi::c_void;

use libjs_rust::api::{Trace, Vm};

use crate::bindings::interface::{Ref, Wrappable, allocate, cell_into_abi};
use crate::bindings::{CallContext, WebIdlResult};

/// Web::Geolocation::CoordinatesData.
#[repr(C)]
pub(crate) struct CoordinatesData {
    accuracy: f64,
    latitude: f64,
    longitude: f64,
    has_altitude: bool,
    altitude: f64,
    has_altitude_accuracy: bool,
    altitude_accuracy: f64,
    has_heading: bool,
    heading: f64,
    has_speed: bool,
    speed: f64,
}

// https://w3c.github.io/geolocation/#coordinates_interface
#[derive(Trace)]
pub(crate) struct GeolocationCoordinates {
    #[gc(untraced)]
    accuracy: f64,
    #[gc(untraced)]
    latitude: f64,
    #[gc(untraced)]
    longitude: f64,
    #[gc(untraced)]
    altitude: Option<f64>,
    #[gc(untraced)]
    altitude_accuracy: Option<f64>,
    #[gc(untraced)]
    heading: Option<f64>,
    #[gc(untraced)]
    speed: Option<f64>,
}

impl Wrappable for GeolocationCoordinates {}

impl GeolocationCoordinates {
    pub(crate) fn create(vm: &Vm, data: &CoordinatesData) -> Ref<Self> {
        let optional = |has_value: bool, value: f64| has_value.then_some(value);
        allocate(vm, |_| Self {
            accuracy: data.accuracy,
            latitude: data.latitude,
            longitude: data.longitude,
            altitude: optional(data.has_altitude, data.altitude),
            altitude_accuracy: optional(data.has_altitude_accuracy, data.altitude_accuracy),
            heading: optional(data.has_heading, data.heading),
            speed: optional(data.has_speed, data.speed),
        })
    }

    pub(crate) fn accuracy(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.accuracy)
    }

    pub(crate) fn latitude(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.latitude)
    }

    pub(crate) fn longitude(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.longitude)
    }

    pub(crate) fn altitude(&self, _: &mut CallContext) -> WebIdlResult<Option<f64>> {
        Ok(self.altitude)
    }

    pub(crate) fn altitude_accuracy(&self, _: &mut CallContext) -> WebIdlResult<Option<f64>> {
        Ok(self.altitude_accuracy)
    }

    pub(crate) fn heading(&self, _: &mut CallContext) -> WebIdlResult<Option<f64>> {
        Ok(self.heading)
    }

    pub(crate) fn speed(&self, _: &mut CallContext) -> WebIdlResult<Option<f64>> {
        Ok(self.speed)
    }
}

/// A new GeolocationCoordinates with the data.
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_geolocation_create_geolocation_coordinates(vm: &Vm, data: &CoordinatesData) -> *mut c_void {
    cell_into_abi(GeolocationCoordinates::create(vm, data))
}
