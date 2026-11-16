/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Memory.h>
#include <AK/ScopeGuard.h>
#include <LibCrypto/ASN1/DER.h>
#include <LibCrypto/PK/ModuleLatticePrivateKey.h>

namespace Crypto::PK {

enum class Choice {
    Seed,
    ExpandedKey,
    Both,
};

static ErrorOr<ByteBuffer> encode(Choice choice, ReadonlyBytes seed, ReadonlyBytes expanded_key)
{
    ASN1::Encoder encoder;
    switch (choice) {
    case Choice::Seed:
        TRY(encoder.write<ReadonlyBytes>(seed, ASN1::Class::Context, static_cast<ASN1::Kind>(0)));
        break;
    case Choice::ExpandedKey:
        TRY(encoder.write<ReadonlyBytes>(expanded_key));
        break;
    case Choice::Both:
        TRY(encoder.write_constructed(ASN1::Class::Universal, ASN1::Kind::Sequence, [&]() -> ErrorOr<void> {
            TRY(encoder.write<ReadonlyBytes>(seed));
            TRY(encoder.write<ReadonlyBytes>(expanded_key));
            return {};
        }));
        break;
    }
    return encoder.finish();
}

// https://datatracker.ietf.org/doc/html/draft-ietf-lamps-kyber-certificates-11#autoid-7
// https://www.rfc-editor.org/rfc/rfc9881.html#section-6
ErrorOr<ModuleLatticePrivateKey> parse_module_lattice_private_key(ReadonlyBytes der, size_t seed_size, size_t expanded_key_size)
{
    // PrivateKey ::= CHOICE {
    //      seed [0] IMPLICIT OCTET STRING (SIZE (seed_size)),
    //      expandedKey OCTET STRING (SIZE (expanded_key_size)),
    //      both SEQUENCE {
    //           seed OCTET STRING (SIZE (seed_size)),
    //           expandedKey OCTET STRING (SIZE (expanded_key_size))
    //      }
    // }
    // NB: The sizes are fixed, so each choice has a single DER layout. The input is a choice when it is the DER encoding
    //     of the bytes at that choice's places.
    auto zero_seed = TRY(ByteBuffer::create_zeroed(seed_size));
    auto zero_expanded_key = TRY(ByteBuffer::create_zeroed(expanded_key_size));
    auto expanded_key_header_size = TRY(encode(Choice::ExpandedKey, {}, zero_expanded_key)).size() - expanded_key_size;

    for (auto choice : { Choice::Seed, Choice::ExpandedKey, Choice::Both }) {
        if (TRY(encode(choice, zero_seed, zero_expanded_key)).size() != der.size())
            continue;

        ModuleLatticePrivateKey key;
        switch (choice) {
        case Choice::Seed:
            key.seed = der.slice(der.size() - seed_size);
            break;
        case Choice::ExpandedKey:
            key.expanded_key = der.slice(der.size() - expanded_key_size);
            break;
        case Choice::Both:
            key.expanded_key = der.slice(der.size() - expanded_key_size);
            key.seed = der.slice(der.size() - expanded_key_size - expanded_key_header_size - seed_size, seed_size);
            break;
        }

        auto encoded = TRY(encode(choice, key.seed, key.expanded_key));
        ScopeGuard const clear_encoded = [&] { secure_zero(encoded.data(), encoded.size()); };
        if (encoded == der)
            return key;
    }
    return Error::from_string_literal("Invalid module-lattice private key");
}

}
