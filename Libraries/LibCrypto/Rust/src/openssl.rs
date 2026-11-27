/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Safe wrappers around the OpenSSL calls that libcrypto_rust makes. A failed call's reasons are dropped from OpenSSL's
//! error queue, and it fails with CryptoError::Backend.

use core::ffi::{CStr, c_int, c_long, c_uint};
use core::marker::PhantomData;
use core::ptr::{self, NonNull};

use crate::CryptoError;
use crate::key::SecretBytes;
use crate::key_info::KeyAlgorithm;
use crate::sys;

fn failed<T>() -> Result<T, CryptoError> {
    // SAFETY: Clearing the error queue takes no arguments.
    unsafe { sys::ERR_clear_error() };
    Err(CryptoError::Backend)
}

fn check(result: c_int) -> Result<(), CryptoError> {
    if result > 0 { Ok(()) } else { failed() }
}

/// An OpenSSL result that is 1 for true and 0 for false, such as a verification's.
fn check_bool(result: c_int) -> Result<bool, CryptoError> {
    match result {
        1 => Ok(true),
        0 => {
            // SAFETY: Clearing the error queue takes no arguments.
            unsafe { sys::ERR_clear_error() };
            Ok(false)
        }
        _ => failed(),
    }
}

/// `length` as the int that OpenSSL takes for some lengths.
fn int_length(length: usize) -> Result<c_int, CryptoError> {
    c_int::try_from(length).map_err(|_| CryptoError::Backend)
}

macro_rules! owned {
    ($(#[$meta:meta])* $name:ident, $type:ident, $free:ident) => {
        $(#[$meta])*
        pub(crate) struct $name(NonNull<sys::$type>);

        impl $name {
            fn new(pointer: *mut sys::$type) -> Result<Self, CryptoError> {
                NonNull::new(pointer).map_or_else(failed, |pointer| Ok(Self(pointer)))
            }

            pub(crate) fn as_ptr(&self) -> *mut sys::$type {
                self.0.as_ptr()
            }
        }

        impl Drop for $name {
            fn drop(&mut self) {
                // SAFETY: The pointer is owned, and is freed only here.
                unsafe { sys::$free(self.0.as_ptr()) };
            }
        }
    };
}

owned!(
    /// A key, which OpenSSL does not change once it is built.
    Pkey,
    EVP_PKEY,
    EVP_PKEY_free
);
owned!(PkeyContext, EVP_PKEY_CTX, EVP_PKEY_CTX_free);
owned!(DigestContext, EVP_MD_CTX, EVP_MD_CTX_free);
owned!(Digest, EVP_MD, EVP_MD_free);
owned!(Kdf, EVP_KDF, EVP_KDF_free);
owned!(KdfContext, EVP_KDF_CTX, EVP_KDF_CTX_free);
owned!(Mac, EVP_MAC, EVP_MAC_free);
owned!(MacContext, EVP_MAC_CTX, EVP_MAC_CTX_free);
owned!(SignatureAlgorithm, EVP_SIGNATURE, EVP_SIGNATURE_free);
owned!(CipherContext, EVP_CIPHER_CTX, EVP_CIPHER_CTX_free);
owned!(
    /// A big number, which is cleared when it is freed.
    BigNum,
    BIGNUM,
    BN_clear_free
);
owned!(BigNumContext, BN_CTX, BN_CTX_free);
owned!(Group, EC_GROUP, EC_GROUP_free);
owned!(Point, EC_POINT, EC_POINT_free);
owned!(EcdsaSignature, ECDSA_SIG, ECDSA_SIG_free);
owned!(ParamArray, OSSL_PARAM, OSSL_PARAM_free);
owned!(PublicKeyInfoPointer, X509_PUBKEY, X509_PUBKEY_free);
owned!(
    /// A PrivateKeyInfo, whose free clears the private key.
    PrivateKeyInfoPointer,
    PKCS8_PRIV_KEY_INFO,
    PKCS8_PRIV_KEY_INFO_free
);

// SAFETY: OpenSSL keys are reference counted atomically and are not changed once built, so they can be shared and sent
// between threads.
unsafe impl Send for Pkey {}
// SAFETY: See above.
unsafe impl Sync for Pkey {}

impl Clone for Pkey {
    fn clone(&self) -> Self {
        // SAFETY: The key is live; taking a reference keeps it alive for the clone.
        let result = unsafe { sys::EVP_PKEY_up_ref(self.as_ptr()) };
        assert_eq!(result, 1, "OpenSSL takes a reference to a key");
        Self(self.0)
    }
}

/// An OSSL_PARAM, which points at a static name and at a value that lives for `'a`.
#[derive(Clone, Copy)]
#[repr(transparent)]
pub(crate) struct Param<'a> {
    raw: sys::OSSL_PARAM,
    value: PhantomData<&'a ()>,
}

impl Param<'_> {
    fn new(raw: sys::OSSL_PARAM) -> Self {
        Self {
            raw,
            value: PhantomData,
        }
    }
}

/// A list of parameters for OpenSSL, which reads them up to the end marker.
///
/// # Safety
///
/// `as_params_ptr()` must give parameters that end with the end marker, and whose names and values live as long as the
/// list does.
pub(crate) unsafe trait Params {
    fn as_params_ptr(&self) -> *const sys::OSSL_PARAM;
}

// SAFETY: The parameters borrow their values, and the end marker is checked for.
unsafe impl Params for [Param<'_>] {
    fn as_params_ptr(&self) -> *const sys::OSSL_PARAM {
        assert!(
            self.last().is_some_and(|param| param.raw.is_end()),
            "The parameters end with the end marker"
        );
        <[Param<'_>]>::as_ptr(self).cast()
    }
}

// SAFETY: See the slice's implementation.
unsafe impl<const N: usize> Params for [Param<'_>; N] {
    fn as_params_ptr(&self) -> *const sys::OSSL_PARAM {
        self.as_slice().as_params_ptr()
    }
}

// SAFETY: OpenSSL builds the array with the end marker, and the array owns the parameters' values.
unsafe impl Params for ParamArray {
    fn as_params_ptr(&self) -> *const sys::OSSL_PARAM {
        self.as_ptr()
    }
}

pub(crate) fn octet_string_parameter<'a>(name: &'static CStr, bytes: &'a [u8]) -> Param<'a> {
    // SAFETY: Constructing a parameter only records the pointers and length, which the parameter borrows.
    Param::new(unsafe {
        sys::OSSL_PARAM_construct_octet_string(name.as_ptr(), bytes.as_ptr().cast_mut().cast(), bytes.len())
    })
}

pub(crate) fn utf8_parameter(name: &'static CStr, value: &'static CStr) -> Param<'static> {
    // SAFETY: Constructing a parameter only records the pointers, to static strings, and the value's length.
    Param::new(unsafe {
        sys::OSSL_PARAM_construct_utf8_string(name.as_ptr(), value.as_ptr().cast_mut(), value.count_bytes())
    })
}

pub(crate) fn uint_parameter<'a>(name: &'static CStr, value: &'a mut c_uint) -> Param<'a> {
    // SAFETY: Constructing a parameter only records the pointers, which the parameter borrows.
    Param::new(unsafe { sys::OSSL_PARAM_construct_uint(name.as_ptr(), value) })
}

pub(crate) fn int_parameter<'a>(name: &'static CStr, value: &'a mut c_int) -> Param<'a> {
    // SAFETY: Constructing a parameter only records the pointers, which the parameter borrows.
    Param::new(unsafe { sys::OSSL_PARAM_construct_int(name.as_ptr(), value) })
}

pub(crate) fn end_parameter() -> Param<'static> {
    // SAFETY: Constructing the end marker takes no pointers.
    Param::new(unsafe { sys::OSSL_PARAM_construct_end() })
}

