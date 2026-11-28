/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C ABI that LibWebView's C++ SecurityKeySession runs a session through, on a thread of its own.

use super::ctap::{
    Assertion, Attestation, GetAssertionRequest, MakeCredentialRequest, Request, ResidentKeyRequirement,
    SecurityKeyError, UserVerificationRequirement,
};
use super::session::{Outcome, Pin, Session};
use super::{DeviceError, HidDevice, Host, REPORT_SIZE, Report, Secret};
use crate::ffi_bytes::FfiBytes;
use crate::rust_panic::abort_on_panic;
use std::collections::HashSet;
use std::ffi::c_void;
use std::time::{Duration, Instant};

/// # Safety
/// `items` points to `count` byte strings, or `count` is zero.
unsafe fn byte_strings(items: *const FfiBytes, count: usize) -> Vec<Vec<u8>> {
    // SAFETY: The caller says so.
    unsafe { ak::slice_from_raw_parts(items, count) }
        .iter()
        // SAFETY: The caller says so.
        .map(|item| unsafe { item.as_slice() }.to_vec())
        .collect()
}

pub type FfiAddDevice = unsafe extern "C" fn(devices: *mut c_void, device: *mut c_void, id: u64);

/// How a session reaches the devices of the machine. Every function is called on the thread of the session.
#[repr(C)]
pub struct FfiHost {
    pub context: *mut c_void,
    /// Opens the connected devices that are not among the known ones, and gives each to `add_device` with `devices`.
    pub open_new_devices: unsafe extern "C" fn(
        context: *mut c_void,
        known_device_ids: *const u64,
        known_device_count: usize,
        devices: *mut c_void,
        add_device: FfiAddDevice,
    ),
    /// Sends one output report of REPORT_SIZE bytes, and says whether the device took it.
    pub write_report: unsafe extern "C" fn(device: *mut c_void, report: *const u8) -> bool,
    /// Gives 1 with an input report of REPORT_SIZE bytes in `report`, 0 when none came in the time, and -1 when the
    /// device is gone.
    pub read_report: unsafe extern "C" fn(device: *mut c_void, timeout_in_milliseconds: u32, report: *mut u8) -> i32,
    pub close_device: unsafe extern "C" fn(device: *mut c_void),
    pub is_cancelled: unsafe extern "C" fn(context: *mut c_void) -> bool,
}

struct FfiDevice {
    device: *mut c_void,
    id: u64,
    write_report: unsafe extern "C" fn(device: *mut c_void, report: *const u8) -> bool,
    read_report: unsafe extern "C" fn(device: *mut c_void, timeout_in_milliseconds: u32, report: *mut u8) -> i32,
    close_device: unsafe extern "C" fn(device: *mut c_void),
}

impl HidDevice for FfiDevice {
    fn id(&self) -> u64 {
        self.id
    }

    fn write_report(&mut self, report: &Report) -> Result<(), DeviceError> {
        // SAFETY: The device is open until this is dropped, and the report has REPORT_SIZE bytes.
        if unsafe { (self.write_report)(self.device, report.as_ptr()) } {
            Ok(())
        } else {
            Err(DeviceError)
        }
    }

    fn read_report(&mut self, timeout_in_milliseconds: u32) -> Result<Option<Report>, DeviceError> {
        let mut report = [0; REPORT_SIZE];
        // SAFETY: The device is open until this is dropped, and the report has room for REPORT_SIZE bytes.
        match unsafe { (self.read_report)(self.device, timeout_in_milliseconds, report.as_mut_ptr()) } {
            1 => Ok(Some(report)),
            0 => Ok(None),
            _ => Err(DeviceError),
        }
    }
}

impl Drop for FfiDevice {
    fn drop(&mut self) {
        // SAFETY: The device came from `open_new_devices` and is not used again.
        unsafe { (self.close_device)(self.device) };
    }
}

struct FfiHostAdapter<'a> {
    host: &'a FfiHost,
    epoch: Instant,
}

unsafe extern "C" fn add_device(devices: *mut c_void, device: *mut c_void, id: u64) {
    // SAFETY: `devices` is the list that `open_new_devices()` below gave for the call.
    let devices = unsafe { &mut *devices.cast::<Vec<(*mut c_void, u64)>>() };
    devices.push((device, id));
}

