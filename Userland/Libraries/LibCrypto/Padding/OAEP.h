/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteReader.h>
#include <AK/Endian.h>
#include <AK/Function.h>
#include <AK/Random.h>
#include <LibCrypto/BigInt/UnsignedBigInteger.h>

namespace Crypto::Padding {

// https://datatracker.ietf.org/doc/html/rfc2437#section-9.1.1
class OAEP {
public:
    // https://datatracker.ietf.org/doc/html/rfc2437#section-9.1.1.1
    template<typename HashFunction, typename MaskGenerationFunction>
    static ErrorOr<ByteBuffer> encode(ReadonlyBytes message, ReadonlyBytes parameters, size_t length, Function<void(Bytes)> seed_function = fill_with_random)
    {
        // FIXME: 1. If the length of P is greater than the input limitation for the
        // hash function (2^61-1 octets for SHA-1) then output "parameter string
        // too long" and stop.

        // 2. If ||M|| > emLen - 2hLen - 1 then output "message too long" and stop.
        auto h_len = HashFunction::digest_size();
        auto max_message_size = length - (2 * h_len) - 1;
        if (message.size() > max_message_size)
            return Error::from_string_literal("message too long");

        // 3. Generate an octet string PS consisting of emLen-||M||-2hLen-1 zero octets. The length of PS may be 0.
        auto padding_size = length - message.size() - (2 * h_len) - 1;
        auto ps = TRY(ByteBuffer::create_zeroed(padding_size));

        // 4. Let pHash = Hash(P), an octet string of length hLen.
        HashFunction hash;
        hash.update(parameters);
        auto digest = hash.digest();
        auto p_hash = digest.bytes();

        // 5. Concatenate pHash, PS, the message M, and other padding to form a data block DB as: DB = pHash || PS || 01 || M
        auto db = TRY(ByteBuffer::create_uninitialized(0));
        TRY(db.try_append(p_hash));
        TRY(db.try_append(ps.bytes()));
        TRY(db.try_append(0x01));
        TRY(db.try_append(message));

        // 6. Generate a random octet string seed of length hLen.
        auto seed = TRY(ByteBuffer::create_uninitialized(h_len));
        seed_function(seed);

        // 7. Let dbMask = MGF(seed, emLen-hLen).
        auto db_mask = TRY(MaskGenerationFunction::template mgf1<HashFunction>(seed, length - h_len));

        // 8. Let maskedDB = DB \xor dbMask.
        auto masked_db = TRY(ByteBuffer::xor_buffers(db, db_mask));

        // 9. Let seedMask = MGF(maskedDB, hLen).
        auto seed_mask = TRY(MaskGenerationFunction::template mgf1<HashFunction>(masked_db, h_len));

        // 10. Let maskedSeed = seed \xor seedMask.
        auto masked_seed = TRY(ByteBuffer::xor_buffers(seed, seed_mask));

        // 11. Let EM = maskedSeed || maskedDB.
        auto em = TRY(ByteBuffer::create_uninitialized(0));
        TRY(em.try_append(masked_seed));
        TRY(em.try_append(masked_db));

        // 12. Output EM.
        return em;
    }

