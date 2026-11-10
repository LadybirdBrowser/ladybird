/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/ASN1/DER.h>
#include <LibCrypto/PK/ModuleLatticePrivateKey.h>

namespace Crypto::PK {

static ErrorOr<void> expect_tag(ASN1::Decoder& decoder, ASN1::Class class_, ASN1::Kind kind, ASN1::Type type)
{
    auto tag = TRY(decoder.peek());
    if (tag.class_ != class_ || tag.kind != kind || tag.type != type)
        return Error::from_string_literal("Unexpected tag in private key");
    return {};
}

static ErrorOr<ReadonlyBytes> read_octet_string(ASN1::Decoder& decoder, ASN1::Class class_, ASN1::Kind kind, size_t size)
{
    TRY(expect_tag(decoder, class_, kind, ASN1::Type::Primitive));
    auto contents = TRY(decoder.read<StringView>(ASN1::Class::Universal, ASN1::Kind::OctetString));
    if (contents.length() != size)
        return Error::from_string_literal("Unexpected size of an OCTET STRING in private key");
    return contents.bytes();
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
    ASN1::Decoder decoder { der };
    ModuleLatticePrivateKey key;

    auto tag = TRY(decoder.peek());
    if (tag.class_ == ASN1::Class::Context) {
        key.seed = TRY(read_octet_string(decoder, ASN1::Class::Context, static_cast<ASN1::Kind>(0), seed_size));
    } else if (tag.kind == ASN1::Kind::OctetString) {
        key.expanded_key = TRY(read_octet_string(decoder, ASN1::Class::Universal, ASN1::Kind::OctetString, expanded_key_size));
    } else {
        TRY(expect_tag(decoder, ASN1::Class::Universal, ASN1::Kind::Sequence, ASN1::Type::Constructed));
        TRY(decoder.enter());
        key.seed = TRY(read_octet_string(decoder, ASN1::Class::Universal, ASN1::Kind::OctetString, seed_size));
        key.expanded_key = TRY(read_octet_string(decoder, ASN1::Class::Universal, ASN1::Kind::OctetString, expanded_key_size));
        if (!decoder.eof())
            return Error::from_string_literal("Trailing data in private key");
        TRY(decoder.leave());
    }

    if (!decoder.eof())
        return Error::from_string_literal("Trailing data after private key");
    return key;
}

}