impl Host for FfiHostAdapter<'_> {
    fn open_new_devices(&mut self, known_device_ids: &HashSet<u64>) -> Vec<Box<dyn HidDevice>> {
        let known_device_ids: Vec<u64> = known_device_ids.iter().copied().collect();
        let mut devices: Vec<(*mut c_void, u64)> = Vec::new();
        // SAFETY: The ids and the list live for the call, and `add_device` takes that list.
        unsafe {
            (self.host.open_new_devices)(
                self.host.context,
                known_device_ids.as_ptr(),
                known_device_ids.len(),
                (&raw mut devices).cast(),
                add_device,
            );
        }
        let host = self.host;
        devices
            .into_iter()
            .map(|(device, id)| {
                Box::new(FfiDevice {
                    device,
                    id,
                    write_report: host.write_report,
                    read_report: host.read_report,
                    close_device: host.close_device,
                }) as Box<dyn HidDevice>
            })
            .collect()
    }

    fn now_in_milliseconds(&mut self) -> u64 {
        u64::try_from(self.epoch.elapsed().as_millis()).unwrap_or(u64::MAX)
    }

    fn sleep(&mut self, milliseconds: u32) {
        std::thread::sleep(Duration::from_millis(u64::from(milliseconds)));
    }

    fn is_cancelled(&mut self) -> bool {
        // SAFETY: The context is that of the host.
        unsafe { (self.host.is_cancelled)(self.host.context) }
    }

    fn fill_with_random(&mut self, bytes: &mut [u8]) {
        libcrypto_rust::fill_with_random(bytes).expect("the system gives random bytes");
    }
}

/// The PIN of a security key, when the user gave one: `bytes` is empty otherwise.
#[repr(C)]
pub struct FfiPin {
    pub bytes: FfiBytes,
    /// The key that the PIN is for. No other key gets the PIN.
    pub device_id: u64,
}

impl FfiPin {
    /// # Safety
    /// `bytes` points to `bytes.length` bytes.
    unsafe fn to_pin(&self) -> Option<Pin> {
        // SAFETY: The caller says so.
        let bytes = unsafe { self.bytes.as_slice() };
        (!bytes.is_empty()).then(|| Pin {
            bytes: Secret::new(bytes.to_vec()),
            device_id: self.device_id,
        })
    }
}

/// NB: The text is UTF-8.
#[repr(C)]
pub struct FfiMakeCredentialRequest {
    pub client_data_hash: FfiBytes,
    pub rp_id: FfiBytes,
    pub rp_name: FfiBytes,
    pub user_id: FfiBytes,
    pub user_name: FfiBytes,
    pub user_display_name: FfiBytes,
    pub public_key_algorithms: *const i32,
    pub public_key_algorithm_count: usize,
    pub exclude_credential_ids: *const FfiBytes,
    pub exclude_credential_id_count: usize,
    pub resident_key: ResidentKeyRequirement,
    pub user_verification: UserVerificationRequirement,
}

#[repr(C)]
pub struct FfiGetAssertionRequest {
    pub client_data_hash: FfiBytes,
    pub rp_id: FfiBytes,
    pub allow_credential_ids: *const FfiBytes,
    pub allow_credential_id_count: usize,
    pub user_verification: UserVerificationRequirement,
}

/// NB: The bytes live for the call that gives this.
#[repr(C)]
pub struct FfiAttestation {
    pub credential_id: FfiBytes,
    pub authenticator_data: FfiBytes,
    pub attestation_object: FfiBytes,
    pub has_public_key: bool,
    pub public_key: FfiBytes,
    pub public_key_algorithm: i32,
    pub is_resident_key: bool,
}

/// NB: The bytes live for the call that gives this.
#[repr(C)]
pub struct FfiAssertion {
    pub credential_id: FfiBytes,
    pub authenticator_data: FfiBytes,
    pub signature: FfiBytes,
    pub has_user_handle: bool,
    pub user_handle: FfiBytes,
    pub user_name: FfiBytes,
    pub user_display_name: FfiBytes,
}

#[repr(u8)]
pub enum FfiOutcomeKind {
    /// The attestation, or each assertion, went to the callback.
    Success,
    Failed,
    Cancelled,
}

#[repr(C)]
pub struct FfiOutcome {
    pub kind: FfiOutcomeKind,
    /// Why a session that failed gave no credential.
    pub error: SecurityKeyError,
    /// Whether the user chose a key that needs its PIN or did not take the PIN.
    pub has_device_that_wants_pin: bool,
    pub device_that_wants_pin: u64,
}

impl FfiOutcome {
    fn of(kind: FfiOutcomeKind) -> Self {
        Self {
            kind,
            error: SecurityKeyError::Failed,
            has_device_that_wants_pin: false,
            device_that_wants_pin: 0,
        }
    }
}

fn run_session(
    host: &FfiHost,
    request: &Request,
    pin: Option<&Pin>,
    on_attestation: impl FnOnce(&Attestation),
    mut on_assertion: impl FnMut(&Assertion),
) -> FfiOutcome {
    let mut host = FfiHostAdapter {
        host,
        epoch: Instant::now(),
    };
    match Session::new(request, pin, &mut host).run() {
        Outcome::Attestation(attestation) => {
            on_attestation(&attestation);
            FfiOutcome::of(FfiOutcomeKind::Success)
        }
        Outcome::Assertions(assertions) => {
            assertions.iter().for_each(&mut on_assertion);
            FfiOutcome::of(FfiOutcomeKind::Success)
        }
        Outcome::Failed {
            error,
            device_that_wants_pin,
        } => FfiOutcome {
            kind: FfiOutcomeKind::Failed,
            error,
            has_device_that_wants_pin: device_that_wants_pin.is_some(),
            device_that_wants_pin: device_that_wants_pin.unwrap_or(0),
        },
        Outcome::Cancelled => FfiOutcome::of(FfiOutcomeKind::Cancelled),
    }
}

