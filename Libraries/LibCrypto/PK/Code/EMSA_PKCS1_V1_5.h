/*
 * Copyright (c) 2022, Michiel Visser <opensource@webmichiel.nl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCrypto/Hash/HashManager.h>
#include <LibCrypto/Hash/MD5.h>
#include <LibCrypto/Hash/SHA1.h>
#include <LibCrypto/Hash/SHA2.h>
#include <LibCrypto/PK/Code/Code.h>

namespace Crypto::PK {

template<typename HashFunction>
class EMSA_PKCS1_V1_5 : public Code<HashFunction> {
public:
    template<typename... Args>
    EMSA_PKCS1_V1_5(Args... args)
        : Code<HashFunction>(args...)
    {
    }

    virtual void encode(ReadonlyBytes in, ByteBuffer& out, size_t em_bits) override
    {
        auto& hash_fn = this->hasher();
        hash_fn.update(in);
        auto message_digest = hash_fn.digest();
        auto message_digest_size = message_digest.bytes().size();

        auto digest_info = hash_function_digest_info();
        auto encoded_message_length = digest_info.size() + message_digest_size;

        auto em_bytes = (em_bits + 7) / 8;
        // RFC8017 section 9.2: 3. If emLen < tLen + 11, output "intended encoded message length too short" and stop.
        if (em_bytes < encoded_message_length + 11) {
            dbgln("EMSA-PKCS1-V1_5-ENCODE: intended encoded message length too short");
            return;
        }

        auto offset = 0;
        // Build the padding 0x0001ffff..ff00
        out[offset++] = 0x00;
        out[offset++] = 0x01;
        for (size_t i = 0; i < em_bytes - encoded_message_length - 3; i++)
            out[offset++] = 0xff;
        out[offset++] = 0x00;
        // Add the digest info and message digest
        out.overwrite(offset, digest_info.data(), digest_info.size());
        offset += digest_info.size();
        out.overwrite(offset, message_digest.immutable_data(), message_digest.data_length());
    }

    virtual VerificationConsistency verify(ReadonlyBytes msg, ReadonlyBytes emsg, size_t em_bits) override
    {
        auto em_bytes = (em_bits + 7) / 8;
        auto buffer_result = ByteBuffer::create_uninitialized(em_bytes);
        if (buffer_result.is_error()) {
            dbgln("EMSA-PKCS1-V1_5-VERIFY: out of memory");
            return VerificationConsistency::Inconsistent;
        }
        auto buffer = buffer_result.release_value();

        // Encode the supplied message into the buffer
        encode(msg, buffer, em_bits);

        // Check that the expected message matches the encoded original message
        if (emsg != buffer) {
            return VerificationConsistency::Inconsistent;
        }
        return VerificationConsistency::Consistent;
    }

private:
    inline ReadonlyBytes hash_function_digest_info();
};

template<>
inline ReadonlyBytes EMSA_PKCS1_V1_5<Crypto::Hash::MD5>::hash_function_digest_info()
{
    // RFC8017 section 9.2 notes 1
    return { "\x30\x20\x30\x0c\x06\x08\x2a\x86\x48\x86\xf7\x0d\x02\x05\x05\x00\x04\x10", 18 };
}

template<>
inline ReadonlyBytes EMSA_PKCS1_V1_5<Crypto::Hash::SHA1>::hash_function_digest_info()
{
    // RFC8017 section 9.2 notes 1
    return { "\x30\x21\x30\x09\x06\x05\x2b\x0e\x03\x02\x1a\x05\x00\x04\x14", 15 };
}

template<>
inline ReadonlyBytes EMSA_PKCS1_V1_5<Crypto::Hash::SHA256>::hash_function_digest_info()
{
    // RFC8017 section 9.2 notes 1
    return { "\x30\x31\x30\x0d\x06\x09\x60\x86\x48\x01\x65\x03\x04\x02\x01\x05\x00\x04\x20", 19 };
}

template<>
inline ReadonlyBytes EMSA_PKCS1_V1_5<Crypto::Hash::SHA384>::hash_function_digest_info()
{
    // RFC8017 section 9.2 notes 1
    return { "\x30\x41\x30\x0d\x06\x09\x60\x86\x48\x01\x65\x03\x04\x02\x02\x05\x00\x04\x30", 19 };
}

template<>
inline ReadonlyBytes EMSA_PKCS1_V1_5<Crypto::Hash::SHA512>::hash_function_digest_info()
{
    // RFC8017 section 9.2 notes 1
    return { "\x30\x51\x30\x0d\x06\x09\x60\x86\x48\x01\x65\x03\x04\x02\x03\x05\x00\x04\x40", 19 };
}

template<>
inline ReadonlyBytes EMSA_PKCS1_V1_5<Crypto::Hash::Manager>::hash_function_digest_info()
{
    // RFC8017 section 9.2 notes 1
    switch (hasher().kind()) {
    case Hash::HashKind::MD5:
        return { "\x30\x20\x30\x0c\x06\x08\x2a\x86\x48\x86\xf7\x0d\x02\x05\x05\x00\x04\x10", 18 };
    case Hash::HashKind::SHA1:
        return { "\x30\x21\x30\x09\x06\x05\x2b\x0e\x03\x02\x1a\x05\x00\x04\x14", 15 };
    case Hash::HashKind::SHA256:
        return { "\x30\x31\x30\x0d\x06\x09\x60\x86\x48\x01\x65\x03\x04\x02\x01\x05\x00\x04\x20", 19 };
    case Hash::HashKind::SHA384:
        return { "\x30\x41\x30\x0d\x06\x09\x60\x86\x48\x01\x65\x03\x04\x02\x02\x05\x00\x04\x30", 19 };
    case Hash::HashKind::SHA512:
        return { "\x30\x51\x30\x0d\x06\x09\x60\x86\x48\x01\x65\x03\x04\x02\x03\x05\x00\x04\x40", 19 };
    case Hash::HashKind::None:
    default:
        VERIFY_NOT_REACHED();
    }
}

}
