/*
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Memory.h>
#include <LibCrypto/KeyHandle.h>

namespace Crypto {

struct CryptoKeyHandleFields {
    u8 handle_tag { 0 };
    Array<ReadonlyBytes, 8> handle_buffers;
    size_t handle_buffer_count { 0 };
    bool has_ec_parameters { false };
    ReadonlySpan<u32> ec_parameters;
    bool has_ec_public_key { false };
};

extern "C" {
void crypto_key_destroy_handle(KeyHandle*);
KeyHandle* crypto_key_create_byte_handle(ReadonlyBytes);
KeyHandle* crypto_key_create_okp_handle(bool, ReadonlyBytes);
void crypto_key_okp_handle_bytes(KeyHandle const*, ReadonlyBytes*);
void crypto_key_byte_handle_bytes(KeyHandle const*, ReadonlyBytes*);
void crypto_key_encode_handle(KeyHandle const*, void*, void (*)(void*, CryptoKeyHandleFields const*));
bool crypto_key_decode_handle(CryptoKeyHandleFields const*, KeyHandle**);
}

void crypto_key_destroy_handle(KeyHandle* handle)
{
    handle->visit(
        [](ByteBuffer& data) { secure_zero(data.data(), data.size()); },
        [](OKPPublicKey& data) { secure_zero(data.bytes.data(), data.bytes.size()); },
        [](OKPPrivateKey& data) { secure_zero(data.bytes.data(), data.bytes.size()); },
        [](::Crypto::PK::RSAPrivateKey& data) { data.clear_sensitive_data(); },
        [](::Crypto::PK::ECPrivateKey& data) { data.clear_sensitive_data(); },
        [](::Crypto::PK::MLDSAPrivateKey& data) { data.clear_sensitive_data(); },
        [](::Crypto::PK::MLKEMPrivateKey& data) { data.clear_sensitive_data(); },
        [](auto&) { });
    delete handle;
}

KeyHandle* crypto_key_create_byte_handle(ReadonlyBytes bytes)
{
    return new KeyHandle(MUST(ByteBuffer::copy(bytes)));
}

void crypto_key_byte_handle_bytes(KeyHandle const* handle, ReadonlyBytes* bytes)
{
    *bytes = handle->get<ByteBuffer>().bytes();
}

KeyHandle* crypto_key_create_okp_handle(bool is_private, ReadonlyBytes bytes)
{
    auto key_bytes = MUST(ByteBuffer::copy(bytes));
    if (is_private)
        return new KeyHandle(OKPPrivateKey { move(key_bytes) });
    return new KeyHandle(OKPPublicKey { move(key_bytes) });
}

void crypto_key_okp_handle_bytes(KeyHandle const* handle, ReadonlyBytes* bytes)
{
    if (handle->has<OKPPrivateKey>())
        *bytes = handle->get<OKPPrivateKey>().bytes;
    else
        *bytes = handle->get<OKPPublicKey>().bytes;
}

static_assert(to_underlying(HandleTag::ECPrivateKey) == 4);
static_assert(to_underlying(HandleTag::OKPPrivateKey) == 10);

static ByteBuffer export_big_integer(::Crypto::UnsignedBigInteger const& value)
{
    auto buffer = MUST(ByteBuffer::create_zeroed(value.byte_length()));
    auto written = value.export_data(buffer.bytes());
    VERIFY(written.size() == buffer.size());
    return buffer;
}

void crypto_key_encode_handle(KeyHandle const* handle, void* state, void (*steps)(void*, CryptoKeyHandleFields const*))
{
    CryptoKeyHandleFields fields;
    // NB: The inline capacity holds every buffer that a handle exports, so that none moves while the fields borrow it.
    Vector<ByteBuffer, 8> exported;
    auto add = [&](ReadonlyBytes bytes) { fields.handle_buffers[fields.handle_buffer_count++] = bytes; };
    auto add_exported = [&](ByteBuffer buffer) {
        exported.unchecked_append(move(buffer));
        add(exported.last().bytes());
    };
    handle->visit(
        [&](ByteBuffer const& buffer) {
            fields.handle_tag = to_underlying(HandleTag::ByteBuffer);
            add(buffer.bytes());
        },
        [&](::Crypto::PK::RSAPublicKey const& rsa_key) {
            fields.handle_tag = to_underlying(HandleTag::RSAPublicKey);
            add_exported(export_big_integer(rsa_key.modulus()));
            add_exported(export_big_integer(rsa_key.public_exponent()));
        },
        [&](::Crypto::PK::RSAPrivateKey const& rsa_key) {
            fields.handle_tag = to_underlying(HandleTag::RSAPrivateKey);
            add_exported(export_big_integer(rsa_key.modulus()));
            add_exported(export_big_integer(rsa_key.private_exponent()));
            add_exported(export_big_integer(rsa_key.public_exponent()));
            add_exported(export_big_integer(rsa_key.prime1()));
            add_exported(export_big_integer(rsa_key.prime2()));
            add_exported(export_big_integer(rsa_key.exponent1()));
            add_exported(export_big_integer(rsa_key.exponent2()));
            add_exported(export_big_integer(rsa_key.coefficient()));
        },
        [&](::Crypto::PK::ECPublicKey const& ec_key) {
            fields.handle_tag = to_underlying(HandleTag::ECPublicKey);
            add_exported(MUST(ec_key.x_bytes()));
            add_exported(MUST(ec_key.y_bytes()));
        },
        [&](::Crypto::PK::ECPrivateKey const& ec_key) {
            fields.handle_tag = to_underlying(HandleTag::ECPrivateKey);
            add_exported(MUST(ec_key.d_bytes()));
            fields.has_ec_parameters = ec_key.parameters().has_value();
            if (fields.has_ec_parameters)
                fields.ec_parameters = ec_key.parameters()->span();
            fields.has_ec_public_key = ec_key.public_key().has_value();
            if (fields.has_ec_public_key) {
                add_exported(MUST(ec_key.public_key()->x_bytes()));
                add_exported(MUST(ec_key.public_key()->y_bytes()));
            }
        },
        [&](::Crypto::PK::MLDSAPublicKey const& mldsa_key) {
            fields.handle_tag = to_underlying(HandleTag::MLDSAPublicKey);
            add(mldsa_key.public_key().bytes());
        },
        [&](::Crypto::PK::MLDSAPrivateKey const& mldsa_key) {
            fields.handle_tag = to_underlying(HandleTag::MLDSAPrivateKey);
            add(mldsa_key.seed().bytes());
            add(mldsa_key.public_key().bytes());
            add(mldsa_key.private_key().bytes());
        },
        [&](::Crypto::PK::MLKEMPublicKey const& mlkem_key) {
            fields.handle_tag = to_underlying(HandleTag::MLKEMPublicKey);
            add_exported(mlkem_key.public_key());
        },
        [&](::Crypto::PK::MLKEMPrivateKey const& mlkem_key) {
            fields.handle_tag = to_underlying(HandleTag::MLKEMPrivateKey);
            add(mlkem_key.seed().bytes());
            add(mlkem_key.public_key().bytes());
            add(mlkem_key.private_key().bytes());
        },
        [&](OKPPublicKey const& okp_key) {
            fields.handle_tag = to_underlying(HandleTag::OKPPublicKey);
            add(okp_key.bytes.bytes());
        },
        [&](OKPPrivateKey const& okp_key) {
            fields.handle_tag = to_underlying(HandleTag::OKPPrivateKey);
            add(okp_key.bytes.bytes());
        });

    steps(state, &fields);

    for (auto& buffer : exported)
        secure_zero(buffer.data(), buffer.size());
}

static ErrorOr<KeyHandle> handle_from_fields(CryptoKeyHandleFields const& fields)
{
    auto buffer = [&](size_t index) {
        VERIFY(index < fields.handle_buffer_count);
        return fields.handle_buffers[index];
    };
    auto copy = [&](size_t index) -> ErrorOr<ByteBuffer> {
        return TRY(ByteBuffer::copy(buffer(index)));
    };
    auto big_integer = [&](size_t index) { return ::Crypto::UnsignedBigInteger::import_data(buffer(index)); };

    switch (static_cast<HandleTag>(fields.handle_tag)) {
    case HandleTag::ByteBuffer:
        return TRY(copy(0));
    case HandleTag::RSAPublicKey:
        return ::Crypto::PK::RSAPublicKey { big_integer(0), big_integer(1) };
    case HandleTag::RSAPrivateKey:
        return ::Crypto::PK::RSAPrivateKey { big_integer(0), big_integer(1), big_integer(2), big_integer(3), big_integer(4), big_integer(5), big_integer(6), big_integer(7) };
    case HandleTag::ECPublicKey:
        return ::Crypto::PK::ECPublicKey { big_integer(0), big_integer(1), buffer(0).size() };
    case HandleTag::ECPrivateKey: {
        Optional<::Crypto::ASN1::ObjectIdentifier> parameters;
        if (fields.has_ec_parameters) {
            ::Crypto::ASN1::ObjectIdentifier identifier;
            TRY(identifier.try_append(fields.ec_parameters.data(), fields.ec_parameters.size()));
            parameters = move(identifier);
        }
        Optional<::Crypto::PK::ECPublicKey> public_key;
        if (fields.has_ec_public_key)
            public_key = ::Crypto::PK::ECPublicKey { big_integer(1), big_integer(2), buffer(1).size() };
        return ::Crypto::PK::ECPrivateKey { big_integer(0), buffer(0).size(), move(parameters), move(public_key) };
    }
    case HandleTag::MLDSAPublicKey:
        return ::Crypto::PK::MLDSAPublicKey { TRY(copy(0)) };
    case HandleTag::MLDSAPrivateKey:
        return ::Crypto::PK::MLDSAPrivateKey { TRY(copy(0)), TRY(copy(1)), TRY(copy(2)) };
    case HandleTag::MLKEMPublicKey:
        return ::Crypto::PK::MLKEMPublicKey { TRY(copy(0)) };
    case HandleTag::MLKEMPrivateKey:
        return ::Crypto::PK::MLKEMPrivateKey { TRY(copy(0)), TRY(copy(1)), TRY(copy(2)) };
    case HandleTag::OKPPublicKey:
        return OKPPublicKey { TRY(copy(0)) };
    case HandleTag::OKPPrivateKey:
        return OKPPrivateKey { TRY(copy(0)) };
    }
    VERIFY_NOT_REACHED();
}

bool crypto_key_decode_handle(CryptoKeyHandleFields const* fields, KeyHandle** out_handle)
{
    auto handle = handle_from_fields(*fields);
    if (handle.is_error())
        return false;
    *out_handle = new KeyHandle(handle.release_value());
    return true;
}

}
