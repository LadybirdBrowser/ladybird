/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibCrypto/Cipher/AES.h>
#include <LibCrypto/Cipher/AESTables.h>

namespace Crypto::Cipher {

template<typename T>
constexpr u32 get_key(T pt)
{
    return ((u32)(pt)[0] << 24) ^ ((u32)(pt)[1] << 16) ^ ((u32)(pt)[2] << 8) ^ ((u32)(pt)[3]);
}

constexpr void swap_keys(u32* keys, size_t i, size_t j)
{
    u32 temp = keys[i];
    keys[i] = keys[j];
    keys[j] = temp;
}

ByteString AESCipherBlock::to_byte_string() const
{
    StringBuilder builder;
    for (auto value : m_data)
        builder.appendff("{:02x}", value);
    return builder.to_byte_string();
}

ByteString AESCipherKey::to_byte_string() const
{
    StringBuilder builder;
    for (size_t i = 0; i < (rounds() + 1) * 4; ++i)
        builder.appendff("{:02x}", m_rd_keys[i]);
    return builder.to_byte_string();
}

void AESCipherKey::expand_encrypt_key(ReadonlyBytes user_key, size_t bits)
{
    u32* round_key;
    u32 temp;
    size_t i { 0 };

    VERIFY(!user_key.is_null());
    VERIFY(is_valid_key_size(bits));
    VERIFY(user_key.size() == bits / 8);

    round_key = round_keys();

    if (bits == 128) {
        m_rounds = 10;
    } else if (bits == 192) {
        m_rounds = 12;
    } else {
        m_rounds = 14;
    }

    round_key[0] = get_key(user_key.data());
    round_key[1] = get_key(user_key.data() + 4);
    round_key[2] = get_key(user_key.data() + 8);
    round_key[3] = get_key(user_key.data() + 12);
    if (bits == 128) {
        for (;;) {
            temp = round_key[3];
            // clang-format off
            round_key[4] = round_key[0] ^
                    (AESTables::Encode2[(temp >> 16) & 0xff] & 0xff000000) ^
                    (AESTables::Encode3[(temp >>  8) & 0xff] & 0x00ff0000) ^
                    (AESTables::Encode0[(temp      ) & 0xff] & 0x0000ff00) ^
                    (AESTables::Encode1[(temp >> 24)       ] & 0x000000ff) ^ AESTables::RCON[i];
            // clang-format on
            round_key[5] = round_key[1] ^ round_key[4];
            round_key[6] = round_key[2] ^ round_key[5];
            round_key[7] = round_key[3] ^ round_key[6];
            ++i;
            if (i == 10)
                break;
            round_key += 4;
        }
        return;
    }

    round_key[4] = get_key(user_key.data() + 16);
    round_key[5] = get_key(user_key.data() + 20);
    if (bits == 192) {
        for (;;) {
            temp = round_key[5];
            // clang-format off
            round_key[6] = round_key[0] ^
                    (AESTables::Encode2[(temp >> 16) & 0xff] & 0xff000000) ^
                    (AESTables::Encode3[(temp >>  8) & 0xff] & 0x00ff0000) ^
                    (AESTables::Encode0[(temp      ) & 0xff] & 0x0000ff00) ^
                    (AESTables::Encode1[(temp >> 24)       ] & 0x000000ff) ^ AESTables::RCON[i];
            // clang-format on
            round_key[7] = round_key[1] ^ round_key[6];
            round_key[8] = round_key[2] ^ round_key[7];
            round_key[9] = round_key[3] ^ round_key[8];

            ++i;
            if (i == 8)
                break;

            round_key[10] = round_key[4] ^ round_key[9];
            round_key[11] = round_key[5] ^ round_key[10];

            round_key += 6;
        }
        return;
    }

    round_key[6] = get_key(user_key.data() + 24);
    round_key[7] = get_key(user_key.data() + 28);
    if (true) { // bits == 256
        for (;;) {
            temp = round_key[7];
            // clang-format off
            round_key[8] = round_key[0] ^
                    (AESTables::Encode2[(temp >> 16) & 0xff] & 0xff000000) ^
                    (AESTables::Encode3[(temp >>  8) & 0xff] & 0x00ff0000) ^
                    (AESTables::Encode0[(temp      ) & 0xff] & 0x0000ff00) ^
                    (AESTables::Encode1[(temp >> 24)       ] & 0x000000ff) ^ AESTables::RCON[i];
            // clang-format on
            round_key[9] = round_key[1] ^ round_key[8];
            round_key[10] = round_key[2] ^ round_key[9];
            round_key[11] = round_key[3] ^ round_key[10];

            ++i;
            if (i == 7)
                break;

            temp = round_key[11];
            // clang-format off
            round_key[12] = round_key[4] ^
                    (AESTables::Encode2[(temp >> 24)       ] & 0xff000000) ^
                    (AESTables::Encode3[(temp >> 16) & 0xff] & 0x00ff0000) ^
                    (AESTables::Encode0[(temp >>  8) & 0xff] & 0x0000ff00) ^
                    (AESTables::Encode1[(temp      ) & 0xff] & 0x000000ff) ;
            // clang-format on
            round_key[13] = round_key[5] ^ round_key[12];
            round_key[14] = round_key[6] ^ round_key[13];
            round_key[15] = round_key[7] ^ round_key[14];

            round_key += 8;
        }
        return;
    }
}

void AESCipherKey::expand_decrypt_key(ReadonlyBytes user_key, size_t bits)
{
    u32* round_key;

    expand_encrypt_key(user_key, bits);

    round_key = round_keys();

    // reorder round keys
    for (size_t i = 0, j = 4 * rounds(); i < j; i += 4, j -= 4) {
        swap_keys(round_key, i, j);
        swap_keys(round_key, i + 1, j + 1);
        swap_keys(round_key, i + 2, j + 2);
        swap_keys(round_key, i + 3, j + 3);
    }

    // apply inverse mix-column to middle rounds
    for (size_t i = 1; i < rounds(); ++i) {
        round_key += 4;
        // clang-format off
        round_key[0] =
                AESTables::Decode0[AESTables::Encode1[(round_key[0] >> 24)       ] & 0xff] ^
                AESTables::Decode1[AESTables::Encode1[(round_key[0] >> 16) & 0xff] & 0xff] ^
                AESTables::Decode2[AESTables::Encode1[(round_key[0] >>  8) & 0xff] & 0xff] ^
                AESTables::Decode3[AESTables::Encode1[(round_key[0]      ) & 0xff] & 0xff] ;
        round_key[1] =
                AESTables::Decode0[AESTables::Encode1[(round_key[1] >> 24)       ] & 0xff] ^
                AESTables::Decode1[AESTables::Encode1[(round_key[1] >> 16) & 0xff] & 0xff] ^
                AESTables::Decode2[AESTables::Encode1[(round_key[1] >>  8) & 0xff] & 0xff] ^
                AESTables::Decode3[AESTables::Encode1[(round_key[1]      ) & 0xff] & 0xff] ;
        round_key[2] =
                AESTables::Decode0[AESTables::Encode1[(round_key[2] >> 24)       ] & 0xff] ^
                AESTables::Decode1[AESTables::Encode1[(round_key[2] >> 16) & 0xff] & 0xff] ^
                AESTables::Decode2[AESTables::Encode1[(round_key[2] >>  8) & 0xff] & 0xff] ^
                AESTables::Decode3[AESTables::Encode1[(round_key[2]      ) & 0xff] & 0xff] ;
        round_key[3] =
                AESTables::Decode0[AESTables::Encode1[(round_key[3] >> 24)       ] & 0xff] ^
                AESTables::Decode1[AESTables::Encode1[(round_key[3] >> 16) & 0xff] & 0xff] ^
                AESTables::Decode2[AESTables::Encode1[(round_key[3] >>  8) & 0xff] & 0xff] ^
                AESTables::Decode3[AESTables::Encode1[(round_key[3]      ) & 0xff] & 0xff] ;
        // clang-format on
    }
}

void AESCipher::encrypt_block(AESCipherBlock const& in, AESCipherBlock& out)
{
    u32 s0, s1, s2, s3, t0, t1, t2, t3;
    size_t r { 0 };

    auto const& dec_key = key();
    auto const* round_keys = dec_key.round_keys();

    s0 = get_key(in.bytes().offset_pointer(0)) ^ round_keys[0];
    s1 = get_key(in.bytes().offset_pointer(4)) ^ round_keys[1];
    s2 = get_key(in.bytes().offset_pointer(8)) ^ round_keys[2];
    s3 = get_key(in.bytes().offset_pointer(12)) ^ round_keys[3];

    r = dec_key.rounds() >> 1;

    // apply the first |r - 1| rounds
    for (;;) {
        // clang-format off
        t0 = AESTables::Encode0[(s0 >> 24)       ] ^
             AESTables::Encode1[(s1 >> 16) & 0xff] ^
             AESTables::Encode2[(s2 >>  8) & 0xff] ^
             AESTables::Encode3[(s3      ) & 0xff] ^ round_keys[4];
        t1 = AESTables::Encode0[(s1 >> 24)       ] ^
             AESTables::Encode1[(s2 >> 16) & 0xff] ^
             AESTables::Encode2[(s3 >>  8) & 0xff] ^
             AESTables::Encode3[(s0      ) & 0xff] ^ round_keys[5];
        t2 = AESTables::Encode0[(s2 >> 24)       ] ^
             AESTables::Encode1[(s3 >> 16) & 0xff] ^
             AESTables::Encode2[(s0 >>  8) & 0xff] ^
             AESTables::Encode3[(s1      ) & 0xff] ^ round_keys[6];
        t3 = AESTables::Encode0[(s3 >> 24)       ] ^
             AESTables::Encode1[(s0 >> 16) & 0xff] ^
             AESTables::Encode2[(s1 >>  8) & 0xff] ^
             AESTables::Encode3[(s2      ) & 0xff] ^ round_keys[7];
        // clang-format on

        round_keys += 8;
        --r;
        if (r == 0)
            break;

        // clang-format off
        s0 = AESTables::Encode0[(t0 >> 24)       ] ^
             AESTables::Encode1[(t1 >> 16) & 0xff] ^
             AESTables::Encode2[(t2 >>  8) & 0xff] ^
             AESTables::Encode3[(t3      ) & 0xff] ^ round_keys[0];
        s1 = AESTables::Encode0[(t1 >> 24)       ] ^
             AESTables::Encode1[(t2 >> 16) & 0xff] ^
             AESTables::Encode2[(t3 >>  8) & 0xff] ^
             AESTables::Encode3[(t0      ) & 0xff] ^ round_keys[1];
        s2 = AESTables::Encode0[(t2 >> 24)       ] ^
             AESTables::Encode1[(t3 >> 16) & 0xff] ^
             AESTables::Encode2[(t0 >>  8) & 0xff] ^
             AESTables::Encode3[(t1      ) & 0xff] ^ round_keys[2];
        s3 = AESTables::Encode0[(t3 >> 24)       ] ^
             AESTables::Encode1[(t0 >> 16) & 0xff] ^
             AESTables::Encode2[(t1 >>  8) & 0xff] ^
             AESTables::Encode3[(t2      ) & 0xff] ^ round_keys[3];
        // clang-format on
    }

    // apply the last round and put the encrypted data into out
    // clang-format off
    s0 = (AESTables::Encode2[(t0 >> 24)       ] & 0xff000000) ^
         (AESTables::Encode3[(t1 >> 16) & 0xff] & 0x00ff0000) ^
         (AESTables::Encode0[(t2 >>  8) & 0xff] & 0x0000ff00) ^
         (AESTables::Encode1[(t3      ) & 0xff] & 0x000000ff) ^ round_keys[0];
    out.put(0, s0);

    s1 = (AESTables::Encode2[(t1 >> 24)       ] & 0xff000000) ^
         (AESTables::Encode3[(t2 >> 16) & 0xff] & 0x00ff0000) ^
         (AESTables::Encode0[(t3 >>  8) & 0xff] & 0x0000ff00) ^
         (AESTables::Encode1[(t0      ) & 0xff] & 0x000000ff) ^ round_keys[1];
    out.put(4, s1);

    s2 = (AESTables::Encode2[(t2 >> 24)       ] & 0xff000000) ^
         (AESTables::Encode3[(t3 >> 16) & 0xff] & 0x00ff0000) ^
         (AESTables::Encode0[(t0 >>  8) & 0xff] & 0x0000ff00) ^
         (AESTables::Encode1[(t1      ) & 0xff] & 0x000000ff) ^ round_keys[2];
    out.put(8, s2);

    s3 = (AESTables::Encode2[(t3 >> 24)       ] & 0xff000000) ^
         (AESTables::Encode3[(t0 >> 16) & 0xff] & 0x00ff0000) ^
         (AESTables::Encode0[(t1 >>  8) & 0xff] & 0x0000ff00) ^
         (AESTables::Encode1[(t2      ) & 0xff] & 0x000000ff) ^ round_keys[3];
    out.put(12, s3);
    // clang-format on
}

void AESCipher::decrypt_block(AESCipherBlock const& in, AESCipherBlock& out)
{
    u32 s0, s1, s2, s3, t0, t1, t2, t3;
    size_t r { 0 };

    auto const& dec_key = key();
    auto const* round_keys = dec_key.round_keys();

    s0 = get_key(in.bytes().offset_pointer(0)) ^ round_keys[0];
    s1 = get_key(in.bytes().offset_pointer(4)) ^ round_keys[1];
    s2 = get_key(in.bytes().offset_pointer(8)) ^ round_keys[2];
    s3 = get_key(in.bytes().offset_pointer(12)) ^ round_keys[3];

    r = dec_key.rounds() >> 1;

    // apply the first |r - 1| rounds
    for (;;) {
        // clang-format off
        t0 = AESTables::Decode0[(s0 >> 24)       ] ^
             AESTables::Decode1[(s3 >> 16) & 0xff] ^
             AESTables::Decode2[(s2 >>  8) & 0xff] ^
             AESTables::Decode3[(s1      ) & 0xff] ^ round_keys[4];
        t1 = AESTables::Decode0[(s1 >> 24)       ] ^
             AESTables::Decode1[(s0 >> 16) & 0xff] ^
             AESTables::Decode2[(s3 >>  8) & 0xff] ^
             AESTables::Decode3[(s2      ) & 0xff] ^ round_keys[5];
        t2 = AESTables::Decode0[(s2 >> 24)       ] ^
             AESTables::Decode1[(s1 >> 16) & 0xff] ^
             AESTables::Decode2[(s0 >>  8) & 0xff] ^
             AESTables::Decode3[(s3      ) & 0xff] ^ round_keys[6];
        t3 = AESTables::Decode0[(s3 >> 24)       ] ^
             AESTables::Decode1[(s2 >> 16) & 0xff] ^
             AESTables::Decode2[(s1 >>  8) & 0xff] ^
             AESTables::Decode3[(s0      ) & 0xff] ^ round_keys[7];
        // clang-format on

        round_keys += 8;
        --r;
        if (r == 0)
            break;

        // clang-format off
        s0 = AESTables::Decode0[(t0 >> 24)       ] ^
             AESTables::Decode1[(t3 >> 16) & 0xff] ^
             AESTables::Decode2[(t2 >>  8) & 0xff] ^
             AESTables::Decode3[(t1      ) & 0xff] ^ round_keys[0];
        s1 = AESTables::Decode0[(t1 >> 24)       ] ^
             AESTables::Decode1[(t0 >> 16) & 0xff] ^
             AESTables::Decode2[(t3 >>  8) & 0xff] ^
             AESTables::Decode3[(t2      ) & 0xff] ^ round_keys[1];
        s2 = AESTables::Decode0[(t2 >> 24)       ] ^
             AESTables::Decode1[(t1 >> 16) & 0xff] ^
             AESTables::Decode2[(t0 >>  8) & 0xff] ^
             AESTables::Decode3[(t3      ) & 0xff] ^ round_keys[2];
        s3 = AESTables::Decode0[(t3 >> 24)       ] ^
             AESTables::Decode1[(t2 >> 16) & 0xff] ^
             AESTables::Decode2[(t1 >>  8) & 0xff] ^
             AESTables::Decode3[(t0      ) & 0xff] ^ round_keys[3];
        // clang-format on
    }

    // apply the last round and put the decrypted data into out
    // clang-format off
    s0 = ((u32)AESTables::Decode4[(t0 >> 24)       ] << 24) ^
         ((u32)AESTables::Decode4[(t3 >> 16) & 0xff] << 16) ^
         ((u32)AESTables::Decode4[(t2 >>  8) & 0xff] <<  8) ^
         ((u32)AESTables::Decode4[(t1      ) & 0xff]      ) ^ round_keys[0];
    out.put(0, s0);

    s1 = ((u32)AESTables::Decode4[(t1 >> 24)       ] << 24) ^
         ((u32)AESTables::Decode4[(t0 >> 16) & 0xff] << 16) ^
         ((u32)AESTables::Decode4[(t3 >>  8) & 0xff] <<  8) ^
         ((u32)AESTables::Decode4[(t2      ) & 0xff]      ) ^ round_keys[1];
    out.put(4, s1);

    s2 = ((u32)AESTables::Decode4[(t2 >> 24)       ] << 24) ^
         ((u32)AESTables::Decode4[(t1 >> 16) & 0xff] << 16) ^
         ((u32)AESTables::Decode4[(t0 >>  8) & 0xff] <<  8) ^
         ((u32)AESTables::Decode4[(t3      ) & 0xff]      ) ^ round_keys[2];
    out.put(8, s2);

    s3 = ((u32)AESTables::Decode4[(t3 >> 24)       ] << 24) ^
         ((u32)AESTables::Decode4[(t2 >> 16) & 0xff] << 16) ^
         ((u32)AESTables::Decode4[(t1 >>  8) & 0xff] <<  8) ^
         ((u32)AESTables::Decode4[(t0      ) & 0xff]      ) ^ round_keys[3];
    out.put(12, s3);
    // clang-format on
}

void AESCipherBlock::overwrite(ReadonlyBytes bytes)
{
    auto data = bytes.data();
    auto length = bytes.size();

    VERIFY(length <= this->data_size());
    this->bytes().overwrite(0, data, length);
    if (length < this->data_size()) {
        switch (padding_mode()) {
        case PaddingMode::Null:
            // fill with zeros
            __builtin_memset(m_data + length, 0, this->data_size() - length);
            break;
        case PaddingMode::CMS:
            // fill with the length of the padding bytes
            __builtin_memset(m_data + length, this->data_size() - length, this->data_size() - length);
            break;
        case PaddingMode::RFC5246:
            // fill with the length of the padding bytes minus one
            __builtin_memset(m_data + length, this->data_size() - length - 1, this->data_size() - length);
            break;
        default:
            // FIXME: We should handle the rest of the common padding modes
            VERIFY_NOT_REACHED();
            break;
        }
    }
}

}