/// Parameters that hold big numbers, which an OSSL_PARAM_BLD builds.
// NB: The builder reads each big number and octet string when it builds the parameters, so the numbers live as long as
//     the builder, and the octet strings outlive it.
pub(crate) struct ParamBuilder<'a> {
    builder: NonNull<sys::OSSL_PARAM_BLD>,
    numbers: Vec<BigNum>,
    octets: PhantomData<&'a [u8]>,
}

impl Drop for ParamBuilder<'_> {
    fn drop(&mut self) {
        // SAFETY: The builder is owned, and is freed only here.
        unsafe { sys::OSSL_PARAM_BLD_free(self.builder.as_ptr()) };
    }
}

impl<'a> ParamBuilder<'a> {
    pub(crate) fn new() -> Result<Self, CryptoError> {
        // SAFETY: Making a builder takes no arguments.
        let builder = NonNull::new(unsafe { sys::OSSL_PARAM_BLD_new() }).map_or_else(failed, Ok)?;
        Ok(Self {
            builder,
            numbers: Vec::new(),
            octets: PhantomData,
        })
    }

    pub(crate) fn push_number(&mut self, name: &'static CStr, bytes: &[u8], secret: bool) -> Result<(), CryptoError> {
        let number = BigNum::from_bytes(bytes, secret)?;
        // SAFETY: The builder and number are live, and the number lives as long as the builder.
        check(unsafe { sys::OSSL_PARAM_BLD_push_BN(self.builder.as_ptr(), name.as_ptr(), number.as_ptr()) })?;
        self.numbers.push(number);
        Ok(())
    }

    pub(crate) fn push_utf8(&mut self, name: &'static CStr, value: &'static CStr) -> Result<(), CryptoError> {
        // SAFETY: The builder is live, and the strings are static.
        check(unsafe { sys::OSSL_PARAM_BLD_push_utf8_string(self.builder.as_ptr(), name.as_ptr(), value.as_ptr(), 0) })
    }

    pub(crate) fn push_octets(&mut self, name: &'static CStr, bytes: &'a [u8]) -> Result<(), CryptoError> {
        // SAFETY: The builder is live, and the bytes outlive it.
        check(unsafe {
            sys::OSSL_PARAM_BLD_push_octet_string(
                self.builder.as_ptr(),
                name.as_ptr(),
                bytes.as_ptr().cast(),
                bytes.len(),
            )
        })
    }

    pub(crate) fn push_uint(&mut self, name: &'static CStr, value: c_uint) -> Result<(), CryptoError> {
        // SAFETY: The builder is live, and the name is static.
        check(unsafe { sys::OSSL_PARAM_BLD_push_uint(self.builder.as_ptr(), name.as_ptr(), value) })
    }

    pub(crate) fn build(&self) -> Result<ParamArray, CryptoError> {
        // SAFETY: The builder and the numbers and octet strings it was given are live.
        ParamArray::new(unsafe { sys::OSSL_PARAM_BLD_to_param(self.builder.as_ptr()) })
    }
}

impl BigNum {
    pub(crate) fn from_bytes(bytes: &[u8], secret: bool) -> Result<Self, CryptoError> {
        // SAFETY: Making a number takes no arguments.
        let number = Self::new(unsafe { if secret { sys::BN_secure_new() } else { sys::BN_new() } })?;
        // SAFETY: The bytes are live for the call, and the number is owned.
        if unsafe { sys::BN_bin2bn(bytes.as_ptr(), int_length(bytes.len())?, number.as_ptr()) }.is_null() {
            return failed();
        }
        Ok(number)
    }

    pub(crate) fn num_bits(&self) -> usize {
        // SAFETY: The number is live.
        usize::try_from(unsafe { sys::BN_num_bits(self.as_ptr()) }).unwrap_or_default()
    }

    pub(crate) fn is_odd(&self) -> bool {
        // SAFETY: The number is live.
        unsafe { sys::BN_is_odd(self.as_ptr()) == 1 }
    }

    pub(crate) fn is_less_than(&self, other: &Self) -> bool {
        // SAFETY: Both numbers are live.
        unsafe { sys::BN_cmp(self.as_ptr(), other.as_ptr()) < 0 }
    }

    pub(crate) fn incremented(&self) -> Result<Self, CryptoError> {
        // SAFETY: Making a number takes no arguments.
        let result = Self::new(unsafe { sys::BN_new() })?;
        // SAFETY: All three numbers are live; BN_value_one() is static.
        check(unsafe { sys::BN_add(result.as_ptr(), self.as_ptr(), sys::BN_value_one()) })?;
        Ok(result)
    }

    /// The big-endian bytes of the number, padded with zeros to `length`.
    pub(crate) fn to_padded(&self, length: usize) -> Result<SecretBytes, CryptoError> {
        let mut bytes = SecretBytes::new(vec![0; length]);
        // SAFETY: The buffer holds `length` bytes, and BN_bn2binpad fails if the number does not fit.
        if unsafe { sys::BN_bn2binpad(self.as_ptr(), bytes.as_mut_ptr(), int_length(length)?) } < 0 {
            return failed();
        }
        Ok(bytes)
    }

    /// The big-endian bytes of the number, without leading zeros.
    pub(crate) fn to_bytes(&self) -> Result<SecretBytes, CryptoError> {
        self.to_padded(self.num_bits().div_ceil(8))
    }
}

impl Pkey {
    fn context(&self) -> Result<PkeyContext, CryptoError> {
        // SAFETY: The key is live; a non-null result is owned.
        PkeyContext::new(unsafe { sys::EVP_PKEY_CTX_new_from_pkey(ptr::null_mut(), self.as_ptr(), ptr::null()) })
    }

    /// The key of the algorithm `name` that `params` describe, with the parts that `selection` names.
    pub(crate) fn from_data(
        name: &CStr,
        selection: c_int,
        params: &(impl Params + ?Sized),
    ) -> Result<Self, CryptoError> {
        // SAFETY: The name is live; a non-null result is owned.
        let context =
            PkeyContext::new(unsafe { sys::EVP_PKEY_CTX_new_from_name(ptr::null_mut(), name.as_ptr(), ptr::null()) })?;
        // SAFETY: The context is live.
        check(unsafe { sys::EVP_PKEY_fromdata_init(context.as_ptr()) })?;
        let mut key = ptr::null_mut();
        // SAFETY: The context and the parameters are live, and OpenSSL only reads the parameters; a non-null result is
        //         owned.
        check(unsafe {
            sys::EVP_PKEY_fromdata(
                context.as_ptr(),
                &raw mut key,
                selection,
                params.as_params_ptr().cast_mut(),
            )
        })?;
        Self::new(key)
    }

    /// A new key of the algorithm `name`, with the generation parameters `params`.
    pub(crate) fn generate(name: &CStr, params: &(impl Params + ?Sized)) -> Result<Self, CryptoError> {
        // SAFETY: The name is live; a non-null result is owned.
        let context =
            PkeyContext::new(unsafe { sys::EVP_PKEY_CTX_new_from_name(ptr::null_mut(), name.as_ptr(), ptr::null()) })?;
        // SAFETY: The context is live.
        check(unsafe { sys::EVP_PKEY_keygen_init(context.as_ptr()) })?;
        // SAFETY: The context and the parameters are live.
        check(unsafe { sys::EVP_PKEY_CTX_set_params(context.as_ptr(), params.as_params_ptr()) })?;
        let mut key = ptr::null_mut();
        // SAFETY: The context is live; a non-null result is owned.
        check(unsafe { sys::EVP_PKEY_generate(context.as_ptr(), &raw mut key) })?;
        Self::new(key)
    }

