/*
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteReader.h>
#include <AK/Endian.h>
#include <LibCrypto/AEAD/ChaCha20Poly1305.h>
#include <LibCrypto/Authentication/Poly1305.h>
#include <LibCrypto/Cipher/ChaCha20.h>

namespace Crypto::AEAD {

// https://datatracker.ietf.org/doc/html/rfc8439#section-2.6
ErrorOr<ByteBuffer> ChaCha20Poly1305::poly1305_key()
{
    Crypto::Cipher::ChaCha20 cipher(m_key, m_nonce, 0);
    cipher.generate_block();
    auto state = cipher.block();
    return TRY(ByteBuffer::copy(state.slice(0, 32)));
}

// https://datatracker.ietf.org/doc/html/rfc8439#section-2.8
ErrorOr<ByteBuffer> ChaCha20Poly1305::encrypt(ReadonlyBytes aad, ReadonlyBytes input_plaintext)
{
    // First, a Poly1305 one-time key is generated from the 256-bit key
    // and nonce using the procedure described in Section 2.6.
    auto otk = TRY(poly1305_key());

    // Next, the ChaCha20 encryption function is called to encrypt the
    // plaintext, using the same key and nonce, and with the initial
    // counter set to 1.
    auto ciphertext_buffer = TRY(ByteBuffer::create_zeroed(input_plaintext.size()));
    auto ciphertext = ciphertext_buffer.bytes();
    auto chacha = Crypto::Cipher::ChaCha20(m_key, m_nonce, 1);
    chacha.encrypt(input_plaintext, ciphertext);

    // Finally, the Poly1305 function is called with the Poly1305 key
    // calculated above, and a message constructed as a concatenation of
    // the following:
    auto mac_data = TRY(ByteBuffer::create_zeroed(0));
    auto buffer_size = aad.size() + pad_to_16(aad) + ciphertext_buffer.size() + pad_to_16(ciphertext_buffer) + sizeof(u64) + sizeof(u64);
    mac_data.ensure_capacity(buffer_size);

    // The AAD
    mac_data.append(aad);

    // padding1 -- the padding is up to 15 zero bytes, and it brings
    // the total length so far to an integral multiple of 16.  If the
    // length of the AAD was already an integral multiple of 16 bytes,
    // this field is zero-length.
    for (size_t i = 0; i < pad_to_16(aad); ++i)
        mac_data.append(0);

    // The ciphertext
    mac_data.append(ciphertext);

    // padding2 -- the padding is up to 15 zero bytes, and it brings
    // the total length so far to an integral multiple of 16.  If the
    // length of the ciphertext was already an integral multiple of 16
    // bytes, this field is zero-length.
    for (size_t i = 0; i < pad_to_16(ciphertext); ++i)
        mac_data.append(0);

    u8 placeholder[8] = { 0 };
    // The length of the additional data in octets (as a 64-bit little-endian integer).
    mac_data.append(ReadonlyBytes { placeholder, 8 });
    ByteReader::store(static_cast<u8*>(mac_data.end_pointer()) - sizeof(u64), AK::convert_between_host_and_little_endian(static_cast<u64>(aad.size())));

    // The length of the ciphertext in octets (as a 64-bit little-endian integer).
    mac_data.append(ReadonlyBytes { placeholder, 8 });
    ByteReader::store(static_cast<u8*>(mac_data.end_pointer()) - sizeof(u64), AK::convert_between_host_and_little_endian(static_cast<u64>(ciphertext.size())));

    Crypto::Authentication::Poly1305 mac_function(otk);
    mac_function.update(mac_data.bytes());
    auto tag = TRY(mac_function.digest());

    // The output from the AEAD is the concatenation of:
    auto result = TRY(ByteBuffer::create_zeroed(0));
    result.ensure_capacity(ciphertext.size() + tag.size());

    // A ciphertext of the same length as the plaintext.
    result.append(ciphertext);

    // A 128-bit tag, which is the output of the Poly1305 function.
    result.append(tag);
    return result;
}

// https://datatracker.ietf.org/doc/html/rfc8439#section-2.8
ErrorOr<ByteBuffer> ChaCha20Poly1305::decrypt(ReadonlyBytes aad, ReadonlyBytes ciphertext)
{
    // Decryption is similar with the following differences:
    // o  The roles of ciphertext and plaintext are reversed, so the
    //    ChaCha20 encryption function is applied to the ciphertext,
    //    producing the plaintext.
    // o  The Poly1305 function is still run on the AAD and the ciphertext,
    //    not the plaintext.

    // First, a Poly1305 one-time key is generated from the 256-bit key
    // and nonce using the procedure described in Section 2.6.
    auto otk = TRY(poly1305_key());

    // Next, the ChaCha20 encryption function is called to decrypt the
    // ciphertext, using the same key and nonce, and with the initial
    // counter set to 1.
    auto chacha = Crypto::Cipher::ChaCha20(m_key, m_nonce, 1);
    auto plaintext_buffer = TRY(ByteBuffer::create_zeroed(ciphertext.size()));
    auto plaintext = plaintext_buffer.bytes();
    chacha.encrypt(ciphertext, plaintext);

    // Finally, the Poly1305 function is called with the Poly1305 key
    // calculated above, and a message constructed as a concatenation of
    // the following:
    auto mac_data = TRY(ByteBuffer::create_zeroed(0));
    auto buffer_size = aad.size() + pad_to_16(aad) + ciphertext.size() + pad_to_16(ciphertext) + sizeof(u64) + sizeof(u64);
    mac_data.ensure_capacity(buffer_size);

    // The AAD
    mac_data.append(aad);

    // padding1 -- the padding is up to 15 zero bytes, and it brings
    // the total length so far to an integral multiple of 16.  If the
    // length of the AAD was already an integral multiple of 16 bytes,
    // this field is zero-length.
    for (size_t i = 0; i < pad_to_16(aad); ++i)
        mac_data.append(0);

    // The ciphertext
    mac_data.append(ciphertext);

    // padding2 -- the padding is up to 15 zero bytes, and it brings
    // the total length so far to an integral multiple of 16.  If the
    // length of the ciphertext was already an integral multiple of 16
    // bytes, this field is zero-length.
    for (size_t i = 0; i < pad_to_16(ciphertext); ++i)
        mac_data.append(0);

    u8 placeholder[8] = { 0 };
    // The length of the additional data in octets (as a 64-bit little-endian integer).
    mac_data.append(ReadonlyBytes { placeholder, 8 });
    ByteReader::store(static_cast<u8*>(mac_data.end_pointer()) - sizeof(u64), AK::convert_between_host_and_little_endian(static_cast<u64>(aad.size())));

    // The length of the ciphertext in octets (as a 64-bit little-endian integer).
    mac_data.append(ReadonlyBytes { placeholder, 8 });
    ByteReader::store(static_cast<u8*>(mac_data.end_pointer()) - sizeof(u64), AK::convert_between_host_and_little_endian(static_cast<u64>(ciphertext.size())));

    Crypto::Authentication::Poly1305 mac_function(otk);
    mac_function.update(mac_data.bytes());
    auto tag = TRY(mac_function.digest());

    // The output from the AEAD is the concatenation of:
    auto result = TRY(ByteBuffer::create_zeroed(0));
    result.ensure_capacity(plaintext.size() + tag.size());

    // A plaintext of the same length as the ciphertext.
    result.append(plaintext);

    // A 128-bit tag, which is the output of the Poly1305 function.
    result.append(tag);
    return result;
}

// https://datatracker.ietf.org/doc/html/rfc8439#section-4
bool ChaCha20Poly1305::verify_tag(ReadonlyBytes encrypted, ReadonlyBytes decrypted)
{
    // With online protocols, implementation MUST use a constant-time comparison function rather
    // than relying on optimized but insecure library functions such as the C language's memcmp().
    auto encrypted_tag = encrypted.slice_from_end(16);
    auto decrypted_tag = decrypted.slice_from_end(16);

    if (encrypted_tag.size() != decrypted_tag.size())
        return false;

    auto result = 0;
    for (size_t i = 0; i < encrypted_tag.size(); ++i)
        result |= encrypted_tag[i] ^ decrypted_tag[i];

    return result == 0;
}
}
