/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2025, Altomani Gianluca <altomanigianluca@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ffi::c_void;
use core::ptr::{NonNull, null, null_mut};
use std::rc::Rc;

use ak::{Span, Utf16String};
use libjs_rust::api::{Gc, RawNativeFunctionResult, Realm, Trace, Value};

use crate::bindings::WebIdlResult;
use crate::bindings::interface::{Ref, cell_from_abi, cell_into_abi};
use crate::crypto::crypto_bindings::{CryptoKeyPair, JsonWebKey, RawJsonWebKey};
use crate::crypto::crypto_key::{CryptoKey, KeyUsage};
use crate::crypto::subtle_crypto::{CryptoKeyOrCryptoKeyPair, KeyFormat};

#[derive(Clone, Copy)]
#[repr(u8)]
pub(crate) enum AlgorithmId {
    RsassaPkcs1,
    RsaPss,
    RsaOaep,
    Ecdsa,
    Ecdh,
    AesCtr,
    AesCbc,
    AesGcm,
    AesKw,
    Hmac,
    Sha,
    CShake,
    Hkdf,
    Pbkdf2,
    X25519,
    X448,
    ED25519,
    ED448,
    MlDsa,
    MlKem,
    Argon2,
    ChaCha20Poly1305,
    AesOcb,
    Kmac,
}