    pub(crate) fn from_raw_public_key(name: &CStr, bytes: &[u8]) -> Result<Self, CryptoError> {
        // SAFETY: The name and bytes are live for the call; a non-null result is owned.
        Self::new(unsafe {
            sys::EVP_PKEY_new_raw_public_key_ex(
                ptr::null_mut(),
                name.as_ptr(),
                ptr::null(),
                bytes.as_ptr(),
                bytes.len(),
            )
        })
    }

    pub(crate) fn from_raw_private_key(name: &CStr, bytes: &[u8]) -> Result<Self, CryptoError> {
        // SAFETY: The name and bytes are live for the call; a non-null result is owned.
        Self::new(unsafe {
            sys::EVP_PKEY_new_raw_private_key_ex(
                ptr::null_mut(),
                name.as_ptr(),
                ptr::null(),
                bytes.as_ptr(),
                bytes.len(),
            )
        })
    }

    pub(crate) fn number(&self, name: &CStr) -> Option<BigNum> {
        let mut number = ptr::null_mut();
        // SAFETY: The key and name are live; a non-null result is owned.
        let result = unsafe { sys::EVP_PKEY_get_bn_param(self.as_ptr(), name.as_ptr(), &raw mut number) };
        if result != 1 {
            // SAFETY: BN_clear_free accepts null; clearing the error queue takes no arguments.
            unsafe {
                sys::BN_clear_free(number);
                sys::ERR_clear_error();
            }
            return None;
        }
        BigNum::new(number).ok()
    }

    /// The octet string parameter `name` of the key, such as an ML-DSA or ML-KEM key's seed.
    pub(crate) fn octet_string(&self, name: &CStr) -> Result<SecretBytes, CryptoError> {
        let mut size = 0;
        // SAFETY: A null buffer asks for the size, which is written to `size`.
        check(unsafe {
            sys::EVP_PKEY_get_octet_string_param(self.as_ptr(), name.as_ptr(), ptr::null_mut(), 0, &raw mut size)
        })?;
        let mut bytes = SecretBytes::new(vec![0; size]);
        // SAFETY: The buffer holds `size` bytes.
        check(unsafe {
            sys::EVP_PKEY_get_octet_string_param(self.as_ptr(), name.as_ptr(), bytes.as_mut_ptr(), size, &raw mut size)
        })?;
        bytes.truncate(size);
        Ok(bytes)
    }

    pub(crate) fn raw_public_key(&self) -> Result<Vec<u8>, CryptoError> {
        let mut size = 0;
        // SAFETY: A null buffer asks for the size.
        check(unsafe { sys::EVP_PKEY_get_raw_public_key(self.as_ptr(), ptr::null_mut(), &raw mut size) })?;
        let mut bytes = vec![0; size];
        // SAFETY: The buffer holds `size` bytes.
        check(unsafe { sys::EVP_PKEY_get_raw_public_key(self.as_ptr(), bytes.as_mut_ptr(), &raw mut size) })?;
        bytes.truncate(size);
        Ok(bytes)
    }

    pub(crate) fn raw_private_key(&self) -> Result<SecretBytes, CryptoError> {
        let mut size = 0;
        // SAFETY: A null buffer asks for the size.
        check(unsafe { sys::EVP_PKEY_get_raw_private_key(self.as_ptr(), ptr::null_mut(), &raw mut size) })?;
        let mut bytes = SecretBytes::new(vec![0; size]);
        // SAFETY: The buffer holds `size` bytes.
        check(unsafe { sys::EVP_PKEY_get_raw_private_key(self.as_ptr(), bytes.as_mut_ptr(), &raw mut size) })?;
        bytes.truncate(size);
        Ok(bytes)
    }

    /// The size of the key's signatures and ciphertexts, in bytes.
    pub(crate) fn size(&self) -> usize {
        // SAFETY: The key is live.
        usize::try_from(unsafe { sys::EVP_PKEY_get_size(self.as_ptr()) }).unwrap_or_default()
    }

    pub(crate) fn bits(&self) -> u32 {
        // SAFETY: The key is live.
        u32::try_from(unsafe { sys::EVP_PKEY_get_bits(self.as_ptr()) }).unwrap_or_default()
    }

    /// The DER SubjectPublicKeyInfo of the key, as OpenSSL writes it.
    pub(crate) fn public_key_info(&self) -> Result<Vec<u8>, CryptoError> {
        let encoded = encoding(self.as_ptr(), sys::i2d_PUBKEY);
        if encoded.is_empty() {
            return failed();
        }
        Ok(encoded.into_vec())
    }

    /// The DER encoding of the key's own structure, such as an RSAPrivateKey, as OpenSSL writes it.
    fn private_key_structure(&self) -> SecretBytes {
        encoding(self.as_ptr(), sys::i2d_PrivateKey)
    }
}

/// Runs OpenSSL's public, private and pairwise checks of `key`, the last two only when asked for.
pub(crate) fn check_key(key: &Pkey, private: bool, pairwise: bool) -> Result<bool, CryptoError> {
    let context = key.context()?;
    // NB: OpenSSL's pairwise check of RSA factors is unbounded, so it must only run on a key whose modulus passed the
    //     public check.
    // SAFETY: The context is live for each call.
    let valid = unsafe {
        sys::EVP_PKEY_public_check(context.as_ptr()) == 1
            && (!private || sys::EVP_PKEY_private_check(context.as_ptr()) == 1)
            && (!pairwise || sys::EVP_PKEY_pairwise_check(context.as_ptr()) == 1)
    };
    // NB: A failed check leaves its reasons queued.
    // SAFETY: Clearing the error queue takes no arguments.
    unsafe { sys::ERR_clear_error() };
    Ok(valid)
}