    // https://www.rfc-editor.org/rfc/rfc3447#section-7.1.1
    template<typename HashFunction, typename MaskGenerationFunction>
    static ErrorOr<ByteBuffer> eme_encode(ReadonlyBytes message, ReadonlyBytes label, u32 rsa_modulus_n, Function<void(Bytes)> seed_function = fill_with_random)
    {
        // FIXME: 1. If the length of L is greater than the input limitation for the
        //           hash function (2^61 - 1 octets for SHA-1), output "label too
        //           long" and stop.

        // 2. If mLen > k - 2hLen - 2, output "message too long" and stop.
        auto m_len = message.size();
        auto k = rsa_modulus_n;
        auto h_len = HashFunction::digest_size();
        auto max_message_size = k - (2 * h_len) - 2;

        if (m_len > max_message_size)
            return Error::from_string_view("message too long"sv);

        // 3. If the label L is not provided, let L be the empty string. Let lHash = Hash(L), an octet string of length hLen.
        HashFunction hash;
        hash.update(label);
        auto digest = hash.digest();
        auto l_hash = digest.bytes();

        // 4. Generate an octet string PS consisting of k - mLen - 2hLen - 2 zero octets.  The length of PS may be zero.
        auto ps_size = k - m_len - (2 * h_len) - 2;
        auto ps = TRY(ByteBuffer::create_zeroed(ps_size));

        // 5. Concatenate lHash, PS, a single octet with hexadecimal value 0x01, and the message M
        //    to form a data block DB of length k - hLen - 1 octets as DB = lHash || PS || 0x01 || M.
        auto db = TRY(ByteBuffer::create_uninitialized(0));
        TRY(db.try_append(l_hash));
        TRY(db.try_append(ps.bytes()));
        TRY(db.try_append(0x01));
        TRY(db.try_append(message));

        // 6. Generate a random octet string seed of length hLen.
        auto seed = TRY(ByteBuffer::create_uninitialized(h_len));
        seed_function(seed);

        // 7. Let dbMask = MGF(seed, k - hLen - 1).
        auto db_mask = TRY(MaskGenerationFunction::template mgf1<HashFunction>(seed, k - h_len - 1));

        // 8. Let maskedDB = DB \xor dbMask.
        auto masked_db = TRY(ByteBuffer::xor_buffers(db, db_mask));

        // 9. Let seedMask = MGF(maskedDB, hLen).
        auto seed_mask = TRY(MaskGenerationFunction::template mgf1<HashFunction>(masked_db, h_len));

        // 10. Let maskedSeed = seed \xor seedMask.
        auto masked_seed = TRY(ByteBuffer::xor_buffers(seed, seed_mask));

        // 11. Concatenate a single octet with hexadecimal value 0x00, maskedSeed, and maskedDB
        //     to form an encoded message EM of length k octets as EM = 0x00 || maskedSeed || maskedDB.
        auto em = TRY(ByteBuffer::create_uninitialized(0));
        TRY(em.try_append(0x00));
        TRY(em.try_append(masked_seed));
        TRY(em.try_append(masked_db));

        // 12. Output EM.
        return em;
    }

    // https://datatracker.ietf.org/doc/html/rfc2437#section-9.1.1.2
    template<typename HashFunction, typename MaskGenerationFunction>
    static ErrorOr<ByteBuffer> decode(ReadonlyBytes encoded_message, ReadonlyBytes parameters)
    {
        // FIXME: 1. If the length of P is greater than the input limitation for the
        //           hash function (2^61-1 octets for SHA-1) then output "parameter string
        //           too long" and stop.

        // 2. If ||EM|| < 2hLen+1, then output "decoding error" and stop.
        auto h_len = HashFunction::digest_size();
        auto max_message_size = (2 * h_len) + 1;
        if (encoded_message.size() < max_message_size)
            return Error::from_string_view("decoding error"sv);

        // 3. Let maskedSeed be the first hLen octets of EM and let maskedDB be the remaining ||EM|| - hLen octets.
        auto masked_seed = encoded_message.slice(0, h_len);
        auto masked_db = encoded_message.slice(h_len, encoded_message.size() - h_len);

        // 4. Let seedMask = MGF(maskedDB, hLen).
        auto seed_mask = TRY(MaskGenerationFunction::template mgf1<HashFunction>(masked_db, h_len));

        // 5. Let seed = maskedSeed \xor seedMask.
        auto seed = TRY(ByteBuffer::xor_buffers(masked_seed, seed_mask));

        // 6. Let dbMask = MGF(seed, ||EM|| - hLen).
        auto db_mask = TRY(MaskGenerationFunction::template mgf1<HashFunction>(seed, encoded_message.size() - h_len));

        // 7. Let DB = maskedDB \xor dbMask.
        auto db = TRY(ByteBuffer::xor_buffers(masked_db, db_mask));

        // 8. Let pHash = Hash(P), an octet string of length hLen.
        HashFunction hash;
        hash.update(parameters);
        auto digest = hash.digest();
        auto p_hash = digest.bytes();

        // 9. Separate DB into an octet string pHash' consisting of the first hLen octets of DB,
        //    a (possibly empty) octet string PS consisting of consecutive zero octets following pHash',
        //    and a message M as: DB = pHash' || PS || 01 || M
        auto p_hash_prime = TRY(db.slice(0, h_len));

        size_t i = h_len;
        for (; i < db.size(); ++i) {
            if (db[i] == 0x01)
                break;
        }

        //    If there is no 01 octet to separate PS from M, output "decoding error" and stop.
        if (i == db.size())
            return Error::from_string_view("decoding error"sv);

        auto ps = TRY(db.slice(h_len, i - h_len));
        auto message = TRY(db.slice(i + 1, db.size() - i - 1));

        // 10. If pHash' does not equal pHash, output "decoding error" and stop.
        if (p_hash_prime.span() != p_hash)
            return Error::from_string_view("decoding error"sv);

        // 11. Output M.
        return message;
    }

