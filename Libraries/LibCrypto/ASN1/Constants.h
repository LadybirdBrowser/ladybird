/*
 * Copyright (c) 2020-2023, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCrypto/ASN1/DER.h>

namespace Crypto::ASN1 {

constexpr static Array<int, 7>
    rsa_encryption_oid { 1, 2, 840, 113549, 1, 1, 1 },
    rsa_md5_encryption_oid { 1, 2, 840, 113549, 1, 1, 4 },
    rsa_sha1_encryption_oid { 1, 2, 840, 113549, 1, 1, 5 },
    rsa_sha256_encryption_oid { 1, 2, 840, 113549, 1, 1, 11 },
    rsa_sha384_encryption_oid { 1, 2, 840, 113549, 1, 1, 12 },
    rsa_sha512_encryption_oid { 1, 2, 840, 113549, 1, 1, 13 },
    rsa_sha224_encryption_oid { 1, 2, 840, 113549, 1, 1, 14 },
    ecdsa_with_sha224_encryption_oid { 1, 2, 840, 10045, 4, 3, 1 },
    ecdsa_with_sha256_encryption_oid { 1, 2, 840, 10045, 4, 3, 2 },
    ecdsa_with_sha384_encryption_oid { 1, 2, 840, 10045, 4, 3, 3 },
    ecdsa_with_sha512_encryption_oid { 1, 2, 840, 10045, 4, 3, 4 },
    secp256r1_oid { 1, 2, 840, 10045, 3, 1, 7 };

constexpr static Array<int, 6>
    ec_public_key_encryption_oid { 1, 2, 840, 10045, 2, 1 };

constexpr static Array<int, 5>
    secp384r1_oid { 1, 3, 132, 0, 34 },
    secp521r1_oid { 1, 3, 132, 0, 35 };

constexpr static Array<int, 4>
    x25519_oid { 1, 3, 101, 110 },
    x448_oid { 1, 3, 101, 111 },
    ed25519_oid { 1, 3, 101, 112 },
    ed448_oid { 1, 3, 101, 113 },
    key_usage_oid { 2, 5, 29, 15 },
    subject_alternative_name_oid { 2, 5, 29, 17 },
    issuer_alternative_name_oid { 2, 5, 29, 18 },
    basic_constraints_oid { 2, 5, 29, 19 };

#define _ENUM(key, value) key,

#define __ENUM_OBJECT_CLASS                    \
    _ENUM(ApplicationProcess, "2.5.6.11"_sv)   \
    _ENUM(Country, "2.5.6.2"_sv)               \
    _ENUM(DcObject, "1.3.6.1.4.1.1466.344"_sv) \
    _ENUM(Device, "2.5.6.14"_sv)               \
    _ENUM(GroupOfNames, "2.5.6.9"_sv)          \
    _ENUM(GroupOfUniqueNames, "2.5.6.17"_sv)   \
    _ENUM(Locality, "2.5.6.3"_sv)              \
    _ENUM(Organization, "2.5.6.4"_sv)          \
    _ENUM(OrganizationalPerson, "2.5.6.7"_sv)  \
    _ENUM(OrganizationalRole, "2.5.6.8"_sv)    \
    _ENUM(OrganizationalUnit, "2.5.6.5"_sv)    \
    _ENUM(Person, "2.5.6.6"_sv)                \
    _ENUM(ResidentialPerson, "2.5.6.10"_sv)    \
    _ENUM(UidObject, "1.3.6.1.1.3.1"_sv)

// NOTE: Type = O
// NOTE: This list is not exhaustive. If more members are needed, find them at the link below.
// https://www.iana.org/assignments/ldap-parameters/ldap-parameters.xhtml#ldap-parameters-3
enum class ObjectClass {
    __ENUM_OBJECT_CLASS
};

#define __ENUM_ATTRIBUTE_TYPE                        \
    _ENUM(BusinessCategory, "2.5.4.15"_sv)           \
    _ENUM(C, "2.5.4.6"_sv)                           \
    _ENUM(Cn, "2.5.4.3"_sv)                          \
    _ENUM(Dc, "0.9.2342.19200300.100.1.25"_sv)       \
    _ENUM(Description, "2.5.4.13"_sv)                \
    _ENUM(DestinationIndicator, "2.5.4.27"_sv)       \
    _ENUM(DistinguishedName, "2.5.4.49"_sv)          \
    _ENUM(DnQualifier, "2.5.4.46"_sv)                \
    _ENUM(EnhancedSearchGuide, "2.5.4.47"_sv)        \
    _ENUM(Email, "1.2.840.113549.1.9.1"_sv)          \
    _ENUM(FacsimileTelephoneNumber, "2.5.4.23"_sv)   \
    _ENUM(GenerationQualifier, "2.5.4.44"_sv)        \
    _ENUM(GivenName, "2.5.4.42"_sv)                  \
    _ENUM(HouseIdentifier, "2.5.4.51"_sv)            \
    _ENUM(Initials, "2.5.4.43"_sv)                   \
    _ENUM(InternationalISDNNumber, "2.5.4.25"_sv)    \
    _ENUM(L, "2.5.4.7"_sv)                           \
    _ENUM(Member, "2.5.4.31"_sv)                     \
    _ENUM(Name, "2.5.4.41"_sv)                       \
    _ENUM(O, "2.5.4.10"_sv)                          \
    _ENUM(Ou, "2.5.4.11"_sv)                         \
    _ENUM(Owner, "2.5.4.32"_sv)                      \
    _ENUM(PhysicalDeliveryOfficeName, "2.5.4.19"_sv) \
    _ENUM(PostalAddress, "2.5.4.16"_sv)              \
    _ENUM(PostalCode, "2.5.4.17"_sv)                 \
    _ENUM(PostOfficeBox, "2.5.4.18"_sv)              \
    _ENUM(PreferredDeliveryMethod, "2.5.4.28"_sv)    \
    _ENUM(RegisteredAddress, "2.5.4.26"_sv)          \
    _ENUM(RoleOccupant, "2.5.4.33"_sv)               \
    _ENUM(SearchGuide, "2.5.4.14"_sv)                \
    _ENUM(SeeAlso, "2.5.4.34"_sv)                    \
    _ENUM(SerialNumber, "2.5.4.5"_sv)                \
    _ENUM(Sn, "2.5.4.4"_sv)                          \
    _ENUM(St, "2.5.4.8"_sv)                          \
    _ENUM(Street, "2.5.4.9"_sv)                      \
    _ENUM(Surname, "2.5.4.4"_sv)                     \
    _ENUM(TelephoneNumber, "2.5.4.20"_sv)            \
    _ENUM(TeletexTerminalIdentifier, "2.5.4.22"_sv)  \
    _ENUM(TelexNumber, "2.5.4.21"_sv)                \
    _ENUM(Title, "2.5.4.12"_sv)                      \
    _ENUM(Uid, "0.9.2342.19200300.100.1.1"_sv)       \
    _ENUM(UniqueMember, "2.5.4.50"_sv)               \
    _ENUM(UserPassword, "2.5.4.35"_sv)               \
    _ENUM(X121Address, "2.5.4.24"_sv)                \
    _ENUM(X500UniqueIdentifier, "2.5.4.45"_sv)

// NOTE: Type = A
// NOTE: This list is not exhaustive. If more members are needed, find them at the link below.
// https://www.iana.org/assignments/ldap-parameters/ldap-parameters.xhtml#ldap-parameters-3
enum class AttributeType {
    __ENUM_ATTRIBUTE_TYPE
};

#undef _ENUM

constexpr static StringView enum_value(ObjectClass object_class)
{
#define _ENUM(key, value)  \
    case ObjectClass::key: \
        return value;

    switch (object_class) {
        __ENUM_OBJECT_CLASS
    }

    return "Unknown"_sv;
#undef _ENUM
#undef __ENUM_OBJECT_CLASS
}

constexpr static StringView enum_value(AttributeType object_class)
{
#define _ENUM(key, value)    \
    case AttributeType::key: \
        return value;

    switch (object_class) {
        __ENUM_ATTRIBUTE_TYPE
    }

    return "Unknown"_sv;
#undef _ENUM
#undef __ENUM_ATTRIBUTE_TYPE
}

}
