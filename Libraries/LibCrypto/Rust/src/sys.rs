/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The OpenSSL 3 functions that libcrypto_rust calls. The libraries that link the crate link OpenSSL.

#![allow(non_camel_case_types, clippy::upper_case_acronyms)]

use core::ffi::{c_char, c_int, c_long, c_uint, c_void};

macro_rules! opaque_types {
    ($($name:ident),* $(,)?) => {
        $(
            #[repr(C)]
            pub(crate) struct $name {
                _private: [u8; 0],
            }
        )*
    };
}

opaque_types!(
    ASN1_OBJECT,
    BIGNUM,
    BN_CTX,
    ECDSA_SIG,
    EC_GROUP,
    EC_POINT,
    EVP_CIPHER,
    EVP_CIPHER_CTX,
    EVP_KDF,
    EVP_KDF_CTX,
    EVP_MAC,
    EVP_MAC_CTX,
    EVP_MD,
    EVP_MD_CTX,
    EVP_PKEY,
    EVP_PKEY_CTX,
    EVP_SIGNATURE,
    OSSL_LIB_CTX,
    OSSL_PARAM_BLD,
    PKCS8_PRIV_KEY_INFO,
    X509_ALGOR,
    X509_PUBKEY,
);

#[repr(C)]
#[derive(Clone, Copy)]
pub(crate) struct OSSL_PARAM {
    key: *const c_char,
    data_type: c_uint,
    data: *mut c_void,
    data_size: usize,
    return_size: usize,
}

impl OSSL_PARAM {
    pub(crate) fn is_end(&self) -> bool {
        self.key.is_null()
    }
}

pub(crate) const EVP_PKEY_PUBLIC_KEY: c_int = 0x86;
pub(crate) const EVP_PKEY_KEYPAIR: c_int = 0x87;

pub(crate) const EVP_CIPHER_CTX_FLAG_WRAP_ALLOW: c_int = 0x1;
pub(crate) const EVP_CTRL_AEAD_SET_IVLEN: c_int = 0x9;
pub(crate) const EVP_CTRL_AEAD_GET_TAG: c_int = 0x10;
pub(crate) const EVP_CTRL_AEAD_SET_TAG: c_int = 0x11;

pub(crate) const V_ASN1_UNDEF: c_int = -1;
pub(crate) const V_ASN1_NULL: c_int = 5;
pub(crate) const V_ASN1_OBJECT: c_int = 6;

pub(crate) const NID_X9_62_PRIME256V1: c_int = 415;
pub(crate) const NID_SECP384R1: c_int = 715;
pub(crate) const NID_SECP521R1: c_int = 716;

pub(crate) const POINT_CONVERSION_UNCOMPRESSED: c_int = 4;

