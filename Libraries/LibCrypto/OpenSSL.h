/*
 * Copyright (c) 2024, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Format.h>
#include <LibCrypto/BigInt/UnsignedBigInteger.h>
#include <LibCrypto/OpenSSLForward.h>

namespace Crypto {

#define OPENSSL_TRY_PTR(...)                                           \
    ({                                                                 \
        auto* _temporary_result = (__VA_ARGS__);                       \
        if (!_temporary_result) [[unlikely]] {                         \
            auto err = ERR_get_error();                                \
            VERIFY(err);                                               \
            auto* err_message = ERR_error_string(err, nullptr);        \
            dbgln("OpenSSL error: {}", err_message);                   \
            return Error::from_string_literal(#__VA_ARGS__ " failed"); \
        }                                                              \
        _temporary_result;                                             \
    })

#define OPENSSL_TRY(...)                                               \
    ({                                                                 \
        auto _temporary_result = (__VA_ARGS__);                        \
        if (_temporary_result != 1) [[unlikely]] {                     \
            auto err = ERR_get_error();                                \
            VERIFY(err);                                               \
            auto* err_message = ERR_error_string(err, nullptr);        \
            dbgln("OpenSSL error: {}", err_message);                   \
            return Error::from_string_literal(#__VA_ARGS__ " failed"); \
        }                                                              \
        _temporary_result;                                             \
    })

#define OPENSSL_WRAPPER_CLASS(class_name, openssl_type, openssl_prefix) \
    AK_MAKE_NONCOPYABLE(class_name);                                    \
                                                                        \
public:                                                                 \
    static ErrorOr<class_name> wrap(openssl_type* ptr)                  \
    {                                                                   \
        return class_name(OPENSSL_TRY_PTR(ptr));                        \
    }                                                                   \
                                                                        \
    ~class_name()                                                       \
    {                                                                   \
        openssl_prefix##_free(m_ptr);                                   \
    }                                                                   \
                                                                        \
    class_name(class_name&& other)                                      \
        : m_ptr(other.leak_ptr())                                       \
    {                                                                   \
    }                                                                   \
                                                                        \
    class_name& operator=(class_name&& other)                           \
    {                                                                   \
        class_name ptr(move(other));                                    \
        swap(m_ptr, ptr.m_ptr);                                         \
        return *this;                                                   \
    }                                                                   \
                                                                        \
    openssl_type const* ptr() const { return m_ptr; }                   \
    openssl_type* ptr() { return m_ptr; }                               \
                                                                        \
private:                                                                \
    [[nodiscard]] openssl_type* leak_ptr()                              \
    {                                                                   \
        return exchange(m_ptr, nullptr);                                \
    }                                                                   \
                                                                        \
    explicit class_name(openssl_type* ptr)                              \
        : m_ptr(ptr)                                                    \
    {                                                                   \
    }                                                                   \
                                                                        \
    openssl_type* m_ptr { nullptr };

class OpenSSL_BN {
    OPENSSL_WRAPPER_CLASS(OpenSSL_BN, BIGNUM, BN);

public:
    static ErrorOr<OpenSSL_BN> create();
};

class OpenSSL_PKEY {
    OPENSSL_WRAPPER_CLASS(OpenSSL_PKEY, EVP_PKEY, EVP_PKEY);

public:
    static ErrorOr<OpenSSL_PKEY> create();
};

class OpenSSL_PKEY_CTX {
    OPENSSL_WRAPPER_CLASS(OpenSSL_PKEY_CTX, EVP_PKEY_CTX, EVP_PKEY_CTX);
};

class OpenSSL_MD_CTX {
    OPENSSL_WRAPPER_CLASS(OpenSSL_MD_CTX, EVP_MD_CTX, EVP_MD_CTX);

public:
    static ErrorOr<OpenSSL_MD_CTX> create();
};

#undef OPENSSL_WRAPPER_CLASS

ErrorOr<OpenSSL_BN> unsigned_big_integer_to_openssl_bignum(UnsignedBigInteger const& integer);
ErrorOr<UnsignedBigInteger> openssl_bignum_to_unsigned_big_integer(OpenSSL_BN const& bn);

}
