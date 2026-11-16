/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <LibCrypto/OpenSSL.h>

#include <openssl/bn.h>
#include <openssl/evp.h>

namespace Crypto {

ErrorOr<OpenSSL_BN> OpenSSL_BN::create()
{
    return OpenSSL_BN(OPENSSL_TRY_PTR(BN_new()));
}

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

ErrorOr<OpenSSL_CIPHER_CTX> OpenSSL_CIPHER_CTX::create()
{
    return OpenSSL_CIPHER_CTX(OPENSSL_TRY_PTR(EVP_CIPHER_CTX_new()));
}

ErrorOr<OpenSSL_BN> unsigned_big_integer_to_openssl_bignum(UnsignedBigInteger const& integer)
{
    auto bn = TRY(OpenSSL_BN::create());
    auto buf = TRY(ByteBuffer::create_uninitialized(integer.byte_length()));
    auto result = integer.export_data(buf.bytes());
    OPENSSL_TRY_PTR(BN_bin2bn(result.data(), result.size(), bn.ptr()));
    return bn;
}

ErrorOr<UnsignedBigInteger> openssl_bignum_to_unsigned_big_integer(OpenSSL_BN const& bn)
{
    auto size = BN_num_bytes(bn.ptr());
    auto buf = TRY(ByteBuffer::create_uninitialized(size));
    BN_bn2bin(bn.ptr(), buf.bytes().data());
    return UnsignedBigInteger::import_data(buf);
}

ErrorOr<ByteBuffer> get_byte_buffer_param_from_key(OpenSSL_PKEY& key, char const* key_name)
{
    size_t size;
    OPENSSL_TRY(EVP_PKEY_get_octet_string_param(key.ptr(), key_name, nullptr, 0, &size));

    auto buffer = TRY(ByteBuffer::create_uninitialized(size));

    OPENSSL_TRY(EVP_PKEY_get_octet_string_param(key.ptr(), key_name, buffer.data(), buffer.size(), &size));
    return buffer;
}

}