unsafe extern "C" {
    pub(crate) fn ERR_clear_error();
    pub(crate) fn OPENSSL_cleanse(pointer: *mut c_void, length: usize);
    pub(crate) fn RAND_bytes(buffer: *mut u8, length: c_int) -> c_int;

    pub(crate) fn OSSL_PARAM_construct_octet_string(key: *const c_char, buffer: *mut c_void, size: usize)
    -> OSSL_PARAM;
    pub(crate) fn OSSL_PARAM_construct_utf8_string(key: *const c_char, buffer: *mut c_char, size: usize) -> OSSL_PARAM;
    pub(crate) fn OSSL_PARAM_construct_uint(key: *const c_char, buffer: *mut c_uint) -> OSSL_PARAM;
    pub(crate) fn OSSL_PARAM_construct_int(key: *const c_char, buffer: *mut c_int) -> OSSL_PARAM;
    pub(crate) fn OSSL_PARAM_construct_end() -> OSSL_PARAM;
    pub(crate) fn OSSL_PARAM_BLD_new() -> *mut OSSL_PARAM_BLD;
    pub(crate) fn OSSL_PARAM_BLD_free(builder: *mut OSSL_PARAM_BLD);
    pub(crate) fn OSSL_PARAM_BLD_push_BN(
        builder: *mut OSSL_PARAM_BLD,
        key: *const c_char,
        value: *const BIGNUM,
    ) -> c_int;
    pub(crate) fn OSSL_PARAM_BLD_push_utf8_string(
        builder: *mut OSSL_PARAM_BLD,
        key: *const c_char,
        value: *const c_char,
        size: usize,
    ) -> c_int;
    pub(crate) fn OSSL_PARAM_BLD_push_octet_string(
        builder: *mut OSSL_PARAM_BLD,
        key: *const c_char,
        value: *const c_void,
        size: usize,
    ) -> c_int;
    pub(crate) fn OSSL_PARAM_BLD_push_uint(builder: *mut OSSL_PARAM_BLD, key: *const c_char, value: c_uint) -> c_int;
    pub(crate) fn OSSL_PARAM_BLD_to_param(builder: *mut OSSL_PARAM_BLD) -> *mut OSSL_PARAM;
    pub(crate) fn OSSL_PARAM_free(params: *mut OSSL_PARAM);

    pub(crate) fn BN_new() -> *mut BIGNUM;
    pub(crate) fn BN_secure_new() -> *mut BIGNUM;
    pub(crate) fn BN_clear_free(number: *mut BIGNUM);
    pub(crate) fn BN_bin2bn(bytes: *const u8, length: c_int, number: *mut BIGNUM) -> *mut BIGNUM;
    pub(crate) fn BN_bn2binpad(number: *const BIGNUM, bytes: *mut u8, length: c_int) -> c_int;
    pub(crate) fn BN_num_bits(number: *const BIGNUM) -> c_int;
    pub(crate) fn BN_is_odd(number: *const BIGNUM) -> c_int;
    pub(crate) fn BN_cmp(left: *const BIGNUM, right: *const BIGNUM) -> c_int;
    pub(crate) fn BN_add(result: *mut BIGNUM, left: *const BIGNUM, right: *const BIGNUM) -> c_int;
    pub(crate) fn BN_value_one() -> *const BIGNUM;
    pub(crate) fn BN_CTX_new() -> *mut BN_CTX;
    pub(crate) fn BN_CTX_free(context: *mut BN_CTX);

    pub(crate) fn EC_GROUP_new_by_curve_name(nid: c_int) -> *mut EC_GROUP;
    pub(crate) fn EC_GROUP_free(group: *mut EC_GROUP);
    pub(crate) fn EC_GROUP_get0_order(group: *const EC_GROUP) -> *const BIGNUM;
    pub(crate) fn EC_POINT_new(group: *const EC_GROUP) -> *mut EC_POINT;
    pub(crate) fn EC_POINT_free(point: *mut EC_POINT);
    pub(crate) fn EC_POINT_mul(
        group: *const EC_GROUP,
        result: *mut EC_POINT,
        generator_scalar: *const BIGNUM,
        point: *const EC_POINT,
        point_scalar: *const BIGNUM,
        context: *mut BN_CTX,
    ) -> c_int;
    pub(crate) fn EC_POINT_point2oct(
        group: *const EC_GROUP,
        point: *const EC_POINT,
        form: c_int,
        buffer: *mut u8,
        length: usize,
        context: *mut BN_CTX,
    ) -> usize;
    pub(crate) fn ECDSA_SIG_new() -> *mut ECDSA_SIG;
    pub(crate) fn ECDSA_SIG_free(signature: *mut ECDSA_SIG);
    pub(crate) fn ECDSA_SIG_set0(signature: *mut ECDSA_SIG, r: *mut BIGNUM, s: *mut BIGNUM) -> c_int;
    pub(crate) fn ECDSA_SIG_get0(signature: *const ECDSA_SIG, r: *mut *const BIGNUM, s: *mut *const BIGNUM);
    pub(crate) fn d2i_ECDSA_SIG(
        signature: *mut *mut ECDSA_SIG,
        input: *mut *const u8,
        length: c_long,
    ) -> *mut ECDSA_SIG;
    pub(crate) fn i2d_ECDSA_SIG(signature: *const ECDSA_SIG, output: *mut *mut u8) -> c_int;

    pub(crate) fn EVP_Q_digest(
        library_context: *mut OSSL_LIB_CTX,
        name: *const c_char,
        properties: *const c_char,
        data: *const c_void,
        data_length: usize,
        digest: *mut u8,
        digest_length: *mut usize,
    ) -> c_int;
    pub(crate) fn EVP_MD_fetch(
        library_context: *mut OSSL_LIB_CTX,
        name: *const c_char,
        properties: *const c_char,
    ) -> *mut EVP_MD;
    pub(crate) fn EVP_MD_free(digest: *mut EVP_MD);
    pub(crate) fn EVP_MD_CTX_new() -> *mut EVP_MD_CTX;
    pub(crate) fn EVP_MD_CTX_free(context: *mut EVP_MD_CTX);
    pub(crate) fn EVP_DigestInit_ex(context: *mut EVP_MD_CTX, digest: *const EVP_MD, engine: *mut c_void) -> c_int;
    pub(crate) fn EVP_DigestUpdate(context: *mut EVP_MD_CTX, data: *const c_void, length: usize) -> c_int;
    pub(crate) fn EVP_DigestFinalXOF(context: *mut EVP_MD_CTX, output: *mut u8, length: usize) -> c_int;
    pub(crate) fn EVP_DigestSignInit_ex(
        context: *mut EVP_MD_CTX,
        key_context: *mut *mut EVP_PKEY_CTX,
        digest_name: *const c_char,
        library_context: *mut OSSL_LIB_CTX,
        properties: *const c_char,
        key: *mut EVP_PKEY,
        params: *const OSSL_PARAM,
    ) -> c_int;
    pub(crate) fn EVP_DigestVerifyInit_ex(
        context: *mut EVP_MD_CTX,
        key_context: *mut *mut EVP_PKEY_CTX,
        digest_name: *const c_char,
        library_context: *mut OSSL_LIB_CTX,
        properties: *const c_char,
        key: *mut EVP_PKEY,
        params: *const OSSL_PARAM,
    ) -> c_int;
    pub(crate) fn EVP_DigestSign(
        context: *mut EVP_MD_CTX,
        signature: *mut u8,
        signature_length: *mut usize,
        data: *const u8,
        data_length: usize,
    ) -> c_int;
    pub(crate) fn EVP_DigestVerify(
        context: *mut EVP_MD_CTX,
        signature: *const u8,
        signature_length: usize,
        data: *const u8,
        data_length: usize,
    ) -> c_int;

    pub(crate) fn EVP_KDF_fetch(
        library_context: *mut OSSL_LIB_CTX,
        name: *const c_char,
        properties: *const c_char,
    ) -> *mut EVP_KDF;
    pub(crate) fn EVP_KDF_free(kdf: *mut EVP_KDF);
    pub(crate) fn EVP_KDF_CTX_new(kdf: *mut EVP_KDF) -> *mut EVP_KDF_CTX;
    pub(crate) fn EVP_KDF_CTX_free(context: *mut EVP_KDF_CTX);
    pub(crate) fn EVP_KDF_derive(
        context: *mut EVP_KDF_CTX,
        key: *mut u8,
        key_length: usize,
        params: *const OSSL_PARAM,
    ) -> c_int;
    pub(crate) fn EVP_MAC_fetch(
        library_context: *mut OSSL_LIB_CTX,
        name: *const c_char,
        properties: *const c_char,
    ) -> *mut EVP_MAC;
    pub(crate) fn EVP_MAC_free(mac: *mut EVP_MAC);
    pub(crate) fn EVP_MAC_CTX_new(mac: *mut EVP_MAC) -> *mut EVP_MAC_CTX;
    pub(crate) fn EVP_MAC_CTX_free(context: *mut EVP_MAC_CTX);
    pub(crate) fn EVP_MAC_init(
        context: *mut EVP_MAC_CTX,
        key: *const u8,
        key_length: usize,
        params: *const OSSL_PARAM,
    ) -> c_int;
    pub(crate) fn EVP_MAC_update(context: *mut EVP_MAC_CTX, data: *const u8, length: usize) -> c_int;
    pub(crate) fn EVP_MAC_final(
        context: *mut EVP_MAC_CTX,
        output: *mut u8,
        output_length: *mut usize,
        output_size: usize,
    ) -> c_int;

    pub(crate) fn EVP_CIPHER_CTX_new() -> *mut EVP_CIPHER_CTX;
    pub(crate) fn EVP_CIPHER_CTX_free(context: *mut EVP_CIPHER_CTX);
    pub(crate) fn EVP_CIPHER_get_key_length(cipher: *const EVP_CIPHER) -> c_int;
    pub(crate) fn EVP_CIPHER_get_iv_length(cipher: *const EVP_CIPHER) -> c_int;
    pub(crate) fn EVP_CIPHER_CTX_get_key_length(context: *const EVP_CIPHER_CTX) -> c_int;
    pub(crate) fn EVP_CIPHER_CTX_get_iv_length(context: *const EVP_CIPHER_CTX) -> c_int;
    pub(crate) fn EVP_CipherInit_ex(
        context: *mut EVP_CIPHER_CTX,
        cipher: *const EVP_CIPHER,
        engine: *mut c_void,
        key: *const u8,
        iv: *const u8,
        encrypt: c_int,
    ) -> c_int;
    pub(crate) fn EVP_CipherUpdate(
        context: *mut EVP_CIPHER_CTX,
        output: *mut u8,
        output_length: *mut c_int,
        input: *const u8,
        input_length: c_int,
    ) -> c_int;
    pub(crate) fn EVP_CipherFinal_ex(context: *mut EVP_CIPHER_CTX, output: *mut u8, output_length: *mut c_int)
    -> c_int;
    pub(crate) fn EVP_CIPHER_CTX_ctrl(
        context: *mut EVP_CIPHER_CTX,
        kind: c_int,
        argument: c_int,
        pointer: *mut c_void,
    ) -> c_int;
    pub(crate) fn EVP_CIPHER_CTX_set_flags(context: *mut EVP_CIPHER_CTX, flags: c_int);
    pub(crate) fn EVP_CIPHER_CTX_set_padding(context: *mut EVP_CIPHER_CTX, padding: c_int) -> c_int;
    pub(crate) fn EVP_aes_128_cbc() -> *const EVP_CIPHER;
    pub(crate) fn EVP_aes_192_cbc() -> *const EVP_CIPHER;
    pub(crate) fn EVP_aes_256_cbc() -> *const EVP_CIPHER;
    pub(crate) fn EVP_aes_128_ctr() -> *const EVP_CIPHER;
    pub(crate) fn EVP_aes_192_ctr() -> *const EVP_CIPHER;
    pub(crate) fn EVP_aes_256_ctr() -> *const EVP_CIPHER;
    pub(crate) fn EVP_aes_128_gcm() -> *const EVP_CIPHER;
    pub(crate) fn EVP_aes_192_gcm() -> *const EVP_CIPHER;
    pub(crate) fn EVP_aes_256_gcm() -> *const EVP_CIPHER;
    pub(crate) fn EVP_aes_128_ocb() -> *const EVP_CIPHER;
    pub(crate) fn EVP_aes_192_ocb() -> *const EVP_CIPHER;
    pub(crate) fn EVP_aes_256_ocb() -> *const EVP_CIPHER;
    pub(crate) fn EVP_aes_128_wrap() -> *const EVP_CIPHER;
    pub(crate) fn EVP_aes_192_wrap() -> *const EVP_CIPHER;
    pub(crate) fn EVP_aes_256_wrap() -> *const EVP_CIPHER;
    pub(crate) fn EVP_chacha20_poly1305() -> *const EVP_CIPHER;

    pub(crate) fn EVP_PKEY_free(key: *mut EVP_PKEY);
    pub(crate) fn EVP_PKEY_up_ref(key: *mut EVP_PKEY) -> c_int;
    pub(crate) fn EVP_PKEY_CTX_new_from_name(
        library_context: *mut OSSL_LIB_CTX,
        name: *const c_char,
        properties: *const c_char,
    ) -> *mut EVP_PKEY_CTX;
    pub(crate) fn EVP_PKEY_CTX_new_from_pkey(
        library_context: *mut OSSL_LIB_CTX,
        key: *mut EVP_PKEY,
        properties: *const c_char,
    ) -> *mut EVP_PKEY_CTX;
    pub(crate) fn EVP_PKEY_CTX_free(context: *mut EVP_PKEY_CTX);
    pub(crate) fn EVP_PKEY_CTX_set_params(context: *mut EVP_PKEY_CTX, params: *const OSSL_PARAM) -> c_int;
    pub(crate) fn EVP_PKEY_fromdata_init(context: *mut EVP_PKEY_CTX) -> c_int;
    pub(crate) fn EVP_PKEY_fromdata(
        context: *mut EVP_PKEY_CTX,
        key: *mut *mut EVP_PKEY,
        selection: c_int,
        params: *mut OSSL_PARAM,
    ) -> c_int;
    pub(crate) fn EVP_PKEY_keygen_init(context: *mut EVP_PKEY_CTX) -> c_int;
    pub(crate) fn EVP_PKEY_generate(context: *mut EVP_PKEY_CTX, key: *mut *mut EVP_PKEY) -> c_int;
    pub(crate) fn EVP_PKEY_get_bn_param(key: *const EVP_PKEY, name: *const c_char, number: *mut *mut BIGNUM) -> c_int;
    pub(crate) fn EVP_PKEY_get_octet_string_param(
        key: *const EVP_PKEY,
        name: *const c_char,
        buffer: *mut u8,
        size: usize,
        length: *mut usize,
    ) -> c_int;
    pub(crate) fn EVP_PKEY_get_raw_public_key(key: *const EVP_PKEY, buffer: *mut u8, length: *mut usize) -> c_int;
    pub(crate) fn EVP_PKEY_get_raw_private_key(key: *const EVP_PKEY, buffer: *mut u8, length: *mut usize) -> c_int;
    pub(crate) fn EVP_PKEY_new_raw_public_key_ex(
        library_context: *mut OSSL_LIB_CTX,
        key_type: *const c_char,
        properties: *const c_char,
        bytes: *const u8,
        length: usize,
    ) -> *mut EVP_PKEY;
    pub(crate) fn EVP_PKEY_new_raw_private_key_ex(
        library_context: *mut OSSL_LIB_CTX,
        key_type: *const c_char,
        properties: *const c_char,
        bytes: *const u8,
        length: usize,
    ) -> *mut EVP_PKEY;
    pub(crate) fn EVP_PKEY_get_size(key: *const EVP_PKEY) -> c_int;
    pub(crate) fn EVP_PKEY_get_bits(key: *const EVP_PKEY) -> c_int;
    pub(crate) fn EVP_PKEY_public_check(context: *mut EVP_PKEY_CTX) -> c_int;
    pub(crate) fn EVP_PKEY_private_check(context: *mut EVP_PKEY_CTX) -> c_int;
    pub(crate) fn EVP_PKEY_pairwise_check(context: *mut EVP_PKEY_CTX) -> c_int;
    pub(crate) fn EVP_PKEY_sign_init(context: *mut EVP_PKEY_CTX) -> c_int;
    pub(crate) fn EVP_PKEY_sign(
        context: *mut EVP_PKEY_CTX,
        signature: *mut u8,
        signature_length: *mut usize,
        data: *const u8,
        data_length: usize,
    ) -> c_int;
    pub(crate) fn EVP_PKEY_verify_init(context: *mut EVP_PKEY_CTX) -> c_int;
    pub(crate) fn EVP_PKEY_verify(
        context: *mut EVP_PKEY_CTX,
        signature: *const u8,
        signature_length: usize,
        data: *const u8,
        data_length: usize,
    ) -> c_int;
    pub(crate) fn EVP_PKEY_encrypt_init_ex(context: *mut EVP_PKEY_CTX, params: *const OSSL_PARAM) -> c_int;
    pub(crate) fn EVP_PKEY_encrypt(
        context: *mut EVP_PKEY_CTX,
        output: *mut u8,
        output_length: *mut usize,
        input: *const u8,
        input_length: usize,
    ) -> c_int;
    pub(crate) fn EVP_PKEY_decrypt_init_ex(context: *mut EVP_PKEY_CTX, params: *const OSSL_PARAM) -> c_int;
    pub(crate) fn EVP_PKEY_decrypt(
        context: *mut EVP_PKEY_CTX,
        output: *mut u8,
        output_length: *mut usize,
        input: *const u8,
        input_length: usize,
    ) -> c_int;
    pub(crate) fn EVP_PKEY_derive_init(context: *mut EVP_PKEY_CTX) -> c_int;
    pub(crate) fn EVP_PKEY_derive_set_peer_ex(
        context: *mut EVP_PKEY_CTX,
        peer: *mut EVP_PKEY,
        validate: c_int,
    ) -> c_int;
    pub(crate) fn EVP_PKEY_derive(context: *mut EVP_PKEY_CTX, key: *mut u8, key_length: *mut usize) -> c_int;
    pub(crate) fn EVP_SIGNATURE_fetch(
        library_context: *mut OSSL_LIB_CTX,
        name: *const c_char,
        properties: *const c_char,
    ) -> *mut EVP_SIGNATURE;
    pub(crate) fn EVP_SIGNATURE_free(signature: *mut EVP_SIGNATURE);
    pub(crate) fn EVP_PKEY_sign_message_init(
        context: *mut EVP_PKEY_CTX,
        algorithm: *mut EVP_SIGNATURE,
        params: *const OSSL_PARAM,
    ) -> c_int;
    pub(crate) fn EVP_PKEY_verify_message_init(
        context: *mut EVP_PKEY_CTX,
        algorithm: *mut EVP_SIGNATURE,
        params: *const OSSL_PARAM,
    ) -> c_int;
    pub(crate) fn EVP_PKEY_encapsulate_init(context: *mut EVP_PKEY_CTX, params: *const OSSL_PARAM) -> c_int;
    pub(crate) fn EVP_PKEY_encapsulate(
        context: *mut EVP_PKEY_CTX,
        ciphertext: *mut u8,
        ciphertext_length: *mut usize,
        shared_key: *mut u8,
        shared_key_length: *mut usize,
    ) -> c_int;
    pub(crate) fn EVP_PKEY_decapsulate_init(context: *mut EVP_PKEY_CTX, params: *const OSSL_PARAM) -> c_int;
    pub(crate) fn EVP_PKEY_decapsulate(
        context: *mut EVP_PKEY_CTX,
        shared_key: *mut u8,
        shared_key_length: *mut usize,
        ciphertext: *const u8,
        ciphertext_length: usize,
    ) -> c_int;

    pub(crate) fn i2d_PUBKEY(key: *const EVP_PKEY, output: *mut *mut u8) -> c_int;
    pub(crate) fn i2d_PrivateKey(key: *const EVP_PKEY, output: *mut *mut u8) -> c_int;
    pub(crate) fn EVP_PKCS82PKEY(private_key_info: *const PKCS8_PRIV_KEY_INFO) -> *mut EVP_PKEY;
    pub(crate) fn d2i_PKCS8_PRIV_KEY_INFO(
        private_key_info: *mut *mut PKCS8_PRIV_KEY_INFO,
        input: *mut *const u8,
        length: c_long,
    ) -> *mut PKCS8_PRIV_KEY_INFO;
    pub(crate) fn i2d_PKCS8_PRIV_KEY_INFO(private_key_info: *const PKCS8_PRIV_KEY_INFO, output: *mut *mut u8) -> c_int;
    pub(crate) fn PKCS8_PRIV_KEY_INFO_free(private_key_info: *mut PKCS8_PRIV_KEY_INFO);
    pub(crate) fn PKCS8_pkey_get0(
        algorithm_object: *mut *const ASN1_OBJECT,
        key: *mut *const u8,
        key_length: *mut c_int,
        algorithm: *mut *const X509_ALGOR,
        private_key_info: *const PKCS8_PRIV_KEY_INFO,
    ) -> c_int;
    pub(crate) fn d2i_X509_PUBKEY(
        public_key: *mut *mut X509_PUBKEY,
        input: *mut *const u8,
        length: c_long,
    ) -> *mut X509_PUBKEY;
    pub(crate) fn i2d_X509_PUBKEY(public_key: *const X509_PUBKEY, output: *mut *mut u8) -> c_int;
    pub(crate) fn X509_PUBKEY_free(public_key: *mut X509_PUBKEY);
    pub(crate) fn X509_PUBKEY_get0_param(
        algorithm_object: *mut *mut ASN1_OBJECT,
        key: *mut *const u8,
        key_length: *mut c_int,
        algorithm: *mut *mut X509_ALGOR,
        public_key: *const X509_PUBKEY,
    ) -> c_int;
    pub(crate) fn X509_PUBKEY_get(public_key: *const X509_PUBKEY) -> *mut EVP_PKEY;
    pub(crate) fn X509_ALGOR_get0(
        object: *mut *const ASN1_OBJECT,
        parameters_type: *mut c_int,
        parameters: *mut *const c_void,
        algorithm: *const X509_ALGOR,
    );
    pub(crate) fn OBJ_length(object: *const ASN1_OBJECT) -> usize;
    pub(crate) fn OBJ_get0_data(object: *const ASN1_OBJECT) -> *const u8;
}
