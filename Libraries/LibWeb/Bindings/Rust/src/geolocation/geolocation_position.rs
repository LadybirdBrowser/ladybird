/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! GeolocationPosition, https://w3c.github.io/geolocation/#position_interface

use core::ffi::c_void;

use libjs_rust::api::{Trace, Vm};

use crate::bindings::interface::{Ref, Wrappable, allocate, cell_from_abi, cell_into_abi};
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
}

/// A new GeolocationPosition.
///
/// # Safety
///
/// `vm` must be the live VM, and `coords` the live cell of a GeolocationCoordinates.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_geolocation_create_geolocation_position(
    vm: *mut c_void,
    coords: *mut c_void,
    timestamp: u64,
    is_high_accuracy: bool,
) -> *mut c_void {
    // SAFETY: The caller passes the live VM and a live cell.
    let (vm, coords) = unsafe { (&*vm.cast::<Vm>(), cell_from_abi::<GeolocationCoordinates>(coords)) };
    cell_into_abi(GeolocationPosition::create(vm, coords, timestamp, is_high_accuracy))
}

/// # Safety
///
/// `cell` must be the live cell of a GeolocationPosition.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_geolocation_geolocation_position_timestamp(cell: *mut c_void) -> u64 {
    // SAFETY: The caller passes a live cell.
    unsafe { cell_from_abi::<GeolocationPosition>(cell) }.timestamp
}

/// # Safety
///
/// `cell` must be the live cell of a GeolocationPosition.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_geolocation_geolocation_position_is_high_accuracy(cell: *mut c_void) -> bool {
    // SAFETY: The caller passes a live cell.
    unsafe { cell_from_abi::<GeolocationPosition>(cell) }.is_high_accuracy
}
