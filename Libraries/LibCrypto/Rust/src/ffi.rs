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

use ak::{Span, SpanMut};

use super::{
    Argon2Inputs, Argon2Type, Asn1Structure, CShakeKind, EcCurve, KmacKind, MlDsaSize, MlKemSize, OkpCurve,
    RsaJwkInteger, ShaHashKind,
};

pub(super) type ReceiveBytes = unsafe extern "C" fn(*mut c_void, Span<'_, u8>);

#[repr(u8)]
#[derive(PartialEq, Eq)]
#[allow(dead_code, reason = "constructed by the native decoder")]
pub(super) enum Base64Status {
    Success,
    OutOfMemory,
    InvalidBase64,
    NonAscii,
}

#[repr(C)]
pub(super) struct OptionalBytes<'a> {
    has_value: bool,
    bytes: Span<'a, u8>,
}

impl<'a> From<Option<&'a [u8]>> for OptionalBytes<'a> {
    fn from(bytes: Option<&'a [u8]>) -> Self {
        Self {
            has_value: bytes.is_some(),
            bytes: Span::new(bytes.unwrap_or_default()),
        }
    }
}

unsafe extern "C" {
    pub(super) fn crypto_encode_base64url(bytes: Span<'_, u8>, receive: ReceiveBytes, state: *mut c_void) -> i32;
    pub(super) fn crypto_decode_base64url(
        raw: usize,
        state: *mut c_void,
        receive: ReceiveBytes,
        error_message: &mut usize,
    ) -> Base64Status;
    pub(super) fn crypto_decode_asn1_structure(
        data: Span<'_, u8>,
        structure: Asn1Structure,
        out: *mut c_void,
        all_consumed: &mut bool,
    ) -> usize;
    pub(super) fn crypto_public_key_for_private_key(key: *const c_void, name: usize, named_curve: usize)
    -> *mut c_void;
    pub(super) fn crypto_sha_digest(
        kind: ShaHashKind,
        data: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    pub(super) fn crypto_cshake_validate_parameters(
        output_length: u32,
        customization: OptionalBytes<'_>,
        function_name: OptionalBytes<'_>,
    ) -> usize;
    pub(super) fn crypto_cshake_digest(
        kind: CShakeKind,
        data: Span<'_, u8>,
        output_length: u32,
        customization: OptionalBytes<'_>,
        function_name: OptionalBytes<'_>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> usize;
    pub(super) fn crypto_hmac_sign(
        kind: ShaHashKind,
        key: Span<'_, u8>,
        message: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> ();
    pub(super) fn crypto_timing_safe_compare(left: Span<'_, u8>, right: Span<'_, u8>) -> bool;
    pub(super) fn crypto_kmac_default_key_length(kind: KmacKind) -> u32;
    pub(super) fn crypto_kmac_validate_parameters(output_length: u32, customization: OptionalBytes<'_>) -> bool;
    pub(super) fn crypto_kmac_sign(
        kind: KmacKind,
        key: Span<'_, u8>,
        message: Span<'_, u8>,
        output_length: u32,
        customization: OptionalBytes<'_>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    pub(super) fn crypto_hash_digest_size(kind: ShaHashKind) -> usize;
    pub(super) fn crypto_hkdf_derive_key(
        kind: ShaHashKind,
        salt: Span<'_, u8>,
        key: Span<'_, u8>,
        info: Span<'_, u8>,
        length: u32,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    pub(super) fn crypto_pbkdf2_derive_key(
        kind: ShaHashKind,
        password: Span<'_, u8>,
        salt: Span<'_, u8>,
        iterations: u32,
        length: u32,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    pub(super) fn crypto_argon2_validate_parameters(nonce: Span<'_, u8>) -> usize;
    pub(super) fn crypto_argon2_derive_key(
        kind: Argon2Type,
        inputs: &RawArgon2Inputs<'_>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> usize;
    pub(super) fn crypto_aes_cbc_cipher(
        encrypt: bool,
        key: Span<'_, u8>,
        input: Span<'_, u8>,
        iv: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    pub(super) fn crypto_aes_ctr_cipher(
        encrypt: bool,
        key: Span<'_, u8>,
        input: Span<'_, u8>,
        counter: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    pub(super) fn crypto_aes_kw_cipher(
        wrap: bool,
        key: Span<'_, u8>,
        input: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    pub(super) fn crypto_aes_gcm_encrypt(
        key: Span<'_, u8>,
        plaintext: Span<'_, u8>,
        iv: Span<'_, u8>,
        additional_data: Span<'_, u8>,
        tag_length: usize,
        ciphertext: *mut c_void,
        tag: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    pub(super) fn crypto_aes_gcm_decrypt(
        key: Span<'_, u8>,
        ciphertext: Span<'_, u8>,
        iv: Span<'_, u8>,
        additional_data: Span<'_, u8>,
        tag: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    pub(super) fn crypto_aes_ocb_cipher(
        encrypt: bool,
        key: Span<'_, u8>,
        input: Span<'_, u8>,
        iv: Span<'_, u8>,
        additional_data: Span<'_, u8>,
        tag_length: usize,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> usize;
    pub(super) fn crypto_chacha20_poly1305_cipher(
        encrypt: bool,
        key: Span<'_, u8>,
        iv: Span<'_, u8>,
        input: Span<'_, u8>,
        additional_data: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> usize;
    pub(super) fn crypto_asn1_allocate_key_info(structure: Asn1Structure) -> *mut c_void;
    pub(super) fn crypto_asn1_delete_key_info(structure: Asn1Structure, key_info: *mut c_void) -> ();
    pub(super) fn crypto_rsa_key_info_has_encryption_oid(structure: Asn1Structure, key_info: *const c_void) -> bool;
    pub(super) fn crypto_rsa_key_info_read_key(structure: Asn1Structure, key_info: *const c_void) -> *mut c_void;
    pub(super) fn crypto_rsa_key_is_valid(key: *const c_void, valid: &mut bool) -> bool;
    pub(super) fn crypto_rsa_validate_public_exponent(exponent: Span<'_, u8>, modulus_length: u32) -> bool;
    pub(super) fn crypto_rsa_validate_modulus_length(modulus_length: u32) -> bool;
    pub(super) fn crypto_rsa_generate_key_pair(
        modulus_length: u32,
        exponent: Span<'_, u8>,
        public_key: &mut *mut c_void,
        private_key: &mut *mut c_void,
    ) -> bool;
    pub(super) fn crypto_rsa_export_public_exponent(
        exponent: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    pub(super) fn crypto_rsa_read_key_metadata(
        key: *const c_void,
        modulus_length: &mut u32,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    pub(super) fn crypto_rsa_key_from_jwk_components(
        is_private: bool,
        components: Span<'_, Span<'_, u8>>,
    ) -> *mut c_void;
    pub(super) fn crypto_rsa_export_der(
        key: *const c_void,
        is_public: bool,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    pub(super) fn crypto_rsa_export_jwk_integer(
        key: *const c_void,
        component: RsaJwkInteger,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    pub(super) fn crypto_rsa_oaep_cipher(
        encrypt: bool,
        hash_kind: ShaHashKind,
        key: *const c_void,
        label: Span<'_, u8>,
        input: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    pub(super) fn crypto_rsa_pss_sign(
        hash_kind: ShaHashKind,
        key: *const c_void,
        salt_length: u32,
        message: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    pub(super) fn crypto_rsa_pss_verify(
        hash_kind: ShaHashKind,
        key: *const c_void,
        salt_length: u32,
        message: Span<'_, u8>,
        signature: Span<'_, u8>,
        verified: &mut bool,
    ) -> bool;
    pub(super) fn crypto_rsa_pkcs1_sign(
        hash_kind: ShaHashKind,
        key: *const c_void,
        message: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    pub(super) fn crypto_rsa_pkcs1_verify(
        hash_kind: ShaHashKind,
        key: *const c_void,
        message: Span<'_, u8>,
        signature: Span<'_, u8>,
        verified: &mut bool,
    ) -> bool;
    pub(super) fn crypto_ec_generate_key_pair(
        curve: EcCurve,
        public_key: &mut *mut c_void,
        private_key: &mut *mut c_void,
    ) -> bool;
    pub(super) fn crypto_ec_sign(
        curve: EcCurve,
        key: *const c_void,
        digest: Span<'_, u8>,
        error: &mut usize,
        r: *mut c_void,
        s: *mut c_void,
        receive: ReceiveBytes,
    ) -> u8;
    pub(super) fn crypto_ec_verify(
        curve: EcCurve,
        key: *const c_void,
        digest: Span<'_, u8>,
        r: Span<'_, u8>,
        s: Span<'_, u8>,
        verified: &mut bool,
    ) -> usize;
    pub(super) fn crypto_ec_public_key_scalar_size(key: *const c_void) -> usize;
    pub(super) fn crypto_ec_compute_coordinate(
        curve: EcCurve,
        key: *const c_void,
        public_key: *const c_void,
        error: &mut usize,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> u8;
    pub(super) fn crypto_ec_key_info_has_public_key_oid(structure: Asn1Structure, key_info: *const c_void) -> bool;
    pub(super) fn crypto_ec_key_info_parameters(
        structure: Asn1Structure,
        key_info: *const c_void,
        parameters: &mut Span<'_, u32>,
    ) -> bool;
    pub(super) fn crypto_ec_private_key_info_parameters_match(key_info: *const c_void) -> bool;
    pub(super) fn crypto_ec_key_info_read_key(structure: Asn1Structure, key_info: *const c_void) -> *mut c_void;
    pub(super) fn crypto_ec_key_from_coordinates(
        x: Span<'_, u8>,
        y: Span<'_, u8>,
        coord_size: usize,
        d: OptionalBytes<'_>,
    ) -> *mut c_void;
    pub(super) fn crypto_ec_parse_raw_public_key(bytes: Span<'_, u8>) -> *mut c_void;
    pub(super) fn crypto_ec_key_is_valid(curve: EcCurve, key: *const c_void, valid: &mut bool) -> bool;
    pub(super) fn crypto_ec_export_der(
        curve: EcCurve,
        key: *const c_void,
        is_public: bool,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    pub(super) fn crypto_ec_export_uncompressed(key: *const c_void, state: *mut c_void, receive: ReceiveBytes) -> bool;
    pub(super) fn crypto_ec_export_public_coordinates(
        curve: EcCurve,
        key: *const c_void,
        x: *mut c_void,
        y: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    pub(super) fn crypto_ec_export_private_scalar(
        key: *const c_void,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    pub(super) fn crypto_okp_generate_private_key(curve: OkpCurve, state: *mut c_void, receive: ReceiveBytes) -> bool;
    pub(super) fn crypto_okp_generate_public_key(
        curve: OkpCurve,
        private_key: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    pub(super) fn crypto_okp_compute_coordinate(
        curve: OkpCurve,
        private_key: Span<'_, u8>,
        public_key: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    pub(super) fn crypto_okp_sign(
        curve: OkpCurve,
        private_key: Span<'_, u8>,
        message: Span<'_, u8>,
        context: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    pub(super) fn crypto_okp_verify(
        curve: OkpCurve,
        public_key: Span<'_, u8>,
        signature: Span<'_, u8>,
        message: Span<'_, u8>,
        context: Span<'_, u8>,
        verified: &mut bool,
    ) -> usize;
    pub(super) fn crypto_okp_key_info_has_oid(
        curve: OkpCurve,
        structure: Asn1Structure,
        key_info: *const c_void,
    ) -> bool;
    pub(super) fn crypto_asn1_key_info_raw_key(
        structure: Asn1Structure,
        key_info: *const c_void,
        bytes: &mut Span<'_, u8>,
    ) -> ();
    pub(super) fn crypto_asn1_octet_string_bytes(string: *const c_void, bytes: &mut Span<'_, u8>) -> ();
    pub(super) fn crypto_okp_export_der(
        curve: OkpCurve,
        is_public: bool,
        key: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    pub(super) fn crypto_mldsa_sign(
        size: MlDsaSize,
        key: *const c_void,
        message: Span<'_, u8>,
        context: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    pub(super) fn crypto_mldsa_verify(
        size: MlDsaSize,
        key: *const c_void,
        message: Span<'_, u8>,
        signature: Span<'_, u8>,
        context: Span<'_, u8>,
        verified: &mut bool,
    ) -> bool;
    pub(super) fn crypto_mldsa_generate_key_pair(
        size: MlDsaSize,
        seed: Span<'_, u8>,
        public_key: &mut *mut c_void,
        private_key: &mut *mut c_void,
    ) -> usize;
    pub(super) fn crypto_asn1_key_info_identifier(
        structure: Asn1Structure,
        key_info: *const c_void,
        oid: &mut Span<'_, u32>,
    ) -> ();
    pub(super) fn crypto_mldsa_key_info_read_private_key(key_info: *const c_void) -> *mut c_void;
    pub(super) fn crypto_mldsa_read_public_key(bytes: Span<'_, u8>) -> *mut c_void;
    pub(super) fn crypto_mldsa_public_bytes(key: *const c_void, bytes: &mut Span<'_, u8>) -> ();
    pub(super) fn crypto_mldsa_seed_bytes(key: *const c_void, bytes: &mut Span<'_, u8>) -> ();
    pub(super) fn crypto_mldsa_export_der(
        key: *const c_void,
        is_public: bool,
        oid: Span<'_, u32>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    pub(super) fn crypto_mlkem_generate_key_pair(
        size: MlKemSize,
        seed: Span<'_, u8>,
        public_key: &mut *mut c_void,
        private_key: &mut *mut c_void,
    ) -> usize;
    pub(super) fn crypto_mlkem_key_info_read_private_key(key_info: *const c_void) -> *mut c_void;
    pub(super) fn crypto_mlkem_read_public_key(bytes: Span<'_, u8>) -> *mut c_void;
    pub(super) fn crypto_mlkem_public_bytes(key: *const c_void, state: *mut c_void, receive: ReceiveBytes) -> ();
    pub(super) fn crypto_mlkem_seed_bytes(key: *const c_void, bytes: &mut Span<'_, u8>) -> ();
    pub(super) fn crypto_mlkem_export_der(
        key: *const c_void,
        is_public: bool,
        oid: Span<'_, u32>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> bool;
    pub(super) fn crypto_mlkem_encapsulate(
        size: MlKemSize,
        key: *const c_void,
        shared_key: *mut c_void,
        ciphertext: *mut c_void,
        receive: ReceiveBytes,
    ) -> usize;
    pub(super) fn crypto_mlkem_decapsulate(
        size: MlKemSize,
        key: *const c_void,
        ciphertext: Span<'_, u8>,
        state: *mut c_void,
        receive: ReceiveBytes,
    ) -> usize;
    pub(super) fn crypto_fill_with_random(bytes: SpanMut<'_, u8>);
    pub(super) fn crypto_generate_random_uuid(state: *mut c_void, receive: ReceiveBytes);
}

#[repr(C)]
pub(super) struct RawArgon2Inputs<'a> {
    password: Span<'a, u8>,
    nonce: Span<'a, u8>,
    parallelism: u32,
    memory: u32,
    passes: u32,
    version: u32,
    secret_value: OptionalBytes<'a>,
    associated_data: OptionalBytes<'a>,
    tag_length: u32,
}

impl<'a> From<&Argon2Inputs<'a>> for RawArgon2Inputs<'a> {
    fn from(inputs: &Argon2Inputs<'a>) -> Self {
        Self {
            password: Span::new(inputs.password),
            nonce: Span::new(inputs.nonce),
            memory: inputs.memory,
            passes: inputs.passes,
            parallelism: inputs.parallelism,
            version: inputs.version,
            secret_value: inputs.secret_value.into(),
            associated_data: inputs.associated_data.into(),
            tag_length: inputs.tag_length,
        }
    }
}
