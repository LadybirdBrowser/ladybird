/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::{Span, Utf16String};

use super::{Asn1Structure, KeyHandle, OkpCurve, ffi, result_from_error};

pub enum Asn1Value<'a> {
    KeyInfo(KeyInfo),
    OctetString(&'a [u8]),
}

pub struct KeyInfo {
    structure: Asn1Structure,
    ptr: NonNull<c_void>,
}

pub fn decode_asn1_structure(data: &[u8], structure: Asn1Structure) -> Result<(Asn1Value<'_>, bool), Utf16String> {
    // SAFETY: LibCrypto allocates storage of the selected structure.
    let raw = unsafe { ffi::crypto_asn1_allocate_key_info(structure) };
    let native = KeyInfo {
        structure,
        ptr: NonNull::new(raw).expect("LibCrypto returns the ASN.1 structure"),
    };
    let mut all_consumed = false;
    // SAFETY: The input and native storage of the selected structure live for the call.
    let status = unsafe {
        ffi::crypto_decode_asn1_structure(Span::new(data), structure, native.ptr.as_ptr(), &mut all_consumed)
    };
    result_from_error(status)?;
    let value = if matches!(structure, Asn1Structure::OctetString) {
        let mut bytes = Span::default();
        // SAFETY: This native value is an octet string that borrows its bytes from data.
        unsafe { ffi::crypto_asn1_octet_string_bytes(native.ptr.as_ptr(), &mut bytes) };
        Asn1Value::OctetString(bytes.as_slice())
    } else {
        Asn1Value::KeyInfo(native)
    };
    Ok((value, all_consumed))
}

impl KeyInfo {
    pub fn has_rsa_encryption_oid(&self) -> bool {
        // SAFETY: The native value matches its structure tag.
        unsafe { ffi::crypto_rsa_key_info_has_encryption_oid(self.structure, self.ptr.as_ptr()) }
    }

    pub fn rsa_key(&self) -> KeyHandle {
        // SAFETY: The native value matches its structure tag.
        let raw = unsafe { ffi::crypto_rsa_key_info_read_key(self.structure, self.ptr.as_ptr()) };
        // SAFETY: The native call transfers ownership of the key.
        unsafe { KeyHandle::from_raw(raw) }.expect("LibCrypto returns an RSA key")
    }

    pub fn has_ec_public_key_oid(&self) -> bool {
        // SAFETY: The native value matches its structure tag.
        unsafe { ffi::crypto_ec_key_info_has_public_key_oid(self.structure, self.ptr.as_ptr()) }
    }

    pub fn ec_parameters(&self) -> Option<&[u32]> {
        let mut parameters = Span::default();
        // SAFETY: The native value matches its structure tag and owns the parameters.
        let has_parameters =
            unsafe { ffi::crypto_ec_key_info_parameters(self.structure, self.ptr.as_ptr(), &mut parameters) };
        has_parameters.then(|| parameters.as_slice())
    }

    pub fn ec_private_parameters_match(&self) -> bool {
        assert!(matches!(self.structure, Asn1Structure::PrivateKeyInfo));
        // SAFETY: This is a native private key info value.
        unsafe { ffi::crypto_ec_private_key_info_parameters_match(self.ptr.as_ptr()) }
    }

    pub fn ec_key(&self) -> KeyHandle {
        // SAFETY: The native value matches its structure tag.
        let raw = unsafe { ffi::crypto_ec_key_info_read_key(self.structure, self.ptr.as_ptr()) };
        // SAFETY: The native call transfers ownership of the key.
        unsafe { KeyHandle::from_raw(raw) }.expect("LibCrypto returns an EC key")
    }

    pub fn has_okp_oid(&self, curve: OkpCurve) -> bool {
        // SAFETY: The native value matches its structure tag.
        unsafe { ffi::crypto_okp_key_info_has_oid(curve, self.structure, self.ptr.as_ptr()) }
    }

    pub fn raw_key(&self) -> &[u8] {
        let mut bytes = Span::default();
        // SAFETY: The native value matches its structure tag and owns the bytes.
        unsafe { ffi::crypto_asn1_key_info_raw_key(self.structure, self.ptr.as_ptr(), &mut bytes) };
        bytes.as_slice()
    }

    pub fn oid(&self) -> &[u32] {
        let mut oid = Span::default();
        // SAFETY: The native value matches its structure tag and owns the identifier.
        unsafe { ffi::crypto_asn1_key_info_identifier(self.structure, self.ptr.as_ptr(), &mut oid) };
        oid.as_slice()
    }

    pub fn mldsa_private_key(&self) -> KeyHandle {
        assert!(matches!(self.structure, Asn1Structure::PrivateKeyInfo));
        // SAFETY: This is a native private key info value.
        let raw = unsafe { ffi::crypto_mldsa_key_info_read_private_key(self.ptr.as_ptr()) };
        // SAFETY: The native call transfers ownership of the key.
        unsafe { KeyHandle::from_raw(raw) }.expect("LibCrypto returns an ML-DSA key")
    }

    pub fn mlkem_private_key(&self) -> KeyHandle {
        assert!(matches!(self.structure, Asn1Structure::PrivateKeyInfo));
        // SAFETY: This is a native private key info value.
        let raw = unsafe { ffi::crypto_mlkem_key_info_read_private_key(self.ptr.as_ptr()) };
        // SAFETY: The native call transfers ownership of the key.
        unsafe { KeyHandle::from_raw(raw) }.expect("LibCrypto returns an ML-KEM key")
    }
}

impl Drop for KeyInfo {
    fn drop(&mut self) {
        // SAFETY: This value owns the native structure.
        unsafe { ffi::crypto_asn1_delete_key_info(self.structure, self.ptr.as_ptr()) };
    }
}
