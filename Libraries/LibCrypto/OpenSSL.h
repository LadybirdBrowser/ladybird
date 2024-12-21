/*
 * Copyright (c) 2024, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Format.h>

#include <openssl/err.h>
#include <openssl/evp.h>

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

class OpenSSL_PKEY {
    AK_MAKE_NONCOPYABLE(OpenSSL_PKEY);

public:
    static ErrorOr<OpenSSL_PKEY> wrap(EVP_PKEY* ptr)
    {
        return OpenSSL_PKEY(OPENSSL_TRY_PTR(ptr));
    }

    static ErrorOr<OpenSSL_PKEY> create()
    {
        return OpenSSL_PKEY(OPENSSL_TRY_PTR(EVP_PKEY_new()));
    }

    ~OpenSSL_PKEY()
    {
        EVP_PKEY_free(m_ptr);
    }

    OpenSSL_PKEY(OpenSSL_PKEY&& other)
        : m_ptr(other.leak_ptr())
    {
    }

    OpenSSL_PKEY& operator=(OpenSSL_PKEY&& other)
    {
        OpenSSL_PKEY ptr(move(other));
        swap(m_ptr, ptr.m_ptr);
        return *this;
    }

    EVP_PKEY const* ptr() const { return m_ptr; }
    EVP_PKEY* ptr() { return m_ptr; }

private:
    [[nodiscard]] EVP_PKEY* leak_ptr()
    {
        return exchange(m_ptr, nullptr);
    }

    explicit OpenSSL_PKEY(EVP_PKEY* ptr)
        : m_ptr(ptr)
    {
    }

    EVP_PKEY* m_ptr { nullptr };
};

class OpenSSL_MD_CTX {
    AK_MAKE_NONCOPYABLE(OpenSSL_MD_CTX);

public:
    static ErrorOr<OpenSSL_MD_CTX> wrap(EVP_MD_CTX* ptr)
    {
        return OpenSSL_MD_CTX(OPENSSL_TRY_PTR(ptr));
    }

    static ErrorOr<OpenSSL_MD_CTX> create()
    {
        return OpenSSL_MD_CTX(OPENSSL_TRY_PTR(EVP_MD_CTX_new()));
    }

    OpenSSL_MD_CTX(OpenSSL_MD_CTX&& other)
        : m_ptr(other.leak_ptr())
    {
    }

    OpenSSL_MD_CTX& operator=(OpenSSL_MD_CTX&& other)
    {
        OpenSSL_MD_CTX ptr(move(other));
        swap(m_ptr, ptr.m_ptr);
        return *this;
    }

    ~OpenSSL_MD_CTX()
    {
        EVP_MD_CTX_free(m_ptr);
    }

    EVP_MD_CTX const* ptr() const { return m_ptr; }
    EVP_MD_CTX* ptr() { return m_ptr; }

private:
    [[nodiscard]] EVP_MD_CTX* leak_ptr()
    {
        return exchange(m_ptr, nullptr);
    }

    explicit OpenSSL_MD_CTX(EVP_MD_CTX* ptr)
        : m_ptr(ptr)
    {
    }

    EVP_MD_CTX* m_ptr { nullptr };
};

}