#[derive(Clone, Copy)]
#[repr(u8)]
#[allow(
    clippy::enum_variant_names,
    reason = "each variant names an algorithm parameter dictionary"
)]
pub(crate) enum ParameterId {
    AlgorithmParams,
    RsaHashedKeyGenParams,
    RsaHashedImportParams,
    RsaPssParams,
    RsaOaepParams,
    EcdsaParams,
    EcKeyGenParams,
    EcKeyImportParams,
    EcdhKeyDeriveParams,
    AesCtrParams,
    AesKeyGenParams,
    AesDerivedKeyParams,
    AesCbcParams,
    AesGcmParams,
    HmacKeyGenParams,
    HmacImportParams,
    CShakeParams,
    HkdfParams,
    Pbkdf2Params,
    Ed448Params,
    ContextParams,
    Argon2Params,
    AeadParams,
    KmacParams,
    KmacKeyGenParams,
    KmacImportParams,
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Bytes(#[gc(untraced)] Rc<Vec<u8>>);

impl Bytes {
    pub(crate) fn new(bytes: Vec<u8>) -> Self {
        Self(Rc::new(bytes))
    }

    pub(crate) fn as_slice(&self) -> &[u8] {
        self.0.as_slice()
    }
}

pub(crate) enum ExportKeyResult {
    Bytes(Bytes),
    JsonWebKey(JsonWebKey),
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) enum ImportKeyData {
    Bytes(Bytes),
    JsonWebKey(JsonWebKey),
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct EncapsulatedBitsData {
    pub(crate) shared_key: Option<Bytes>,
    pub(crate) ciphertext: Option<Bytes>,
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct EncapsulatedKeyData {
    pub(crate) shared_key: Ref<CryptoKey>,
    pub(crate) ciphertext: Option<Bytes>,
}

#[repr(C)]
#[derive(Default)]
struct OptionalLength {
    has_value: bool,
    value: u32,
}

impl From<Option<u32>> for OptionalLength {
    fn from(value: Option<u32>) -> Self {
        Self {
            has_value: value.is_some(),
            value: value.unwrap_or_default(),
        }
    }
}

type ReceiveBytes = unsafe extern "C" fn(*mut c_void, Span<'_, u8>);
type ReceiveUsages = unsafe extern "C" fn(*mut c_void, Span<'_, KeyUsage>);
type ReceiveJsonWebKey = unsafe extern "C" fn(*mut c_void, &RawJsonWebKey<'_>);

unsafe extern "C" {
    fn web_crypto_algorithm_from_value(
        realm: *mut c_void,
        method_id: AlgorithmId,
        parameter_id: ParameterId,
        value: u64,
        name: usize,
        out: &mut *mut c_void,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_destroy(algorithm: *mut c_void);
    fn web_crypto_algorithm_public_key_usages(algorithm: *mut c_void, state: *mut c_void, receive: ReceiveUsages);
    fn web_crypto_normalized_key_usages(usages: Span<'_, KeyUsage>, state: *mut c_void, receive: ReceiveUsages);
    fn web_crypto_public_key_for_private_key(key: *mut c_void) -> *mut c_void;
    fn web_crypto_algorithm_encrypt(
        realm: *mut c_void,
        algorithm: *mut c_void,
        key: *mut c_void,
        data: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_decrypt(
        realm: *mut c_void,
        algorithm: *mut c_void,
        key: *mut c_void,
        data: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_sign(
        realm: *mut c_void,
        algorithm: *mut c_void,
        key: *mut c_void,
        data: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_verify(
        realm: *mut c_void,
        algorithm: *mut c_void,
        key: *mut c_void,
        signature: Span<'_, u8>,
        data: Span<'_, u8>,
        out: &mut bool,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_digest(
        realm: *mut c_void,
        algorithm: *mut c_void,
        data: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_derive_bits(
        realm: *mut c_void,
        algorithm: *mut c_void,
        key: *mut c_void,
        length: OptionalLength,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_import_key(
        realm: *mut c_void,
        algorithm: *mut c_void,
        format: KeyFormat,
        key_data: Span<'_, u8>,
        jwk: *const c_void,
        extractable: bool,
        usages: Span<'_, KeyUsage>,
        out: &mut *mut c_void,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_generate_key(
        realm: *mut c_void,
        algorithm: *mut c_void,
        extractable: bool,
        usages: Span<'_, KeyUsage>,
        out_key: &mut *mut c_void,
        out_public_key: &mut *mut c_void,
        out_private_key: &mut *mut c_void,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_export_key(
        realm: *mut c_void,
        algorithm: *mut c_void,
        format: KeyFormat,
        key: *mut c_void,
        state: *mut c_void,
        receive_bytes: ReceiveBytes,
        receive_jwk: ReceiveJsonWebKey,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_get_key_length(
        realm: *mut c_void,
        algorithm: *mut c_void,
        out: &mut OptionalLength,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_get_shared_key_length(
        realm: *mut c_void,
        algorithm: *mut c_void,
        out: &mut u32,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_wrap_key(
        realm: *mut c_void,
        algorithm: *mut c_void,
        key: *mut c_void,
        data: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_unwrap_key(
        realm: *mut c_void,
        algorithm: *mut c_void,
        key: *mut c_void,
        data: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_encapsulate(
        realm: *mut c_void,
        algorithm: *mut c_void,
        key: *mut c_void,
        shared_key: *mut c_void,
        ciphertext: *mut c_void,
        receive: ReceiveBytes,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_decapsulate(
        realm: *mut c_void,
        algorithm: *mut c_void,
        key: *mut c_void,
        data: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_validate_encrypt_parameters(
        realm: *mut c_void,
        algorithm: *mut c_void,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_validate_decrypt_parameters(
        realm: *mut c_void,
        algorithm: *mut c_void,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_validate_sign_parameters(
        realm: *mut c_void,
        algorithm: *mut c_void,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_validate_verify_parameters(
        realm: *mut c_void,
        algorithm: *mut c_void,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_validate_digest_parameters(
        realm: *mut c_void,
        algorithm: *mut c_void,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_validate_generate_key_parameters(
        realm: *mut c_void,
        algorithm: *mut c_void,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_validate_import_key_parameters(
        realm: *mut c_void,
        algorithm: *mut c_void,
    ) -> RawNativeFunctionResult;
    fn web_crypto_algorithm_validate_derive_bits_parameters(
        realm: *mut c_void,
        algorithm: *mut c_void,
        length: OptionalLength,
    ) -> RawNativeFunctionResult;
}

pub(crate) struct CppAlgorithm(NonNull<c_void>);

impl Drop for CppAlgorithm {
    fn drop(&mut self) {
        // SAFETY: This object owns the C++ algorithm.
        unsafe { web_crypto_algorithm_destroy(self.0.as_ptr()) };
    }
}

impl CppAlgorithm {
    pub(crate) fn from_value(
        realm: Gc<Realm>,
        method_id: AlgorithmId,
        parameter_id: ParameterId,
        value: Value,
        name: &Utf16String,
    ) -> WebIdlResult<Self> {
        let mut out = null_mut();
        // SAFETY: The realm and value are live, and the call borrows the name.
        unsafe {
            web_crypto_algorithm_from_value(
                realm.as_ptr().cast(),
                method_id,
                parameter_id,
                value.to_raw(),
                name.raw_identity(),
                &mut out,
            )
        }
        .into_completion()?;
        Ok(Self(NonNull::new(out).expect("LibWeb returns an algorithm")))
    }

    pub(crate) fn into_raw(self) -> *mut c_void {
        core::mem::ManuallyDrop::new(self).0.as_ptr()
    }

    pub(crate) fn public_key_usages(&self) -> Option<Vec<KeyUsage>> {
        let mut usages = None;
        // SAFETY: This object owns the algorithm and the state is writable for the call.
        unsafe {
            web_crypto_algorithm_public_key_usages(
                self.0.as_ptr(),
                core::ptr::from_mut(&mut usages).cast(),
                receive_usages,
            );
        };
        usages
    }

    pub(crate) fn encrypt(&self, realm: Gc<Realm>, key: Ref<CryptoKey>, data: &Bytes) -> WebIdlResult<Bytes> {
        let mut out: Option<Bytes> = None;
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe {
            web_crypto_algorithm_encrypt(
                realm.as_ptr().cast(),
                self.0.as_ptr(),
                cell_into_abi(key),
                Span::new(data.as_slice()),
                core::ptr::from_mut(&mut out).cast(),
                receive_bytes,
            )
        }
        .into_completion()?;
        Ok(out.expect("LibWeb returns bytes"))
    }

    pub(crate) fn decrypt(&self, realm: Gc<Realm>, key: Ref<CryptoKey>, data: &Bytes) -> WebIdlResult<Bytes> {
        let mut out: Option<Bytes> = None;
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe {
            web_crypto_algorithm_decrypt(
                realm.as_ptr().cast(),
                self.0.as_ptr(),
                cell_into_abi(key),
                Span::new(data.as_slice()),
                core::ptr::from_mut(&mut out).cast(),
                receive_bytes,
            )
        }
        .into_completion()?;
        Ok(out.expect("LibWeb returns bytes"))
    }

    pub(crate) fn sign(&self, realm: Gc<Realm>, key: Ref<CryptoKey>, data: &Bytes) -> WebIdlResult<Bytes> {
        let mut out: Option<Bytes> = None;
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe {
            web_crypto_algorithm_sign(
                realm.as_ptr().cast(),
                self.0.as_ptr(),
                cell_into_abi(key),
                Span::new(data.as_slice()),
                core::ptr::from_mut(&mut out).cast(),
                receive_bytes,
            )
        }
        .into_completion()?;
        Ok(out.expect("LibWeb returns bytes"))
    }

    pub(crate) fn verify(
        &self,
        realm: Gc<Realm>,
        key: Ref<CryptoKey>,
        signature: &Bytes,
        data: &Bytes,
    ) -> WebIdlResult<bool> {
        let mut out = false;
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe {
            web_crypto_algorithm_verify(
                realm.as_ptr().cast(),
                self.0.as_ptr(),
                cell_into_abi(key),
                Span::new(signature.as_slice()),
                Span::new(data.as_slice()),
                &mut out,
            )
        }
        .into_completion()?;
        Ok(out)
    }

    pub(crate) fn digest(&self, realm: Gc<Realm>, data: &Bytes) -> WebIdlResult<Bytes> {
        let mut out: Option<Bytes> = None;
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe {
            web_crypto_algorithm_digest(
                realm.as_ptr().cast(),
                self.0.as_ptr(),
                Span::new(data.as_slice()),
                core::ptr::from_mut(&mut out).cast(),
                receive_bytes,
            )
        }
        .into_completion()?;
        Ok(out.expect("LibWeb returns bytes"))
    }

    pub(crate) fn derive_bits(
        &self,
        realm: Gc<Realm>,
        key: Ref<CryptoKey>,
        length: Option<u32>,
    ) -> WebIdlResult<Bytes> {
        let mut out: Option<Bytes> = None;
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe {
            web_crypto_algorithm_derive_bits(
                realm.as_ptr().cast(),
                self.0.as_ptr(),
                cell_into_abi(key),
                length.into(),
                core::ptr::from_mut(&mut out).cast(),
                receive_bytes,
            )
        }
        .into_completion()?;
        Ok(out.expect("LibWeb returns bytes"))
    }

    pub(crate) fn import_key(
        &self,
        realm: Gc<Realm>,
        format: KeyFormat,
        key_data: ImportKeyData,
        extractable: bool,
        usages: &[KeyUsage],
    ) -> WebIdlResult<Ref<CryptoKey>> {
        let mut out = null_mut();
        let mut import = |data, jwk| {
            // SAFETY: The realm and algorithm are live, and the input and output are lent for the call.
            unsafe {
                web_crypto_algorithm_import_key(
                    realm.as_ptr().cast(),
                    self.0.as_ptr(),
                    format,
                    data,
                    jwk,
                    extractable,
                    Span::new(usages),
                    &mut out,
                )
            }
            .into_completion()
        };
        match key_data {
            ImportKeyData::Bytes(bytes) => import(Span::new(bytes.as_slice()), null())?,
            ImportKeyData::JsonWebKey(key) => {
                key.with_raw_fields(|fields| import(Span::default(), core::ptr::from_ref(fields).cast()))?
            }
        };
        // SAFETY: LibWeb returns a live CryptoKey cell.
        Ok(unsafe { cell_from_abi(out) })
    }

    pub(crate) fn generate_key(
        &self,
        realm: Gc<Realm>,
        extractable: bool,
        usages: &[KeyUsage],
    ) -> WebIdlResult<CryptoKeyOrCryptoKeyPair> {
        let (mut key, mut public_key, mut private_key) = (null_mut(), null_mut(), null_mut());
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe {
            web_crypto_algorithm_generate_key(
                realm.as_ptr().cast(),
                self.0.as_ptr(),
                extractable,
                Span::new(usages),
                &mut key,
                &mut public_key,
                &mut private_key,
            )
        }
        .into_completion()?;
        // SAFETY: LibWeb returns a live CryptoKey or two live CryptoKeys.
        unsafe {
            if !key.is_null() {
                Ok(CryptoKeyOrCryptoKeyPair::CryptoKey(cell_from_abi(key)))
            } else {
                Ok(CryptoKeyOrCryptoKeyPair::CryptoKeyPair(CryptoKeyPair {
                    public_key: Some(cell_from_abi(public_key)),
                    private_key: Some(cell_from_abi(private_key)),
                }))
            }
        }
    }

    pub(crate) fn export_key(
        &self,
        realm: Gc<Realm>,
        format: KeyFormat,
        key: Ref<CryptoKey>,
    ) -> WebIdlResult<ExportKeyResult> {
        let mut out: Option<ExportKeyResult> = None;
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe {
            web_crypto_algorithm_export_key(
                realm.as_ptr().cast(),
                self.0.as_ptr(),
                format,
                cell_into_abi(key),
                core::ptr::from_mut(&mut out).cast(),
                receive_export_bytes,
                receive_export_jwk,
            )
        }
        .into_completion()?;
        Ok(out.expect("LibWeb returns an export result"))
    }

    pub(crate) fn get_key_length(&self, realm: Gc<Realm>) -> WebIdlResult<Option<u32>> {
        let mut out = OptionalLength::default();
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe { web_crypto_algorithm_get_key_length(realm.as_ptr().cast(), self.0.as_ptr(), &mut out) }
            .into_completion()?;
        Ok(out.has_value.then_some(out.value))
    }

    pub(crate) fn get_shared_key_length(&self, realm: Gc<Realm>) -> WebIdlResult<u32> {
        let mut out = 0;
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe { web_crypto_algorithm_get_shared_key_length(realm.as_ptr().cast(), self.0.as_ptr(), &mut out) }
            .into_completion()?;
        Ok(out)
    }

    pub(crate) fn wrap_key(&self, realm: Gc<Realm>, key: Ref<CryptoKey>, data: &Bytes) -> WebIdlResult<Bytes> {
        let mut out: Option<Bytes> = None;
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe {
            web_crypto_algorithm_wrap_key(
                realm.as_ptr().cast(),
                self.0.as_ptr(),
                cell_into_abi(key),
                Span::new(data.as_slice()),
                core::ptr::from_mut(&mut out).cast(),
                receive_bytes,
            )
        }
        .into_completion()?;
        Ok(out.expect("LibWeb returns bytes"))
    }

    pub(crate) fn unwrap_key(&self, realm: Gc<Realm>, key: Ref<CryptoKey>, data: &Bytes) -> WebIdlResult<Bytes> {
        let mut out: Option<Bytes> = None;
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe {
            web_crypto_algorithm_unwrap_key(
                realm.as_ptr().cast(),
                self.0.as_ptr(),
                cell_into_abi(key),
                Span::new(data.as_slice()),
                core::ptr::from_mut(&mut out).cast(),
                receive_bytes,
            )
        }
        .into_completion()?;
        Ok(out.expect("LibWeb returns bytes"))
    }

    pub(crate) fn encapsulate(&self, realm: Gc<Realm>, key: Ref<CryptoKey>) -> WebIdlResult<EncapsulatedBitsData> {
        let (mut shared_key, mut ciphertext): (Option<Bytes>, Option<Bytes>) = (None, None);
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe {
            web_crypto_algorithm_encapsulate(
                realm.as_ptr().cast(),
                self.0.as_ptr(),
                cell_into_abi(key),
                core::ptr::from_mut(&mut shared_key).cast(),
                core::ptr::from_mut(&mut ciphertext).cast(),
                receive_bytes,
            )
        }
        .into_completion()?;
        Ok(EncapsulatedBitsData { shared_key, ciphertext })
    }

    pub(crate) fn decapsulate(&self, realm: Gc<Realm>, key: Ref<CryptoKey>, data: &Bytes) -> WebIdlResult<Bytes> {
        let mut out: Option<Bytes> = None;
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe {
            web_crypto_algorithm_decapsulate(
                realm.as_ptr().cast(),
                self.0.as_ptr(),
                cell_into_abi(key),
                Span::new(data.as_slice()),
                core::ptr::from_mut(&mut out).cast(),
                receive_bytes,
            )
        }
        .into_completion()?;
        Ok(out.expect("LibWeb returns bytes"))
    }

    pub(crate) fn validate_encrypt_parameters(&self, realm: Gc<Realm>) -> WebIdlResult<()> {
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe { web_crypto_algorithm_validate_encrypt_parameters(realm.as_ptr().cast(), self.0.as_ptr()) }
            .into_completion()?;
        Ok(())
    }

    pub(crate) fn validate_decrypt_parameters(&self, realm: Gc<Realm>) -> WebIdlResult<()> {
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe { web_crypto_algorithm_validate_decrypt_parameters(realm.as_ptr().cast(), self.0.as_ptr()) }
            .into_completion()?;
        Ok(())
    }

    pub(crate) fn validate_sign_parameters(&self, realm: Gc<Realm>) -> WebIdlResult<()> {
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe { web_crypto_algorithm_validate_sign_parameters(realm.as_ptr().cast(), self.0.as_ptr()) }
            .into_completion()?;
        Ok(())
    }

    pub(crate) fn validate_verify_parameters(&self, realm: Gc<Realm>) -> WebIdlResult<()> {
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe { web_crypto_algorithm_validate_verify_parameters(realm.as_ptr().cast(), self.0.as_ptr()) }
            .into_completion()?;
        Ok(())
    }

    pub(crate) fn validate_digest_parameters(&self, realm: Gc<Realm>) -> WebIdlResult<()> {
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe { web_crypto_algorithm_validate_digest_parameters(realm.as_ptr().cast(), self.0.as_ptr()) }
            .into_completion()?;
        Ok(())
    }

    pub(crate) fn validate_generate_key_parameters(&self, realm: Gc<Realm>) -> WebIdlResult<()> {
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe { web_crypto_algorithm_validate_generate_key_parameters(realm.as_ptr().cast(), self.0.as_ptr()) }
            .into_completion()?;
        Ok(())
    }

    pub(crate) fn validate_import_key_parameters(&self, realm: Gc<Realm>) -> WebIdlResult<()> {
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe { web_crypto_algorithm_validate_import_key_parameters(realm.as_ptr().cast(), self.0.as_ptr()) }
            .into_completion()?;
        Ok(())
    }

    pub(crate) fn validate_derive_bits_parameters(&self, realm: Gc<Realm>, length: Option<u32>) -> WebIdlResult<()> {
        // SAFETY: The realm, algorithm and cells are live, and the other arguments are lent for the call.
        unsafe {
            web_crypto_algorithm_validate_derive_bits_parameters(realm.as_ptr().cast(), self.0.as_ptr(), length.into())
        }
        .into_completion()?;
        Ok(())
    }
}

unsafe extern "C" fn receive_bytes(state: *mut c_void, bytes: Span<'_, u8>) {
    // SAFETY: The state is the caller's slot, and the service lends the bytes.
    unsafe { *state.cast::<Option<Bytes>>() = Some(Bytes::new(bytes.as_slice().to_vec())) };
}

unsafe extern "C" fn receive_usages(state: *mut c_void, usages: Span<'_, KeyUsage>) {
    // SAFETY: The state is the caller's slot, and the service lends the usages.
    unsafe { *state.cast::<Option<Vec<KeyUsage>>>() = Some(usages.as_slice().to_vec()) };
}

unsafe extern "C" fn receive_export_bytes(state: *mut c_void, bytes: Span<'_, u8>) {
    // SAFETY: The state is the caller's slot, and the service lends the bytes.
    unsafe {
        *state.cast::<Option<ExportKeyResult>>() = Some(ExportKeyResult::Bytes(Bytes::new(bytes.as_slice().to_vec())));
    };
}

unsafe extern "C" fn receive_export_jwk(state: *mut c_void, key: &RawJsonWebKey<'_>) {
    // SAFETY: The state is the caller's slot, and the service lends the fields.
    unsafe {
        *state.cast::<Option<ExportKeyResult>>() = Some(ExportKeyResult::JsonWebKey(JsonWebKey::from_raw_fields(key)));
    };
}

pub(crate) fn normalized_key_usages(usages: &[KeyUsage]) -> Vec<KeyUsage> {
    let mut normalized: Option<Vec<KeyUsage>> = None;
    // SAFETY: The usages and the output slot are lent for the call.
    unsafe {
        web_crypto_normalized_key_usages(
            Span::new(usages),
            core::ptr::from_mut(&mut normalized).cast(),
            receive_usages,
        );
    };
    normalized.expect("LibWeb returns the normalized usages")
}

pub(crate) fn public_key_for_private_key(key: Ref<CryptoKey>) -> Option<Ref<CryptoKey>> {
    // SAFETY: The key is live.
    let public_key = unsafe { web_crypto_public_key_for_private_key(cell_into_abi(key)) };
    // SAFETY: A non-null pointer returned by LibWeb is a live CryptoKey cell.
    NonNull::new(public_key).map(|key| unsafe { cell_from_abi(key.as_ptr()) })
}
