/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/OpenSSL.h>

#include <openssl/bn.h>
#include <openssl/evp.h>

namespace Crypto {

ErrorOr<OpenSSL_PKEY> OpenSSL_PKEY::create()
{
    return OpenSSL_PKEY(OPENSSL_TRY_PTR(EVP_PKEY_new()));
}

OpenSSL_SharedPKEY::OpenSSL_SharedPKEY(OpenSSL_PKEY&& key)
    : m_ptr(key.leak_ptr())
{
}

OpenSSL_SharedPKEY::OpenSSL_SharedPKEY(OpenSSL_SharedPKEY const& other)
    : m_ptr(other.m_ptr)
{
    if (m_ptr)
        VERIFY(EVP_PKEY_up_ref(m_ptr) == 1);
}

OpenSSL_SharedPKEY::OpenSSL_SharedPKEY(OpenSSL_SharedPKEY&& other)
    : m_ptr(exchange(other.m_ptr, nullptr))
{
}

OpenSSL_SharedPKEY& OpenSSL_SharedPKEY::operator=(OpenSSL_SharedPKEY const& other)
{
    OpenSSL_SharedPKEY copy(other);
    swap(m_ptr, copy.m_ptr);
    return *this;
}

OpenSSL_SharedPKEY& OpenSSL_SharedPKEY::operator=(OpenSSL_SharedPKEY&& other)
{
    OpenSSL_SharedPKEY moved(move(other));
    swap(m_ptr, moved.m_ptr);
    return *this;
}

OpenSSL_SharedPKEY::~OpenSSL_SharedPKEY()
{
    EVP_PKEY_free(m_ptr);
}

ErrorOr<OpenSSL_MD_CTX> OpenSSL_MD_CTX::create()
{
    return OpenSSL_MD_CTX(OPENSSL_TRY_PTR(EVP_MD_CTX_new()));
}

}
