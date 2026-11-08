/*
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Span;

#[repr(transparent)]
pub struct KeyHandle(NonNull<c_void>);

pub struct KeyPair {
    pub public_key: KeyHandle,
    pub private_key: KeyHandle,
}

impl KeyPair {
    pub(super) unsafe fn from_raw(public_key: *mut c_void, private_key: *mut c_void) -> Self {
        // SAFETY: The caller transfers ownership of both native keys.
        let (public_key, private_key) = unsafe { (KeyHandle::from_raw(public_key), KeyHandle::from_raw(private_key)) };
        Self {
            public_key: public_key.expect("LibCrypto returns a public key"),
            private_key: private_key.expect("LibCrypto returns a private key"),
        }
    }
}

#[derive(Debug)]
pub enum KeyDecodeError {
    InvalidComponents,
    OutOfMemory,
}

pub struct KeyComponents<'a> {
    pub tag: u8,
    buffers: [&'a [u8]; 8],
    buffer_count: usize,
    pub ec_parameters: Option<&'a [u32]>,
    pub has_ec_public_key: bool,
}

impl<'a> KeyComponents<'a> {
    pub fn buffers(&self) -> &[&'a [u8]] {
        &self.buffers[..self.buffer_count]
    }
}

#[repr(C)]
struct RawKeyComponents<'a> {
    tag: u8,
    buffers: [Span<'a, u8>; 8],
    buffer_count: usize,
    has_ec_parameters: bool,
    ec_parameters: Span<'a, u32>,
    has_ec_public_key: bool,
}

unsafe extern "C" {
    fn crypto_key_destroy_handle(key: *mut c_void);
    fn crypto_key_create_byte_handle(bytes: Span<'_, u8>) -> *mut c_void;
    fn crypto_key_create_okp_handle(is_private: bool, bytes: Span<'_, u8>) -> *mut c_void;
    fn crypto_key_byte_handle_bytes(key: *const c_void, bytes: &mut Span<'_, u8>);
    fn crypto_key_okp_handle_bytes(key: *const c_void, bytes: &mut Span<'_, u8>);
    fn crypto_key_encode_handle(
        key: *const c_void,
        state: *mut c_void,
        steps: unsafe extern "C" fn(*mut c_void, *const RawKeyComponents<'_>),
    );
    fn crypto_key_decode_handle(fields: &RawKeyComponents<'_>, key: &mut *mut c_void) -> bool;
}

impl KeyHandle {
    /// # Safety
    ///
    /// A non-null pointer must transfer ownership of a LibCrypto key handle.
    pub unsafe fn from_raw(key: *mut c_void) -> Option<Self> {
        NonNull::new(key).map(Self)
    }

    pub fn as_raw(&self) -> *mut c_void {
        self.0.as_ptr()
    }

    pub fn from_secret_bytes(bytes: &[u8]) -> Self {
        // SAFETY: The input is borrowed for the copy.
        let key = unsafe { crypto_key_create_byte_handle(Span::new(bytes)) };
        // SAFETY: The native call transfers ownership of the key.
        unsafe { Self::from_raw(key) }.expect("LibCrypto returns a secret key")
    }

    pub fn from_okp_bytes(is_private: bool, bytes: &[u8]) -> Self {
        // SAFETY: The input is borrowed for the copy.
        let key = unsafe { crypto_key_create_okp_handle(is_private, Span::new(bytes)) };
        // SAFETY: The native call transfers ownership of the key.
        unsafe { Self::from_raw(key) }.expect("LibCrypto returns an OKP key")
    }

    pub fn secret_bytes(&self) -> &[u8] {
        let mut bytes = Span::default();
        // SAFETY: The key owns the returned bytes.
        unsafe { crypto_key_byte_handle_bytes(self.as_raw(), &mut bytes) };
        bytes.as_slice()
    }

    pub fn okp_bytes(&self) -> &[u8] {
        let mut bytes = Span::default();
        // SAFETY: The key owns the returned bytes.
        unsafe { crypto_key_okp_handle_bytes(self.as_raw(), &mut bytes) };
        bytes.as_slice()
    }

    pub fn with_components<R>(&self, callback: impl FnOnce(KeyComponents<'_>) -> R) -> R {
        fn run<F: FnOnce(KeyComponents<'_>) -> R, R>(key: &KeyHandle, callback: F) -> R {
            unsafe extern "C" fn receive<F: FnOnce(KeyComponents<'_>) -> R, R>(
                state: *mut c_void,
                fields: *const RawKeyComponents<'_>,
            ) {
                // SAFETY: LibCrypto invokes the callback synchronously with the live slots.
                let (state, fields) = unsafe { (&mut *state.cast::<(Option<F>, Option<R>)>(), &*fields) };
                let callback = state.0.take().expect("LibCrypto supplies the components once");
                state.1 = Some(callback(KeyComponents {
                    tag: fields.tag,
                    buffers: fields.buffers.map(Span::as_slice),
                    buffer_count: fields.buffer_count,
                    ec_parameters: fields.has_ec_parameters.then(|| fields.ec_parameters.as_slice()),
                    has_ec_public_key: fields.has_ec_public_key,
                }));
            }
            let mut state: (Option<F>, Option<R>) = (Some(callback), None);
            // SAFETY: The key and callback state are live for the synchronous native call.
            unsafe { crypto_key_encode_handle(key.as_raw(), core::ptr::from_mut(&mut state).cast(), receive::<F, R>) };
            state.1.expect("LibCrypto supplies the key components")
        }
        run(self, callback)
    }

    pub fn from_components(
        tag: u8,
        buffers: &[&[u8]],
        ec_parameters: Option<&[u32]>,
        has_ec_public_key: bool,
    ) -> Result<Self, KeyDecodeError> {
        let expected = match tag {
            0 | 5 | 7 | 9 | 10 => 1,
            1 | 3 => 2,
            2 => 8,
            4 if has_ec_public_key => 3,
            4 => 1,
            6 | 8 => 3,
            _ => return Err(KeyDecodeError::InvalidComponents),
        };
        if buffers.len() != expected || (tag != 4 && (ec_parameters.is_some() || has_ec_public_key)) {
            return Err(KeyDecodeError::InvalidComponents);
        }
        let mut spans = [Span::default(); 8];
        for (span, bytes) in spans.iter_mut().zip(buffers) {
            *span = Span::new(bytes);
        }
        let fields = RawKeyComponents {
            tag,
            buffers: spans,
            buffer_count: buffers.len(),
            has_ec_parameters: ec_parameters.is_some(),
            ec_parameters: Span::new(ec_parameters.unwrap_or_default()),
            has_ec_public_key,
        };
        let mut key = core::ptr::null_mut();
        // SAFETY: The validated fields and output slot are live for the call.
        if !unsafe { crypto_key_decode_handle(&fields, &mut key) } {
            return Err(KeyDecodeError::OutOfMemory);
        }
        // SAFETY: Success transfers ownership of the native key.
        Ok(unsafe { Self::from_raw(key) }.expect("LibCrypto returns the decoded key"))
    }
}

impl Drop for KeyHandle {
    fn drop(&mut self) {
        // SAFETY: This value owns the native handle.
        unsafe { crypto_key_destroy_handle(self.as_raw()) };
    }
}