/// Asks the security keys to make a credential, and waits for the user to touch one. This blocks until then.
///
/// # Safety
/// Every pointer of `request` and `pin` points to as many items as the count or length beside it says. The functions
/// of `host` can be called with its context and its devices until this returns. `on_attestation` takes `context`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn webview_security_key_make_credential(
    host: &FfiHost,
    request: &FfiMakeCredentialRequest,
    pin: &FfiPin,
    context: *mut c_void,
    on_attestation: unsafe extern "C" fn(context: *mut c_void, attestation: &FfiAttestation),
) -> FfiOutcome {
    abort_on_panic(|| {
        // SAFETY: The caller says so.
        let (request, pin) = unsafe {
            let public_key_algorithms =
                ak::slice_from_raw_parts(request.public_key_algorithms, request.public_key_algorithm_count).to_vec();
            let request = Request::MakeCredential(MakeCredentialRequest {
                client_data_hash: request.client_data_hash.as_slice().to_vec(),
                rp_id: request.rp_id.to_string_lossy(),
                rp_name: request.rp_name.to_string_lossy(),
                user_id: request.user_id.as_slice().to_vec(),
                user_name: request.user_name.to_string_lossy(),
                user_display_name: request.user_display_name.to_string_lossy(),
                public_key_algorithms,
                exclude_credential_ids: byte_strings(
                    request.exclude_credential_ids,
                    request.exclude_credential_id_count,
                ),
                resident_key: request.resident_key,
                user_verification: request.user_verification,
            });
            (request, pin.to_pin())
        };
        run_session(
            host,
            &request,
            pin.as_ref(),
            |attestation| {
                let public_key = attestation.public_key.as_deref().unwrap_or_default();
                let attestation = FfiAttestation {
                    credential_id: FfiBytes::from_slice(&attestation.credential_id),
                    authenticator_data: FfiBytes::from_slice(&attestation.authenticator_data),
                    attestation_object: FfiBytes::from_slice(&attestation.attestation_object),
                    has_public_key: attestation.public_key.is_some(),
                    public_key: FfiBytes::from_slice(public_key),
                    public_key_algorithm: attestation.public_key_algorithm,
                    is_resident_key: attestation.is_resident_key,
                };
                // SAFETY: The callback takes the context, and the bytes live for the call.
                unsafe { on_attestation(context, &attestation) };
            },
            |_| {},
        )
    })
}

/// Asks the security keys for an assertion, and waits for the user to touch one. This blocks until then. Each
/// assertion of the key that the user chose goes to `on_assertion`.
///
/// # Safety
/// As for `webview_security_key_make_credential()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn webview_security_key_get_assertion(
    host: &FfiHost,
    request: &FfiGetAssertionRequest,
    pin: &FfiPin,
    context: *mut c_void,
    on_assertion: unsafe extern "C" fn(context: *mut c_void, assertion: &FfiAssertion),
) -> FfiOutcome {
    abort_on_panic(|| {
        // SAFETY: The caller says so.
        let (request, pin) = unsafe {
            let request = Request::GetAssertion(GetAssertionRequest {
                client_data_hash: request.client_data_hash.as_slice().to_vec(),
                rp_id: request.rp_id.to_string_lossy(),
                allow_credential_ids: byte_strings(request.allow_credential_ids, request.allow_credential_id_count),
                user_verification: request.user_verification,
            });
            (request, pin.to_pin())
        };
        run_session(
            host,
            &request,
            pin.as_ref(),
            |_| {},
            |assertion| {
                let user_handle = assertion.user_handle.as_deref().unwrap_or_default();
                let assertion = FfiAssertion {
                    credential_id: FfiBytes::from_slice(&assertion.credential_id),
                    authenticator_data: FfiBytes::from_slice(&assertion.authenticator_data),
                    signature: FfiBytes::from_slice(&assertion.signature),
                    has_user_handle: assertion.user_handle.is_some(),
                    user_handle: FfiBytes::from_slice(user_handle),
                    user_name: FfiBytes::from_slice(assertion.user_name.as_bytes()),
                    user_display_name: FfiBytes::from_slice(assertion.user_display_name.as_bytes()),
                };
                // SAFETY: The callback takes the context, and the bytes live for the call.
                unsafe { on_assertion(context, &assertion) };
            },
        )
    })
}