/// Signs `message` with `key`, hashing it with the digest `digest_name` unless the algorithm takes none. The parameters
/// end with the end marker.
pub(crate) fn digest_sign(
    key: &Pkey,
    digest_name: Option<&CStr>,
    params: &[Param<'_>],
    message: &[u8],
) -> Result<Vec<u8>, CryptoError> {
    // SAFETY: Making a context takes no arguments.
    let context = DigestContext::new(unsafe { sys::EVP_MD_CTX_new() })?;
    // SAFETY: The context, key, name and parameters are live for the call.
    check(unsafe {
        sys::EVP_DigestSignInit_ex(
            context.as_ptr(),
            ptr::null_mut(),
            digest_name.map_or(ptr::null(), CStr::as_ptr),
            ptr::null_mut(),
            ptr::null(),
            key.as_ptr(),
            params.as_params_ptr(),
        )
    })?;
    let mut size = 0;
    // SAFETY: A null signature asks for its size.
    check(unsafe {
        sys::EVP_DigestSign(
            context.as_ptr(),
            ptr::null_mut(),
            &raw mut size,
            message.as_ptr(),
            message.len(),
        )
    })?;
    let mut signature = vec![0; size];
    // SAFETY: The signature buffer holds `size` bytes.
    check(unsafe {
        sys::EVP_DigestSign(
            context.as_ptr(),
            signature.as_mut_ptr(),
            &raw mut size,
            message.as_ptr(),
            message.len(),
        )
    })?;
    signature.truncate(size);
    Ok(signature)
}

/// Verifies `signature` of `message` with `key`, as `digest_sign()` makes it.
pub(crate) fn digest_verify(
    key: &Pkey,
    digest_name: Option<&CStr>,
    params: &[Param<'_>],
    message: &[u8],
    signature: &[u8],
) -> Result<bool, CryptoError> {
    // SAFETY: Making a context takes no arguments.
    let context = DigestContext::new(unsafe { sys::EVP_MD_CTX_new() })?;
    // SAFETY: The context, key, name and parameters are live for the call.
    check(unsafe {
        sys::EVP_DigestVerifyInit_ex(
            context.as_ptr(),
            ptr::null_mut(),
            digest_name.map_or(ptr::null(), CStr::as_ptr),
            ptr::null_mut(),
            ptr::null(),
            key.as_ptr(),
            params.as_params_ptr(),
        )
    })?;
    // SAFETY: The context and both buffers are live for the call.
    check_bool(unsafe {
        sys::EVP_DigestVerify(
            context.as_ptr(),
            signature.as_ptr(),
            signature.len(),
            message.as_ptr(),
            message.len(),
        )
    })
}

/// Signs `digest`, a message's hash, with `key`.
pub(crate) fn sign_digest(key: &Pkey, digest: &[u8]) -> Result<Vec<u8>, CryptoError> {
    let context = key.context()?;
    // SAFETY: The context is live.
    check(unsafe { sys::EVP_PKEY_sign_init(context.as_ptr()) })?;
    let mut size = 0;
    // SAFETY: A null signature asks for its size.
    check(unsafe {
        sys::EVP_PKEY_sign(
            context.as_ptr(),
            ptr::null_mut(),
            &raw mut size,
            digest.as_ptr(),
            digest.len(),
        )
    })?;
    let mut signature = vec![0; size];
    // SAFETY: The signature buffer holds `size` bytes.
    check(unsafe {
        sys::EVP_PKEY_sign(
            context.as_ptr(),
            signature.as_mut_ptr(),
            &raw mut size,
            digest.as_ptr(),
            digest.len(),
        )
    })?;
    signature.truncate(size);
    Ok(signature)
}

/// Verifies `signature` of `digest`, a message's hash, with `key`.
pub(crate) fn verify_digest(key: &Pkey, digest: &[u8], signature: &[u8]) -> Result<bool, CryptoError> {
    let context = key.context()?;
    // SAFETY: The context is live.
    check(unsafe { sys::EVP_PKEY_verify_init(context.as_ptr()) })?;
    // SAFETY: The context and both buffers are live for the call.
    check_bool(unsafe {
        sys::EVP_PKEY_verify(
            context.as_ptr(),
            signature.as_ptr(),
            signature.len(),
            digest.as_ptr(),
            digest.len(),
        )
    })
}

/// Signs `message` with `key` by the message signature algorithm `algorithm` (such as "ML-DSA-44"), with a context
/// string when one is given.
pub(crate) fn sign_message(
    key: &Pkey,
    algorithm: &CStr,
    message: &[u8],
    context: &[u8],
) -> Result<Vec<u8>, CryptoError> {
    let key_context = key.context()?;
    // SAFETY: The name is live; a non-null result is owned.
    let signature_algorithm =
        SignatureAlgorithm::new(unsafe { sys::EVP_SIGNATURE_fetch(ptr::null_mut(), algorithm.as_ptr(), ptr::null()) })?;
    let params = optional_context_string_parameters(context);
    // SAFETY: The context, algorithm, parameters and buffers are live for each call, and the signature buffer holds the
    //         size that the first call gives.
    unsafe {
        check(sys::EVP_PKEY_sign_message_init(
            key_context.as_ptr(),
            signature_algorithm.as_ptr(),
            params.as_params_ptr(),
        ))?;
        let mut size = 0;
        check(sys::EVP_PKEY_sign(
            key_context.as_ptr(),
            ptr::null_mut(),
            &raw mut size,
            message.as_ptr(),
            message.len(),
        ))?;
        let mut signature = vec![0; size];
        check(sys::EVP_PKEY_sign(
            key_context.as_ptr(),
            signature.as_mut_ptr(),
            &raw mut size,
            message.as_ptr(),
            message.len(),
        ))?;
        signature.truncate(size);
        Ok(signature)
    }
}

/// Verifies `signature` of `message` with `key`, as `sign_message()` makes it.
pub(crate) fn verify_message(
    key: &Pkey,
    algorithm: &CStr,
    message: &[u8],
    signature: &[u8],
    context: &[u8],
) -> Result<bool, CryptoError> {
    let key_context = key.context()?;
    // SAFETY: The name is live; a non-null result is owned.
    let signature_algorithm =
        SignatureAlgorithm::new(unsafe { sys::EVP_SIGNATURE_fetch(ptr::null_mut(), algorithm.as_ptr(), ptr::null()) })?;
    let params = optional_context_string_parameters(context);
    // SAFETY: The context, algorithm, parameters and buffers are live for each call.
    unsafe {
        check(sys::EVP_PKEY_verify_message_init(
            key_context.as_ptr(),
            signature_algorithm.as_ptr(),
            params.as_params_ptr(),
        ))?;
        check_bool(sys::EVP_PKEY_verify(
            key_context.as_ptr(),
            signature.as_ptr(),
            signature.len(),
            message.as_ptr(),
            message.len(),
        ))
    }
}

pub(crate) fn context_string_parameters(context: &[u8]) -> [Param<'_>; 2] {
    [octet_string_parameter(c"context-string", context), end_parameter()]
}

fn optional_context_string_parameters(context: &[u8]) -> [Param<'_>; 2] {
    if context.is_empty() {
        [end_parameter(), end_parameter()]
    } else {
        context_string_parameters(context)
    }
}

/// Encrypts `input` with `key`, with the parameters `params`, which end with the end marker.
pub(crate) fn encrypt(key: &Pkey, params: &[Param<'_>], input: &[u8]) -> Result<Vec<u8>, CryptoError> {
    let context = key.context()?;
    // SAFETY: The context and parameters are live.
    check(unsafe { sys::EVP_PKEY_encrypt_init_ex(context.as_ptr(), params.as_params_ptr()) })?;
    let mut size = 0;
    // SAFETY: A null output asks for its size.
    check(unsafe {
        sys::EVP_PKEY_encrypt(
            context.as_ptr(),
            ptr::null_mut(),
            &raw mut size,
            input.as_ptr(),
            input.len(),
        )
    })?;
    let mut output = vec![0; size];
    // SAFETY: The output holds `size` bytes.
    check(unsafe {
        sys::EVP_PKEY_encrypt(
            context.as_ptr(),
            output.as_mut_ptr(),
            &raw mut size,
            input.as_ptr(),
            input.len(),
        )
    })?;
    output.truncate(size);
    Ok(output)
}

/// Decrypts `input` with `key`, with the parameters `params`, which end with the end marker.
pub(crate) fn decrypt(key: &Pkey, params: &[Param<'_>], input: &[u8]) -> Result<Vec<u8>, CryptoError> {
    let context = key.context()?;
    // SAFETY: The context and parameters are live.
    check(unsafe { sys::EVP_PKEY_decrypt_init_ex(context.as_ptr(), params.as_params_ptr()) })?;
    let mut size = 0;
    // SAFETY: A null output asks for its size.
    check(unsafe {
        sys::EVP_PKEY_decrypt(
            context.as_ptr(),
            ptr::null_mut(),
            &raw mut size,
            input.as_ptr(),
            input.len(),
        )
    })?;
    let mut output = vec![0; size];
    // SAFETY: The output holds `size` bytes.
    check(unsafe {
        sys::EVP_PKEY_decrypt(
            context.as_ptr(),
            output.as_mut_ptr(),
            &raw mut size,
            input.as_ptr(),
            input.len(),
        )
    })?;
    output.truncate(size);
    Ok(output)
}

/// The secret that `key` and the public key `peer` agree on. OpenSSL checks the peer when `validate_peer` is set.
pub(crate) fn derive(key: &Pkey, peer: &Pkey, validate_peer: bool) -> Result<SecretBytes, CryptoError> {
    let context = key.context()?;
    // SAFETY: The context and peer are live for each call, and the output holds the size the first call gives.
    unsafe {
        check(sys::EVP_PKEY_derive_init(context.as_ptr()))?;
        check(sys::EVP_PKEY_derive_set_peer_ex(
            context.as_ptr(),
            peer.as_ptr(),
            c_int::from(validate_peer),
        ))?;
        let mut size = 0;
        check(sys::EVP_PKEY_derive(context.as_ptr(), ptr::null_mut(), &raw mut size))?;
        let mut secret = SecretBytes::new(vec![0; size]);
        check(sys::EVP_PKEY_derive(
            context.as_ptr(),
            secret.as_mut_ptr(),
            &raw mut size,
        ))?;
        secret.truncate(size);
        Ok(secret)
    }
}

/// A new shared key encapsulated to `key`, with its ciphertext.
pub(crate) fn encapsulate(key: &Pkey) -> Result<(SecretBytes, Vec<u8>), CryptoError> {
    let context = key.context()?;
    // SAFETY: The context and buffers are live for each call, and the buffers hold the sizes the first call gives.
    unsafe {
        check(sys::EVP_PKEY_encapsulate_init(context.as_ptr(), ptr::null()))?;
        let (mut ciphertext_size, mut shared_key_size) = (0, 0);
        check(sys::EVP_PKEY_encapsulate(
            context.as_ptr(),
            ptr::null_mut(),
            &raw mut ciphertext_size,
            ptr::null_mut(),
            &raw mut shared_key_size,
        ))?;
        let mut ciphertext = vec![0; ciphertext_size];
        let mut shared_key = SecretBytes::new(vec![0; shared_key_size]);
        check(sys::EVP_PKEY_encapsulate(
            context.as_ptr(),
            ciphertext.as_mut_ptr(),
            &raw mut ciphertext_size,
            shared_key.as_mut_ptr(),
            &raw mut shared_key_size,
        ))?;
        ciphertext.truncate(ciphertext_size);
        shared_key.truncate(shared_key_size);
        Ok((shared_key, ciphertext))
    }
}

/// The shared key that `ciphertext` encapsulates to `key`.
pub(crate) fn decapsulate(key: &Pkey, ciphertext: &[u8]) -> Result<SecretBytes, CryptoError> {
    let context = key.context()?;
    // SAFETY: The context and buffers are live for each call, and the buffer holds the size the first call gives.
    unsafe {
        check(sys::EVP_PKEY_decapsulate_init(context.as_ptr(), ptr::null()))?;
        let mut size = 0;
        check(sys::EVP_PKEY_decapsulate(
            context.as_ptr(),
            ptr::null_mut(),
            &raw mut size,
            ciphertext.as_ptr(),
            ciphertext.len(),
        ))?;
        let mut shared_key = SecretBytes::new(vec![0; size]);
        check(sys::EVP_PKEY_decapsulate(
            context.as_ptr(),
            shared_key.as_mut_ptr(),
            &raw mut size,
            ciphertext.as_ptr(),
            ciphertext.len(),
        ))?;
        shared_key.truncate(size);
        Ok(shared_key)
    }
}

/// The uncompressed public point of the private value `scalar` on the curve `nid`, or None if `scalar` is not in
/// [1, n - 1].
pub(crate) fn ec_public_point(nid: c_int, scalar: &[u8]) -> Result<Option<Vec<u8>>, CryptoError> {
    // SAFETY: Making a group takes the curve's NID; a non-null result is owned.
    let group = Group::new(unsafe { sys::EC_GROUP_new_by_curve_name(nid) })?;
    let scalar = BigNum::from_bytes(scalar, true)?;
    // SAFETY: The group is live, and owns its order.
    let order = unsafe { sys::EC_GROUP_get0_order(group.as_ptr()) };
    // SAFETY: Both numbers are live.
    // NB: SEC 1 requires d to be in [1, n − 1].
    if scalar.num_bits() == 0 || unsafe { sys::BN_cmp(scalar.as_ptr(), order) } >= 0 {
        return Ok(None);
    }

    // SAFETY: Making a context takes no arguments.
    let context = BigNumContext::new(unsafe { sys::BN_CTX_new() })?;
    // SAFETY: The group is live; a non-null result is owned.
    let point = Point::new(unsafe { sys::EC_POINT_new(group.as_ptr()) })?;
    // SAFETY: The group, point, scalar and context are live.
    check(unsafe {
        sys::EC_POINT_mul(
            group.as_ptr(),
            point.as_ptr(),
            scalar.as_ptr(),
            ptr::null(),
            ptr::null(),
            context.as_ptr(),
        )
    })?;
    // SAFETY: A null buffer asks for the size.
    let size = unsafe {
        sys::EC_POINT_point2oct(
            group.as_ptr(),
            point.as_ptr(),
            sys::POINT_CONVERSION_UNCOMPRESSED,
            ptr::null_mut(),
            0,
            context.as_ptr(),
        )
    };
    let mut encoded = vec![0; size];
    // SAFETY: The buffer holds `size` bytes.
    let written = unsafe {
        sys::EC_POINT_point2oct(
            group.as_ptr(),
            point.as_ptr(),
            sys::POINT_CONVERSION_UNCOMPRESSED,
            encoded.as_mut_ptr(),
            size,
            context.as_ptr(),
        )
    };
    if size == 0 || written != size {
        return failed();
    }
    Ok(Some(encoded))
}

/// The ECDSA signature of `digest` by `key`: r and then s, each of `scalar_size` bytes.
pub(crate) fn ecdsa_sign(key: &Pkey, digest: &[u8], scalar_size: usize) -> Result<Vec<u8>, CryptoError> {
    let encoded = sign_digest(key, digest)?;
    let mut cursor = encoded.as_ptr();
    // SAFETY: The cursor points into `encoded`, of the length given; a non-null result is owned.
    let signature = EcdsaSignature::new(unsafe {
        sys::d2i_ECDSA_SIG(ptr::null_mut(), &raw mut cursor, int_length(encoded.len())?.into())
    })?;
    let (mut r, mut s) = (ptr::null(), ptr::null());
    // SAFETY: The signature is live, and owns r and s.
    unsafe { sys::ECDSA_SIG_get0(signature.as_ptr(), &raw mut r, &raw mut s) };
    let mut bytes = vec![0; scalar_size * 2];
    let (r_bytes, s_bytes) = bytes.split_at_mut(scalar_size);
    for (number, number_bytes) in [(r, r_bytes), (s, s_bytes)] {
        // SAFETY: The number is live while the signature is, and the buffer holds `scalar_size` bytes.
        if unsafe { sys::BN_bn2binpad(number, number_bytes.as_mut_ptr(), int_length(scalar_size)?) } < 0 {
            return failed();
        }
    }
    Ok(bytes)
}

/// Verifies the ECDSA signature of `digest` with `key`, which is r and then s, each of half its length.
pub(crate) fn ecdsa_verify(key: &Pkey, digest: &[u8], signature: &[u8]) -> Result<bool, CryptoError> {
    let (r, s) = signature.split_at(signature.len() / 2);
    // SAFETY: Making a signature takes no arguments.
    let signature = EcdsaSignature::new(unsafe { sys::ECDSA_SIG_new() })?;
    let r = BigNum::from_bytes(r, false)?;
    let s = BigNum::from_bytes(s, false)?;
    // SAFETY: The signature takes r and s; they are not freed here once it has.
    check(unsafe { sys::ECDSA_SIG_set0(signature.as_ptr(), r.as_ptr(), s.as_ptr()) })?;
    core::mem::forget(r);
    core::mem::forget(s);
    let encoded = encoding(signature.as_ptr(), sys::i2d_ECDSA_SIG);
    if encoded.is_empty() {
        return failed();
    }
    verify_digest(key, digest, &encoded)
}

/// The digest `name` (such as "SHA256") of `data`.
pub(crate) fn digest(name: &CStr, data: &[u8]) -> Result<Vec<u8>, CryptoError> {
    let mut output = vec![0; 64];
    let mut size = 0;
    // SAFETY: The name and data are live, and the output holds 64 bytes, the size of the largest digest.
    check(unsafe {
        sys::EVP_Q_digest(
            ptr::null_mut(),
            name.as_ptr(),
            ptr::null(),
            data.as_ptr().cast(),
            data.len(),
            output.as_mut_ptr(),
            &raw mut size,
        )
    })?;
    output.truncate(size);
    Ok(output)
}

/// `output.len()` bytes of the extendable-output function `name` (such as "SHAKE128") of the parts of `data`.
pub(crate) fn extendable_output(name: &CStr, data: &[&[u8]], output: &mut [u8]) -> Result<(), CryptoError> {
    // SAFETY: The name is live; a non-null result is owned.
    let digest = Digest::new(unsafe { sys::EVP_MD_fetch(ptr::null_mut(), name.as_ptr(), ptr::null()) })?;
    // SAFETY: Making a context takes no arguments.
    let context = DigestContext::new(unsafe { sys::EVP_MD_CTX_new() })?;
    // SAFETY: The context, digest and buffers are live for each call.
    unsafe {
        check(sys::EVP_DigestInit_ex(
            context.as_ptr(),
            digest.as_ptr(),
            ptr::null_mut(),
        ))?;
        for part in data {
            check(sys::EVP_DigestUpdate(
                context.as_ptr(),
                part.as_ptr().cast(),
                part.len(),
            ))?;
        }
        check(sys::EVP_DigestFinalXOF(
            context.as_ptr(),
            output.as_mut_ptr(),
            output.len(),
        ))
    }
}

/// The MAC `name` (such as "HMAC") of `message` under `key`, `output_size` bytes long, with the parameters `params`,
/// which end with the end marker.
pub(crate) fn mac(
    name: &CStr,
    key: &[u8],
    message: &[u8],
    output_size: usize,
    params: &[Param<'_>],
) -> Result<Vec<u8>, CryptoError> {
    // SAFETY: The name is live; a non-null result is owned.
    let mac = Mac::new(unsafe { sys::EVP_MAC_fetch(ptr::null_mut(), name.as_ptr(), ptr::null()) })?;
    // SAFETY: The MAC is live; a non-null result is owned.
    let context = MacContext::new(unsafe { sys::EVP_MAC_CTX_new(mac.as_ptr()) })?;
    // SAFETY: The context, key, parameters and buffers are live for each call, and the output holds `output_size` bytes.
    unsafe {
        check(sys::EVP_MAC_init(
            context.as_ptr(),
            key.as_ptr(),
            key.len(),
            params.as_params_ptr(),
        ))?;
        check(sys::EVP_MAC_update(context.as_ptr(), message.as_ptr(), message.len()))?;
        let mut output = vec![0; output_size];
        let mut written = 0;
        check(sys::EVP_MAC_final(
            context.as_ptr(),
            output.as_mut_ptr(),
            &raw mut written,
            output.len(),
        ))?;
        if written != output_size {
            return failed();
        }
        Ok(output)
    }
}

/// Derives `output.len()` bytes with the KDF `name` (such as "HKDF") and the parameters `params`, which end with the
/// end marker.
// NB: The parameters point at the caller's buffers, so OpenSSL copies secrets only into the KDF context, which clears
//     them.
pub(crate) fn kdf(name: &CStr, params: &[Param<'_>], output: &mut [u8]) -> Result<(), CryptoError> {
    // SAFETY: The name is live; a non-null result is owned.
    let kdf = Kdf::new(unsafe { sys::EVP_KDF_fetch(ptr::null_mut(), name.as_ptr(), ptr::null()) })?;
    // SAFETY: The KDF is live; a non-null result is owned.
    let context = KdfContext::new(unsafe { sys::EVP_KDF_CTX_new(kdf.as_ptr()) })?;
    // SAFETY: The context, parameters and output are live, and the output holds its length.
    check(unsafe {
        sys::EVP_KDF_derive(
            context.as_ptr(),
            output.as_mut_ptr(),
            output.len(),
            params.as_params_ptr(),
        )
    })
}

pub(crate) fn fill_with_random(bytes: &mut [u8]) -> Result<(), CryptoError> {
    for chunk in bytes.chunks_mut(c_int::MAX as usize) {
        // SAFETY: The chunk holds the length given.
        check(unsafe { sys::RAND_bytes(chunk.as_mut_ptr(), int_length(chunk.len())?) })?;
    }
    Ok(())
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) struct Cipher(*const sys::EVP_CIPHER);

impl Cipher {
    pub(crate) fn from_static(getter: unsafe extern "C" fn() -> *const sys::EVP_CIPHER) -> Self {
        // SAFETY: The getters return static ciphers.
        Self(unsafe { getter() })
    }
}

// NB: OpenSSL takes cipher input lengths as int, so input goes to it in pieces of at most this size.
const MAXIMUM_UPDATE_LENGTH: usize = 1 << 30;
// NB: Cipher updates write at most a block, or a key wrap's integrity check, more than their input.
const MAXIMUM_UPDATE_GROWTH: usize = 32;

impl CipherContext {
    pub(crate) fn create() -> Result<Self, CryptoError> {
        // SAFETY: Making a context takes no arguments.
        Self::new(unsafe { sys::EVP_CIPHER_CTX_new() })
    }

    /// Sets up the context to encrypt or decrypt with whichever of the cipher, key and IV are given. The key and IV must
    /// be of the sizes that the cipher, or the cipher the context already has, takes.
    pub(crate) fn init(
        &mut self,
        encrypt: bool,
        cipher: Option<Cipher>,
        key: Option<&[u8]>,
        iv: Option<&[u8]>,
    ) -> Result<(), CryptoError> {
        // SAFETY: The cipher is static, and the context is live.
        let (key_length, iv_length) = unsafe {
            match cipher {
                Some(cipher) => (
                    sys::EVP_CIPHER_get_key_length(cipher.0),
                    sys::EVP_CIPHER_get_iv_length(cipher.0),
                ),
                None => (
                    sys::EVP_CIPHER_CTX_get_key_length(self.as_ptr()),
                    sys::EVP_CIPHER_CTX_get_iv_length(self.as_ptr()),
                ),
            }
        };
        let has_length = |buffer: Option<&[u8]>, length: c_int| {
            buffer.is_none_or(|buffer| usize::try_from(length).is_ok_and(|length| buffer.len() == length))
        };
        if !has_length(key, key_length) || !has_length(iv, iv_length) {
            return Err(CryptoError::Invalid(
                "The key or IV is not of the size the cipher takes",
            ));
        }
        // SAFETY: The context, cipher and buffers are live for the call, and the buffers are of the sizes OpenSSL reads.
        check(unsafe {
            sys::EVP_CipherInit_ex(
                self.as_ptr(),
                cipher.map_or(ptr::null(), |cipher| cipher.0),
                ptr::null_mut(),
                key.map_or(ptr::null(), <[u8]>::as_ptr),
                iv.map_or(ptr::null(), <[u8]>::as_ptr),
                c_int::from(encrypt),
            )
        })
    }

    fn control(&mut self, kind: c_int, argument: usize, pointer: *mut u8) -> Result<(), CryptoError> {
        // SAFETY: The context is live, and the pointer is null or holds `argument` bytes.
        check(unsafe { sys::EVP_CIPHER_CTX_ctrl(self.as_ptr(), kind, int_length(argument)?, pointer.cast()) })
    }

    /// Makes a block cipher take only whole blocks, and add no padding to them. This is for after `init`.
    pub(crate) fn disable_padding(&mut self) -> Result<(), CryptoError> {
        // SAFETY: The context is live.
        check(unsafe { sys::EVP_CIPHER_CTX_set_padding(self.as_ptr(), 0) })
    }

    pub(crate) fn set_iv_length(&mut self, length: usize) -> Result<(), CryptoError> {
        self.control(sys::EVP_CTRL_AEAD_SET_IVLEN, length, ptr::null_mut())
    }

    pub(crate) fn set_tag_length(&mut self, length: usize) -> Result<(), CryptoError> {
        self.control(sys::EVP_CTRL_AEAD_SET_TAG, length, ptr::null_mut())
    }

    pub(crate) fn set_tag(&mut self, tag: &[u8]) -> Result<(), CryptoError> {
        // NB: OpenSSL copies the tag, and does not write to it.
        self.control(sys::EVP_CTRL_AEAD_SET_TAG, tag.len(), tag.as_ptr().cast_mut())
    }

    pub(crate) fn tag(&mut self, length: usize) -> Result<Vec<u8>, CryptoError> {
        let mut tag = vec![0; length];
        self.control(sys::EVP_CTRL_AEAD_GET_TAG, length, tag.as_mut_ptr())?;
        Ok(tag)
    }

    pub(crate) fn allow_key_wrapping(&mut self) {
        // SAFETY: The context is live.
        unsafe { sys::EVP_CIPHER_CTX_set_flags(self.as_ptr(), sys::EVP_CIPHER_CTX_FLAG_WRAP_ALLOW) };
    }

    pub(crate) fn add_additional_data(&mut self, data: &[u8]) -> Result<(), CryptoError> {
        for chunk in data.chunks(MAXIMUM_UPDATE_LENGTH) {
            let mut written = 0;
            // SAFETY: A null output passes the chunk as additional data.
            check(unsafe {
                sys::EVP_CipherUpdate(
                    self.as_ptr(),
                    ptr::null_mut(),
                    &raw mut written,
                    chunk.as_ptr(),
                    int_length(chunk.len())?,
                )
            })?;
        }
        Ok(())
    }

    pub(crate) fn update(&mut self, input: &[u8], output: &mut Vec<u8>) -> Result<(), CryptoError> {
        // NB: Reserving room for all of the output first keeps a reallocation from leaving a copy of it behind.
        output.reserve(input.len() + MAXIMUM_UPDATE_GROWTH);
        for chunk in input.chunks(MAXIMUM_UPDATE_LENGTH) {
            self.update_whole(chunk, output)?;
        }
        Ok(())
    }

    /// Passes all of `input` to OpenSSL in one update, as key wrapping needs, which fails for input whose length does
    /// not fit in an int.
    pub(crate) fn update_whole(&mut self, input: &[u8], output: &mut Vec<u8>) -> Result<(), CryptoError> {
        let input_length = int_length(input.len())?;
        output.reserve(input.len() + MAXIMUM_UPDATE_GROWTH);
        let mut written = 0;
        // SAFETY: The output has room for the input and the most that an update writes beyond it.
        check(unsafe {
            sys::EVP_CipherUpdate(
                self.as_ptr(),
                output.as_mut_ptr().add(output.len()),
                &raw mut written,
                input.as_ptr(),
                input_length,
            )
        })?;
        // SAFETY: OpenSSL wrote `written` bytes past the end.
        unsafe { output.set_len(output.len() + usize::try_from(written).unwrap_or_default()) };
        Ok(())
    }

    pub(crate) fn finalize(&mut self, output: &mut Vec<u8>) -> Result<(), CryptoError> {
        output.reserve(MAXIMUM_UPDATE_GROWTH);
        let mut written = 0;
        // SAFETY: The output has room for the most that finalizing writes.
        check(unsafe {
            sys::EVP_CipherFinal_ex(self.as_ptr(), output.as_mut_ptr().add(output.len()), &raw mut written)
        })?;
        // SAFETY: OpenSSL wrote `written` bytes past the end.
        unsafe { output.set_len(output.len() + usize::try_from(written).unwrap_or_default()) };
        Ok(())
    }
}

/// The DER encoding that an i2d function gives of `object`, or nothing if it fails.
fn encoding<T>(object: *const T, i2d: unsafe extern "C" fn(*const T, *mut *mut u8) -> c_int) -> SecretBytes {
    // SAFETY: A null output asks for the length.
    let length = unsafe { i2d(object, ptr::null_mut()) };
    let Ok(length) = usize::try_from(length) else {
        // SAFETY: Clearing the error queue takes no arguments.
        unsafe { sys::ERR_clear_error() };
        return SecretBytes::new(Vec::new());
    };
    let mut encoded = SecretBytes::new(vec![0; length]);
    let mut output = encoded.as_mut_ptr();
    // SAFETY: The buffer holds the length the first call gave.
    unsafe { i2d(object, &raw mut output) };
    encoded
}

pub(crate) struct AlgorithmIdentifier {
    // The contents of the DER encoding of the algorithm's object identifier.
    pub(crate) oid: Vec<u8>,
    // The parameters, when they are an object identifier, such as the named curve of an EC key.
    pub(crate) curve: Option<Vec<u8>>,
    parameters_type: c_int,
}

impl AlgorithmIdentifier {
    pub(crate) fn has_parameters(&self) -> bool {
        self.parameters_type != sys::V_ASN1_UNDEF
    }
}

pub(crate) struct PublicKeyInfo {
    pub(crate) algorithm: AlgorithmIdentifier,
    pub(crate) raw_key: Vec<u8>,
    pub(crate) consumed: usize,
    // The key OpenSSL decoded, which it does not for ML-KEM.
    pub(crate) key: Option<Pkey>,
}

pub(crate) struct PrivateKeyInfo {
    pub(crate) algorithm: AlgorithmIdentifier,
    pub(crate) raw_key: SecretBytes,
    pub(crate) consumed: usize,
    // The key OpenSSL decoded, which it does not for ML-DSA and ML-KEM.
    pub(crate) key: Option<Pkey>,
}

fn object_identifier(object: *const sys::ASN1_OBJECT) -> Vec<u8> {
    // SAFETY: The object is live, and owns the data of the length it gives.
    unsafe { ak::slice_from_raw_parts(sys::OBJ_get0_data(object), sys::OBJ_length(object)).to_vec() }
}

fn algorithm_identifier(algorithm: *const sys::X509_ALGOR) -> AlgorithmIdentifier {
    let mut object = ptr::null();
    let mut parameters_type = 0;
    let mut parameters = ptr::null();
    // SAFETY: The algorithm is live, and the outputs are live locals.
    unsafe {
        sys::X509_ALGOR_get0(
            &raw mut object,
            &raw mut parameters_type,
            &raw mut parameters,
            algorithm,
        );
    };
    let curve = (parameters_type == sys::V_ASN1_OBJECT).then(|| object_identifier(parameters.cast()));
    AlgorithmIdentifier {
        oid: object_identifier(object),
        curve,
        parameters_type,
    }
}

// NB: The structure is in DER, so its SEQUENCE's length takes one byte, or one more than the count in that byte's low
//     bits. Version 0 follows as 02 01 00.
fn has_version_zero(der: &[u8]) -> bool {
    let Some(&length_byte) = der.get(1) else {
        return false;
    };
    let header_size = 2 + if length_byte & 0x80 != 0 {
        usize::from(length_byte & 0x7f)
    } else {
        0
    };
    der.get(header_size..header_size + 3) == Some(&[0x02, 0x01, 0x00])
}

/// Parses the DER SubjectPublicKeyInfo at the start of `data`, which must be the encoding OpenSSL gives back for it.
pub(crate) fn parse_subject_public_key_info(data: &[u8]) -> Result<PublicKeyInfo, &'static str> {
    let invalid = "Invalid SubjectPublicKeyInfo";
    let mut cursor = data.as_ptr();
    // SAFETY: The cursor points into `data`, of the length given; a non-null result is owned.
    let public_key_info = PublicKeyInfoPointer::new(unsafe {
        sys::d2i_X509_PUBKEY(
            ptr::null_mut(),
            &raw mut cursor,
            c_long::try_from(data.len()).unwrap_or(c_long::MAX),
        )
    })
    .map_err(|_| invalid)?;
    // SAFETY: d2i advanced the cursor within `data`.
    let consumed = unsafe { cursor.offset_from(data.as_ptr()) } as usize;
    let input = &data[..consumed];

    let mut algorithm = ptr::null_mut();
    let mut key_bits = ptr::null();
    let mut key_size = 0;
    // SAFETY: The structure is live, and the outputs are live locals.
    if unsafe {
        sys::X509_PUBKEY_get0_param(
            ptr::null_mut(),
            &raw mut key_bits,
            &raw mut key_size,
            &raw mut algorithm,
            public_key_info.as_ptr(),
        )
    } != 1
    {
        return failed().map_err(|_| invalid);
    }
    let algorithm = algorithm_identifier(algorithm);
    let key_size = usize::try_from(key_size).unwrap_or_default();
    // SAFETY: OpenSSL gives the key's bits, of `key_size` bytes, which the structure owns.
    let raw_key = unsafe { ak::slice_from_raw_parts(key_bits, key_size) }.to_vec();

    // NB: OpenSSL runs the ML-KEM encapsulation key check when it decodes the key, but Web Crypto runs it at
    //     encapsulation. So an ML-KEM key is only checked to be in DER, without parameters.
    if matches!(KeyAlgorithm::from_oid(&algorithm.oid), Some(KeyAlgorithm::MlKem(_))) {
        if algorithm.has_parameters() {
            return Err("The algorithm takes no parameters");
        }
        // NB: The BIT STRING's contents end the encoding, and are its count of unused bits followed by the key.
        if *encoding(public_key_info.as_ptr(), sys::i2d_X509_PUBKEY) != *input || input[consumed - key_size - 1] != 0 {
            return Err("SubjectPublicKeyInfo is not in DER");
        }
        return Ok(PublicKeyInfo {
            algorithm,
            raw_key,
            consumed,
            key: None,
        });
    }

    // SAFETY: The structure is live; a non-null result is owned.
    let key = Pkey::new(unsafe { sys::X509_PUBKEY_get(public_key_info.as_ptr()) })
        .map_err(|_| "Invalid or unsupported public key")?;

    // NB: OpenSSL accepts BER, and parameters that an algorithm does not use. Its own encoding of the key is the DER
    //     encoding with the parameters that the algorithm takes, so any other input is not valid.
    if key
        .public_key_info()
        .map_err(|_| "SubjectPublicKeyInfo is not in DER")?
        != input
    {
        return Err("SubjectPublicKeyInfo is not in DER");
    }
    Ok(PublicKeyInfo {
        algorithm,
        raw_key,
        consumed,
        key: Some(key),
    })
}

/// Parses the DER PrivateKeyInfo at the start of `data`, which must be the encoding OpenSSL gives back for it.
pub(crate) fn parse_private_key_info(data: &[u8]) -> Result<PrivateKeyInfo, &'static str> {
    let invalid = "Invalid PrivateKeyInfo";
    let mut cursor = data.as_ptr();
    // SAFETY: The cursor points into `data`, of the length given; a non-null result is owned.
    let private_key_info = PrivateKeyInfoPointer::new(unsafe {
        sys::d2i_PKCS8_PRIV_KEY_INFO(
            ptr::null_mut(),
            &raw mut cursor,
            c_long::try_from(data.len()).unwrap_or(c_long::MAX),
        )
    })
    .map_err(|_| invalid)?;
    // SAFETY: d2i advanced the cursor within `data`.
    let consumed = unsafe { cursor.offset_from(data.as_ptr()) } as usize;
    let input = &data[..consumed];

    if *encoding(private_key_info.as_ptr(), sys::i2d_PKCS8_PRIV_KEY_INFO) != *input {
        return Err("PrivateKeyInfo is not in DER");
    }

    // NB: OpenSSL also takes the version 1 OneAsymmetricKey of RFC 5958.
    if !has_version_zero(input) {
        return Err("Unsupported PrivateKeyInfo version");
    }

    let mut algorithm = ptr::null();
    let mut private_key = ptr::null();
    let mut private_key_size = 0;
    // SAFETY: The structure is live, and the outputs are live locals.
    if unsafe {
        sys::PKCS8_pkey_get0(
            ptr::null_mut(),
            &raw mut private_key,
            &raw mut private_key_size,
            &raw mut algorithm,
            private_key_info.as_ptr(),
        )
    } != 1
    {
        return failed().map_err(|_| invalid);
    }
    let algorithm = algorithm_identifier(algorithm);
    let private_key_size = usize::try_from(private_key_size).unwrap_or_default();
    // SAFETY: OpenSSL gives the private key's octets, of `private_key_size` bytes, which the structure owns.
    let raw_key = unsafe { ak::slice_from_raw_parts(private_key, private_key_size) };
    let kind = KeyAlgorithm::from_oid(&algorithm.oid);

    // https://www.rfc-editor.org/rfc/rfc8410#section-9
    // For all of the OIDs, the parameters MUST be absent.
    // https://www.rfc-editor.org/rfc/rfc9881#section-2
    // The contents of the parameters component for each algorithm MUST be absent.
    // NB: ML-KEM's parameters are absent likewise, and the outer encoding check above keeps whatever parameters were
    //     given.
    if !matches!(kind, Some(KeyAlgorithm::Rsa | KeyAlgorithm::Ec)) && algorithm.has_parameters() {
        return Err("The algorithm takes no parameters");
    }

    // NB: The ML-DSA and ML-KEM privateKey choices are parsed after this, since OpenSSL also takes forms that Web Crypto
    //     does not, and refuses an expandedKey that is not valid before Web Crypto can reject that form.
    let mut decoded_key = None;
    if !matches!(kind, Some(KeyAlgorithm::MlDsa(_) | KeyAlgorithm::MlKem(_))) {
        // SAFETY: The structure is live; a non-null result is owned.
        let key = Pkey::new(unsafe { sys::EVP_PKCS82PKEY(private_key_info.as_ptr()) })
            .map_err(|_| "Invalid or unsupported private key")?;

        if kind == Some(KeyAlgorithm::Rsa) {
            // https://www.rfc-editor.org/rfc/rfc3279#section-2.3.1
            // The parameters field MUST have ASN.1 type NULL for this algorithm identifier.
            if algorithm.parameters_type != sys::V_ASN1_NULL {
                return Err("RSA parameters must be NULL");
            }

            // NB: OpenSSL accepts BER in the RSAPrivateKey, so it must be the key's own DER encoding.
            if *key.private_key_structure() != *raw_key {
                return Err("RSAPrivateKey is not in DER");
            }

            // NB: OpenSSL also takes other versions, and otherPrimeInfos even in version 0. Keys of more than two primes
            //     are not supported, as in JWK import.
            if !has_version_zero(raw_key) {
                return Err("Unsupported RSAPrivateKey version");
            }
            if key.number(c"rsa-factor3").is_some() {
                return Err("RSA keys of more than two primes are not supported");
            }
        }
        decoded_key = Some(key);
    }

    // NB: The private key is copied only once it has been checked.
    Ok(PrivateKeyInfo {
        algorithm,
        raw_key: SecretBytes::new(raw_key.to_vec()),
        consumed,
        key: decoded_key,
    })
}