    // https://www.rfc-editor.org/rfc/rfc3447#section-7.1.2
    template<typename HashFunction, typename MaskGenerationFunction>
    static ErrorOr<ByteBuffer> eme_decode(ReadonlyBytes encoded_message, ReadonlyBytes label, u32 rsa_modulus_n)
    {
        auto h_len = HashFunction::digest_size();
        auto k = rsa_modulus_n;

        // 1. If the label L is not provided, let L be the empty string.
        // Let lHash = Hash(L), an octet string of length hLen (see the note in Section 7.1.1).
        HashFunction hash;
        hash.update(label);
        auto digest = hash.digest();
        auto l_hash = digest.bytes();

        // 2. Separate the encoded message EM into
        //    a single octet Y,
        //    an octet string maskedSeed of length hLen,
        //    and an octet string maskedDB of length k - hLen - 1
        //    as EM = Y || maskedSeed || maskedDB.
        auto y = encoded_message[0];
        auto masked_seed = encoded_message.slice(1, h_len);
        auto masked_db = encoded_message.slice(h_len + 1, k - h_len - 1);

        // 3. Let seedMask = MGF(maskedDB, hLen).
        auto seed_mask = TRY(MaskGenerationFunction::template mgf1<HashFunction>(masked_db, h_len));

        // 4. Let seed = maskedSeed \xor seedMask.
        auto seed = TRY(ByteBuffer::xor_buffers(masked_seed, seed_mask));

        // 5. Let dbMask = MGF(seed, k - hLen - 1).
        auto db_mask = TRY(MaskGenerationFunction::template mgf1<HashFunction>(seed, k - h_len - 1));

        // 6. Let DB = maskedDB \xor dbMask.
        auto db = TRY(ByteBuffer::xor_buffers(masked_db, db_mask));

        // 7. Separate DB into
        // an octet string lHash' of length hLen,
        // a (possibly empty) padding string PS consisting of octets withhexadecimal value 0x00,
        // and a message M
        // as DB = lHash' || PS || 0x01 || M.
        auto l_hash_prime = TRY(db.slice(0, h_len));

        size_t i = h_len;
        for (; i < db.size(); ++i) {
            if (db[i] == 0x01)
                break;
        }

        auto message = TRY(db.slice(i + 1, db.size() - i - 1));

        // NOTE: We have to make sure to do all these steps before returning an error due to timing attacks
        bool is_valid = true;

        // If there is no octet with hexadecimal value 0x01 to separate PS from M,
        if (i == db.size())
            is_valid = false;

        // if lHash does not equal lHash',
        if (l_hash_prime.span() != l_hash)
            is_valid = false;

        // if Y is nonzero, output "decryption error" and stop.
        if (y != 0x00)
            is_valid = false;

        if (!is_valid)
            return Error::from_string_view("decryption error"sv);

        // 8. Output the message M.
        return message;
    }
};

}
