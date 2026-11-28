/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The client side of CTAP2 over USB HID, for the security keys that are connected to the machine,
//! <https://fidoalliance.org/specs/fido-v2.2-ps-20250714/fido-client-to-authenticator-protocol-v2.2-ps-20250714.html>.
//! A device is not trusted: it can be a key of someone else, or no key at all.

pub mod after_error;
pub mod ctap;
pub mod ctaphid;
pub mod ffi;
pub mod pin_protocol;
pub mod session;

#[cfg(test)]
mod fake_key;
#[cfg(test)]
mod tests;

use std::collections::HashSet;

/// Full speed devices, which security keys are, have reports of 64 bytes.
pub const REPORT_SIZE: usize = 64;

pub type Report = [u8; REPORT_SIZE];

/// The device is gone, or the platform could not reach it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DeviceError;

/// A USB HID device with the FIDO usage page, which is how a security key shows itself to the host.
pub trait HidDevice {
    /// What tells the device from the others for as long as it stays connected.
    fn id(&self) -> u64;
    /// Sends one output report.
    fn write_report(&mut self, report: &Report) -> Result<(), DeviceError>;
    /// The next input report, or nothing when none came in the time.
    fn read_report(&mut self, timeout_in_milliseconds: u32) -> Result<Option<Report>, DeviceError>;
}

/// What a session needs from the machine that it runs on.
pub trait Host {
    /// Opens the connected devices that are not among the known ones.
    fn open_new_devices(&mut self, known_device_ids: &HashSet<u64>) -> Vec<Box<dyn HidDevice>>;
    /// Milliseconds on a clock that only goes forward.
    fn now_in_milliseconds(&mut self) -> u64;
    fn sleep(&mut self, milliseconds: u32);
    /// Whether whoever started the session no longer wants its result.
    fn is_cancelled(&mut self) -> bool;
    fn fill_with_random(&mut self, bytes: &mut [u8]);
}

/// Bytes that are cleared when they are dropped, such as a PIN or a token that a key gave for it.
#[derive(Default)]
pub struct Secret(Vec<u8>);

impl Secret {
    pub fn new(bytes: Vec<u8>) -> Self {
        Self(bytes)
    }

    pub fn is_empty(&self) -> bool {
        self.0.is_empty()
    }
}

impl core::ops::Deref for Secret {
    type Target = [u8];

    fn deref(&self) -> &[u8] {
        &self.0
    }
}

impl Drop for Secret {
    fn drop(&mut self) {
        libcrypto_rust::clear_secret(&mut self.0);
    }
}
