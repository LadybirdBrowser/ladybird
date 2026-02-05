/*
 * Copyright (c) 2024, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Base64.h>
#include <AK/IPv4Address.h>
#include <AK/IPv6Address.h>
#include <AK/RedBlackTree.h>
#include <AK/Time.h>
#include <LibDNS/Export.h>

namespace DNS {
namespace Messages {

struct DomainName;
struct ParseContext {
    CountingStream& stream;
    NonnullOwnPtr<RedBlackTree<u16, DomainName>> pointers;
};

enum class OpCode : u8;

struct Options {
    //                                  1  1  1  1  1  1
    //    0  1  2  3  4  5  6  7  8  9  0  1  2  3  4  5
    //    +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
    //    |                      ID                       |
    //    +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
    //    |QR| Opcode |AA|TC|RD|RA|   Z |AD|CD|   RCODE   |
    constexpr inline static u16 QRMask = 0b1000000000000000;
    constexpr inline static u16 OpCodeMask = 0b0111100000000000;
    constexpr inline static u16 AuthoritativeAnswerMask = 0b0000010000000000;
    constexpr inline static u16 TruncatedMask = 0b0000001000000000;
    constexpr inline static u16 RecursionDesiredMask = 0b0000000100000000;
    constexpr inline static u16 RecursionAvailableMask = 0b0000000010000000;
    constexpr inline static u16 AuthenticatedDataMask = 0b0000000000100000;
    constexpr inline static u16 CheckingDisabledMask = 0b0000000000010000;
    constexpr inline static u16 ResponseCodeMask = 0b0000000000001111;

    enum class ResponseCode : u16 {
        NoError = 0,
        FormatError = 1,
        ServerFailure = 2,
        NameError = 3,
        NotImplemented = 4,
        Refused = 5,
    };

    void set_is_question(bool value) { raw = (raw & ~QRMask) | (value ? QRMask : 0); }
    void set_is_authoritative_answer(bool value) { raw = (raw & ~AuthoritativeAnswerMask) | (value ? AuthoritativeAnswerMask : 0); }
    void set_is_truncated(bool value) { raw = (raw & ~TruncatedMask) | (value ? TruncatedMask : 0); }
    void set_recursion_desired(bool value) { raw = (raw & ~RecursionDesiredMask) | (value ? RecursionDesiredMask : 0); }
    void set_recursion_available(bool value) { raw = (raw & ~RecursionAvailableMask) | (value ? RecursionAvailableMask : 0); }
    void set_response_code(ResponseCode code) { raw = (raw & ~ResponseCodeMask) | static_cast<u16>(code); }
    void set_checking_disabled(bool value) { raw = (raw & ~CheckingDisabledMask) | (value ? CheckingDisabledMask : 0); }
    void set_authenticated_data(bool value) { raw = (raw & ~AuthenticatedDataMask) | (value ? AuthenticatedDataMask : 0); }
    void set_op_code(OpCode code) { raw = (raw & ~OpCodeMask) | (static_cast<u16>(code) << 11); }

    bool is_question() const { return (raw & QRMask) == 0; }
    bool is_authoritative_answer() const { return (raw & AuthoritativeAnswerMask) != 0; }
    bool is_truncated() const { return (raw & TruncatedMask) != 0; }
    bool recursion_desired() const { return (raw & RecursionDesiredMask) != 0; }
    bool recursion_available() const { return (raw & RecursionAvailableMask) != 0; }
    bool checking_disabled() const { return (raw & CheckingDisabledMask) != 0; }
    bool authenticated_data() const { return (raw & AuthenticatedDataMask) != 0; }
    ResponseCode response_code() const { return static_cast<ResponseCode>(raw & ResponseCodeMask); }
    OpCode op_code() const { return static_cast<OpCode>((raw & OpCodeMask) >> 11); }

    String to_string() const;

    NetworkOrdered<u16> raw { 0 };
};
StringView to_string(Options::ResponseCode);

struct Header {
    NetworkOrdered<u16> id;
    Options options;
    NetworkOrdered<u16> question_count;
    NetworkOrdered<u16> answer_count;
    NetworkOrdered<u16> authority_count;
    NetworkOrdered<u16> additional_count;
};

struct DNS_API DomainName {
    Vector<ByteString> labels;

    static DomainName from_string(StringView);
    static ErrorOr<DomainName> from_raw(ParseContext&);
    ErrorOr<void> to_raw(ByteBuffer&) const;
    String to_string() const;
    String to_canonical_string() const;
    DomainName parent() const
    {
        auto copy = *this;
        copy.labels.take_first();
        return copy;
    }

    bool operator==(DomainName const&) const& = default;
    bool operator!=(DomainName const&) const& = default;
};

// Listing from IANA https://www.iana.org/assignments/dns-parameters/dns-parameters.xhtml#dns-parameters-4.
enum class ResourceType : u16 {
    Reserved = 0,    // [RFC6895]
    A = 1,           // a host address [RFC1035]
    NS = 2,          // an authoritative name server [RFC1035]
    MD = 3,          // a mail destination (OBSOLETE - use MX) [RFC1035]
    MF = 4,          // a mail forwarder (OBSOLETE - use MX) [RFC1035]
    CNAME = 5,       // the canonical name for an alias [RFC1035]
    SOA = 6,         // marks the start of a zone of authority [RFC1035]
    MB = 7,          // a mailbox domain name (EXPERIMENTAL) [RFC1035]
    MG = 8,          // a mail group member (EXPERIMENTAL) [RFC1035]
    MR = 9,          // a mail rename domain name (EXPERIMENTAL) [RFC1035]
    NULL_ = 10,      // a null RR (EXPERIMENTAL) [RFC1035]
    WKS = 11,        // a well known service description [RFC1035]
    PTR = 12,        // a domain name pointer [RFC1035]
    HINFO = 13,      // host information [RFC1035]
    MINFO = 14,      // mailbox or mail list information [RFC1035]
    MX = 15,         // mail exchange [RFC1035]
    TXT = 16,        // text strings [RFC1035]
    RP = 17,         // for Responsible Person [RFC1183]
    AFSDB = 18,      // for AFS Data Base location [RFC1183][RFC5864]
    X25 = 19,        // for X.25 PSDN address [RFC1183]
    ISDN = 20,       // for ISDN address [RFC1183]
    RT = 21,         // for Route Through [RFC1183]
    NSAP = 22,       // for NSAP address, NSAP style A record (DEPRECATED) [RFC1706][Moving TPC.INT and NSAP.INT infrastructure domains to historic]
    NSAP_PTR = 23,   // for domain name pointer, NSAP style (DEPRECATED) [RFC1706][Moving TPC.INT and NSAP.INT infrastructure domains to historic]
    SIG = 24,        // for security signature [RFC2536][RFC2931][RFC3110][RFC4034]
    KEY = 25,        // for security key [RFC2536][RFC2539][RFC3110][RFC4034]
    PX = 26,         // X.400 mail mapping information [RFC2163]
    GPOS = 27,       // Geographical Position [RFC1712]
    AAAA = 28,       // IP6 Address [RFC3596]
    LOC = 29,        // Location Information [RFC1876]
    NXT = 30,        // Next Domain (OBSOLETE) [RFC2535][RFC3755]
    EID = 31,        // Endpoint Identifier [Michael_Patton][http://ana-3.lcs.mit.edu/~jnc/nimrod/dns.txt]
    NIMLOC = 32,     // Nimrod Locator [1][Michael_Patton][http://ana-3.lcs.mit.edu/~jnc/nimrod/dns.txt]
    SRV = 33,        // Server Selection [1][RFC2782]
    ATMA = 34,       // ATM Address "[ ATM Forum Technical Committee, "ATM Name System, V2.0", Doc ID: AF-DANS-0152.000, July 2000. Available from and held in escrow by IANA.]"
    NAPTR = 35,      // Naming Authority Pointer [RFC3403]
    KX = 36,         // Key Exchanger [RFC2230]
    CERT = 37,       // CERT [RFC4398]
    A6 = 38,         // A6 (OBSOLETE - use AAAA) [RFC2874][RFC3226][RFC6563]
    DNAME = 39,      // DNAME [RFC6672]
    SINK = 40,       // SINK [Donald_E_Eastlake][draft-eastlake-kitchen-sink]
    OPT = 41,        // OPT [RFC3225][RFC6891]
    APL = 42,        // APL [RFC3123]
    DS = 43,         // Delegation Signer [RFC4034]
    SSHFP = 44,      // SSH Key Fingerprint [RFC4255]
    IPSECKEY = 45,   // IPSECKEY [RFC4025]
    RRSIG = 46,      // RRSIG [RFC4034]
    NSEC = 47,       // NSEC [RFC4034][RFC9077]
    DNSKEY = 48,     // DNSKEY [RFC4034]
    DHCID = 49,      // DHCID [RFC4701]
    NSEC3 = 50,      // NSEC3 [RFC5155][RFC9077]
    NSEC3PARAM = 51, // NSEC3PARAM [RFC5155]
    TLSA = 52,       // TLSA [RFC6698]
    SMIMEA = 53,     // S/MIME cert association [RFC8162]
    HIP = 55,        // Host Identity Protocol [RFC8005]
    NINFO = 56,      // NINFO [Jim_Reid]
    RKEY = 57,       // RKEY [Jim_Reid]
    TALINK = 58,     // Trust Anchor LINK [Wouter_Wijngaards]
    CDS = 59,        // Child DS [RFC7344]
    CDNSKEY = 60,    // DNSKEY(s) the Child wants reflected in DS [RFC7344]
    OPENPGPKEY = 61, // OpenPGP Key [RFC7929]
    CSYNC = 62,      // Child-To-Parent Synchronization [RFC7477]
    ZONEMD = 63,     // Message Digest Over Zone Data [RFC8976]
    SVCB = 64,       // General-purpose service binding [RFC9460]
    HTTPS = 65,      // SVCB-compatible type for use with HTTP [RFC9460]
    SPF = 99,        // [RFC7208]
    UINFO = 100,     // [IANA-Reserved]
    UID = 101,       // [IANA-Reserved]
    GID = 102,       // [IANA-Reserved]
    UNSPEC = 103,    // [IANA-Reserved]
    NID = 104,       // [RFC6742]
    L32 = 105,       // [RFC6742]
    L64 = 106,       // [RFC6742]
    LP = 107,        // [RFC6742]
    EUI48 = 108,     // an EUI-48 address [RFC7043]
    EUI64 = 109,     // an EUI-64 address [RFC7043]
    NXNAME = 128,    // NXDOMAIN indicator for Compact Denial of Existence [draft-ietf-dnsop-compact-denial-of-existence-04]
    TKEY = 249,      // Transaction Key [RFC2930]
    TSIG = 250,      // Transaction Signature [RFC8945]
    IXFR = 251,      // incremental transfer [RFC1995]
    AXFR = 252,      // transfer of an entire zone [RFC1035][RFC5936]
    MAILB = 253,     // mailbox-related RRs (MB, MG or MR) [RFC1035]
    MAILA = 254,     // mail agent RRs (OBSOLETE - see MX) [RFC1035]
    ANY = 255,       // A request for some or all records the server has available [RFC1035][RFC6895][RFC8482]
    URI = 256,       // URI [RFC7553]
    CAA = 257,       // Certification Authority Restriction [RFC8659]
    AVC = 258,       // Application Visibility and Control [Wolfgang_Riedel]
    DOA = 259,       // Digital Object Architecture [draft-durand-doa-over-dns]
    AMTRELAY = 260,  // Automatic Multicast Tunneling Relay [RFC8777]
    RESINFO = 261,   // Resolver Information as Key/Value Pairs [RFC9606]
    WALLET = 262,    // Public wallet address [Paul_Hoffman]
    CLA = 263,       // BP Convergence Layer Adapter [draft-johnson-dns-ipn-cla-07]
    IPN = 264,       // BP Node Number [draft-johnson-dns-ipn-cla-07]
    TA = 32768,      // DNSSEC Trust Authorities "[Sam_Weiler][Deploying DNSSEC Without a Signed Root.  Technical Report 1999-19, Information Networking Institute, Carnegie Mellon University, April 2004.]"
    DLV = 32769,     // DNSSEC Lookaside Validation (OBSOLETE) [RFC8749][RFC4431]
};
DNS_API StringView to_string(ResourceType);
DNS_API Optional<ResourceType> resource_type_from_string(StringView);

// Listing from IANA https://www.iana.org/assignments/dns-parameters/dns-parameters.xhtml#dns-parameters-2.
enum class Class : u16 {
    IN = 1, // the Internet [RFC1035]
    CH = 3, // the CHAOS class [Moon1981]
    HS = 4, // Hesiod [Dyer1987]
};
StringView to_string(Class);

// Listing from IANA https://www.iana.org/assignments/dns-parameters/dns-parameters.xhtml#dns-parameters-3.
enum class OpCode : u8 {
    Query = 0,        // a standard query (QUERY)
    IQuery = 1,       // an inverse query (IQUERY)
    Status = 2,       // a server status request (STATUS)
    Notify = 4,       // NOTIFY
    Update = 5,       // dynamic update (RFC 2136)
    DSO = 6,          // DNS Stateful Operations (DSO) [RFC8490]
    Reserved = 7,     // [RFC6895]
    ReservedMask = 15 // [RFC6895]
};
StringView to_string(OpCode);

namespace TLSA {

// Listings from IANA https://www.iana.org/assignments/dane-parameters/dane-parameters.xhtml.
enum class CertUsage : u8 {
    CAConstraint = 0,
    ServiceCertificateConstraint = 1,
    TrustAnchorAssertion = 2,
    DomainIssuedCertificate = 3,
    Private = 255
};
enum class Selector : u8 {
    FullCertificate = 0,
    SubjectPublicKeyInfo = 1,
    Private = 255
};
enum class MatchingType : u8 {
    Full = 0,
    SHA256 = 1,
    SHA512 = 2,
    Private = 255
};

}

namespace DNSSEC {

// Listing from IANA https://www.iana.org/assignments/dns-sec-alg-numbers/dns-sec-alg-numbers.xhtml.
enum class Algorithm : u8 {
    RSAMD5 = 1,           // RSA/MD5 [RFC4034][RFC3110]
    DSA = 3,              // DSA/SHA-1 [RFC3755][RFC2536]
    RSASHA1 = 5,          // RSA/SHA-1 [RFC3110]
    RSASHA1NSEC3SHA1 = 7, // [RFC5155]
    RSASHA256 = 8,        // RSA/SHA-256 [RFC5702]
    RSASHA512 = 10,       // RSA/SHA-512 [RFC5702]
    ECDSAP256SHA256 = 13, // ECDSA Curve P-256 with SHA-256 [RFC6605]
    ECDSAP384SHA384 = 14, // ECDSA Curve P-384 with SHA-384 [RFC6605]
    ED25519 = 15,         // Ed25519 [RFC8080]
    Unknown = 255         // Reserved for Private Use
};

static inline StringView to_string(Algorithm algorithm)
{
    switch (algorithm) {
    case Algorithm::RSAMD5:
        return "RSAMD5"sv;
    case Algorithm::DSA:
        return "DSA"sv;
    case Algorithm::RSASHA1:
        return "RSASHA1"sv;
    case Algorithm::RSASHA1NSEC3SHA1:
        return "RSASHA1NSEC3SHA1"sv;
    case Algorithm::RSASHA256:
        return "RSASHA256"sv;
    case Algorithm::RSASHA512:
        return "RSASHA512"sv;
    case Algorithm::ECDSAP256SHA256:
        return "ECDSAP256SHA256"sv;
    case Algorithm::ECDSAP384SHA384:
        return "ECDSAP384SHA384"sv;
    case Algorithm::ED25519:
        return "ED25519"sv;
    case Algorithm::Unknown:
        return "Unknown"sv;
    default:
        return "Invalid"sv;
    }
    VERIFY_NOT_REACHED();
}

// Listing from IANA https://www.iana.org/assignments/dns-sec-alg-numbers/dns-sec-alg-numbers.xhtml.
enum class DigestType : u8 {
    SHA1 = 1,     // SHA-1 [RFC3658]
    SHA256 = 2,   // SHA-256 [RFC4509]
    GOST3411 = 3, // GOST R 34.11-94 [RFC5933]
    SHA384 = 4,   // SHA-384 [RFC6605]
    SHA512 = 5,   // SHA-512 [RFC6605]
    SHA224 = 6,   // SHA-224 [RFC6605]
    Unknown = 255 // Reserved for Private Use
};

static inline StringView to_string(DigestType digest_type)
{
    switch (digest_type) {
    case DigestType::SHA1:
        return "SHA1"sv;
    case DigestType::SHA256:
        return "SHA256"sv;
    case DigestType::GOST3411:
        return "GOST3411"sv;
    case DigestType::SHA384:
        return "SHA384"sv;
    case DigestType::SHA512:
        return "SHA512"sv;
    case DigestType::SHA224:
        return "SHA224"sv;
    case DigestType::Unknown:
        return "Unknown"sv;
    }
    VERIFY_NOT_REACHED();
}

// Listing from IANA https://www.iana.org/assignments/dns-sec-alg-numbers/dns-sec-alg-numbers.xhtml.
enum class NSEC3HashAlgorithm : u8 {
    SHA1 = 1,     // [RFC5155]
    SHA256 = 2,   // [RFC6605]
    GOST3411 = 3, // [RFC5933]
    SHA384 = 4,   // [RFC6605]
    SHA512 = 5,   // [RFC6605]
    SHA224 = 6,   // [RFC6605]
    Unknown = 255 // Reserved for Private Use
};

static inline StringView to_string(NSEC3HashAlgorithm hash_algorithm)
{
    switch (hash_algorithm) {
    case NSEC3HashAlgorithm::SHA1:
        return "SHA1"sv;
    case NSEC3HashAlgorithm::SHA256:
        return "SHA256"sv;
    case NSEC3HashAlgorithm::GOST3411:
        return "GOST3411"sv;
    case NSEC3HashAlgorithm::SHA384:
        return "SHA384"sv;
    case NSEC3HashAlgorithm::SHA512:
        return "SHA512"sv;
    case NSEC3HashAlgorithm::SHA224:
        return "SHA224"sv;
    case NSEC3HashAlgorithm::Unknown:
        return "Unknown"sv;
    }
    VERIFY_NOT_REACHED();
}

}

struct Question {
    DomainName name;
    ResourceType type;
    Class class_;

    static ErrorOr<Question> from_raw(ParseContext&);
    ErrorOr<void> to_raw(ByteBuffer&) const;
};

namespace Records {

struct A {
    IPv4Address address;

    static constexpr ResourceType type = ResourceType::A;
    static ErrorOr<A> from_raw(ParseContext&);
    ErrorOr<void> to_raw(ByteBuffer&) const;
    ErrorOr<String> to_string() const { return address.to_string(); }
};
struct AAAA {
    IPv6Address address;

    static constexpr ResourceType type = ResourceType::AAAA;
    static ErrorOr<AAAA> from_raw(ParseContext&);
    ErrorOr<void> to_raw(ByteBuffer&) const;
    ErrorOr<String> to_string() const { return address.to_string(); }
};
struct TXT {
    ByteString content;

    static constexpr ResourceType type = ResourceType::TXT;
    static ErrorOr<TXT> from_raw(ParseContext&);
    ErrorOr<void> to_raw(ByteBuffer&) const;
    ErrorOr<String> to_string() const { return String::formatted("Text: '{}'", StringView { content }); }
};
struct CNAME {
    DomainName names;

    static constexpr ResourceType type = ResourceType::CNAME;
    static ErrorOr<CNAME> from_raw(ParseContext&);
    ErrorOr<void> to_raw(ByteBuffer&) const;
    ErrorOr<String> to_string() const { return names.to_string(); }
};
struct NS {
    DomainName name;

    static constexpr ResourceType type = ResourceType::NS;
    static ErrorOr<NS> from_raw(ParseContext&);
    ErrorOr<void> to_raw(ByteBuffer&) const { return Error::from_string_literal("Not implemented: NS::to_raw"); }
    ErrorOr<String> to_string() const { return name.to_string(); }
};
struct SOA {
    DomainName mname;
    DomainName rname;
    u32 serial;
    u32 refresh;
    u32 retry;
    u32 expire;
    u32 minimum;

    static constexpr ResourceType type = ResourceType::SOA;
    static ErrorOr<SOA> from_raw(ParseContext&);
    ErrorOr<void> to_raw(ByteBuffer&) const;
    ErrorOr<String> to_string() const
    {
        return String::formatted("SOA MName: '{}', RName: '{}', Serial: {}, Refresh: {}, Retry: {}, Expire: {}, Minimum: {}", mname.to_string(), rname.to_string(), serial, refresh, retry, expire, minimum);
    }
};
struct MX {
    u16 preference;
    DomainName exchange;

    static constexpr ResourceType type = ResourceType::MX;
    static ErrorOr<MX> from_raw(ParseContext&);
    ErrorOr<void> to_raw(ByteBuffer&) const { return Error::from_string_literal("Not implemented: MX::to_raw"); }
    ErrorOr<String> to_string() const { return String::formatted("MX Preference: {}, Exchange: '{}'", preference, exchange.to_string()); }
};
struct PTR {
    DomainName name;

    static constexpr ResourceType type = ResourceType::PTR;
    static ErrorOr<PTR> from_raw(ParseContext&);
    ErrorOr<void> to_raw(ByteBuffer&) const { return Error::from_string_literal("Not implemented: PTR::to_raw"); }
    ErrorOr<String> to_string() const { return name.to_string(); }
};
struct SRV {
    u16 priority;
    u16 weight;
    u16 port;
    DomainName target;

    static constexpr ResourceType type = ResourceType::SRV;
    static ErrorOr<SRV> from_raw(ParseContext&);
    ErrorOr<void> to_raw(ByteBuffer&) const { return Error::from_string_literal("Not implemented: SRV::to_raw"); }
    ErrorOr<String> to_string() const { return String::formatted("SRV Priority: {}, Weight: {}, Port: {}, Target: '{}'", priority, weight, port, target.to_string()); }
};
struct DNSKEY {
    u16 flags;
    u8 protocol;
    DNSSEC::Algorithm algorithm;
    ByteBuffer public_key;
    // Extra: calculated key tag
    u16 calculated_key_tag;
    // Extra: public key components (pointing into public_key) ONLY for RSA.
    u16 public_key_rsa_exponent_length() const
    {
        if (public_key[0] != 0)
            return public_key[0];
        return static_cast<u16>(public_key[1]) | static_cast<u16>(public_key[2]) << 8;
    }
    ReadonlyBytes public_key_rsa_exponent() const LIFETIME_BOUND { return public_key.bytes().slice(1, public_key_rsa_exponent_length()); }
    ReadonlyBytes public_key_rsa_modulus() const LIFETIME_BOUND { return public_key.bytes().slice(1 + public_key_rsa_exponent_length()); }

    constexpr static inline u16 FlagSecureEntryPoint = 0b1000000000000000;
    constexpr static inline u16 FlagZoneKey = 0b0100000000000000;
    constexpr static inline u16 FlagRevoked = 0b0010000000000000;

    constexpr bool is_secure_entry_point() const { return flags & FlagSecureEntryPoint; }
    constexpr bool is_zone_key() const { return flags & FlagZoneKey; }
    constexpr bool is_revoked() const { return flags & FlagRevoked; }
    constexpr bool is_key_signing_key() const { return is_secure_entry_point() && is_zone_key() && !is_revoked(); }

    static constexpr ResourceType type = ResourceType::DNSKEY;
    static ErrorOr<DNSKEY> from_raw(ParseContext&);
    ErrorOr<void> to_raw(ByteBuffer&) const;
    ErrorOr<String> to_string() const
    {
        return String::formatted("DNSKEY Flags: {}{}{}{}({}), Protocol: {}, Algorithm: {}, Public Key: {}, Tag: {}",
            is_secure_entry_point() ? "sep "sv : ""sv,
            is_zone_key() ? "zone "sv : ""sv,
            is_revoked() ? "revoked "sv : ""sv,
            is_key_signing_key() ? "ksk "sv : ""sv,
            flags,
            protocol,
            DNSSEC::to_string(algorithm),
            TRY(encode_base64(public_key)),
            calculated_key_tag);
    }
};
struct CDNSKEY : public DNSKEY {
    template<typename... Ts>
    CDNSKEY(Ts&&... args)
        : DNSKEY(forward<Ts>(args)...)
    {
    }

    static constexpr ResourceType type = ResourceType::CDNSKEY;
    static ErrorOr<CDNSKEY> from_raw(ParseContext& raw) { return DNSKEY::from_raw(raw); }
};
struct DS {
    u16 key_tag;
    DNSSEC::Algorithm algorithm;
    DNSSEC::DigestType digest_type;
    ByteBuffer digest;

    static constexpr ResourceType type = ResourceType::DS;
    static ErrorOr<DS> from_raw(ParseContext&);
    ErrorOr<void> to_raw(ByteBuffer&) const;
    ErrorOr<String> to_string() const
    {
        return String::formatted("DS Key Tag: {}, Algorithm: {}, Digest Type: {}, Digest: {}",
            key_tag,
            DNSSEC::to_string(algorithm),
            DNSSEC::to_string(digest_type),
            TRY(encode_base64(digest)));
    }
};
struct CDS : public DS {
    template<typename... Ts>
    CDS(Ts&&... args)
        : DS(forward<Ts>(args)...)
    {
    }
    static constexpr ResourceType type = ResourceType::CDS;
    static ErrorOr<CDS> from_raw(ParseContext& raw) { return DS::from_raw(raw); }
};
struct DNS_API SIG {
    ResourceType type_covered;
    DNSSEC::Algorithm algorithm;
    u8 label_count;
    u32 original_ttl;
    UnixDateTime expiration;
    UnixDateTime inception;
    u16 key_tag;
    DomainName signers_name;
    ByteBuffer signature;

    static constexpr ResourceType type = ResourceType::SIG;
    static ErrorOr<SIG> from_raw(ParseContext&);
    ErrorOr<void> to_raw(ByteBuffer&) const;
    ErrorOr<void> to_raw_excluding_signature(ByteBuffer&) const;
    ErrorOr<String> to_string() const;
};
struct RRSIG : public SIG {
    template<typename... Ts>
    RRSIG(Ts&&... args)
        : SIG(forward<Ts>(args)...)
    {
    }

    static constexpr ResourceType type = ResourceType::RRSIG;
    static ErrorOr<RRSIG> from_raw(ParseContext& raw) { return SIG::from_raw(raw); }
    ErrorOr<void> to_raw_excluding_signature(ByteBuffer& buffer) const { return SIG::to_raw_excluding_signature(buffer); }
};
struct NSEC {
    DomainName next_domain_name;
    Vector<ResourceType> types;

    static constexpr ResourceType type = ResourceType::NSEC;
    static ErrorOr<NSEC> from_raw(ParseContext&);
    ErrorOr<void> to_raw(ByteBuffer&) const { return Error::from_string_literal("Not implemented: NSC::to_raw"); }
    ErrorOr<String> to_string() const { return "NSEC"_string; }
};
struct NSEC3 {
    DNSSEC::NSEC3HashAlgorithm hash_algorithm;
    u8 flags;
    u16 iterations;
    ByteBuffer salt;
    DomainName next_hashed_owner_name;
    Vector<ResourceType> types;

    static constexpr ResourceType type = ResourceType::NSEC3;
    static ErrorOr<NSEC3> from_raw(ParseContext&);
    ErrorOr<void> to_raw(ByteBuffer&) const { return Error::from_string_literal("Not implemented: NSEC3::to_raw"); }
    ErrorOr<String> to_string() const { return "NSEC3"_string; }
};
struct NSEC3PARAM {
    DNSSEC::NSEC3HashAlgorithm hash_algorithm;
    u8 flags;
    u16 iterations;
    ByteBuffer salt;

    constexpr static inline u8 FlagOptOut = 0b10000000;

    constexpr bool is_opt_out() const { return flags & FlagOptOut; }

    static constexpr ResourceType type = ResourceType::NSEC3PARAM;
    static ErrorOr<NSEC3PARAM> from_raw(ParseContext&);
    ErrorOr<void> to_raw(ByteBuffer&) const { return Error::from_string_literal("Not implemented: NSEC3PARAM::to_raw"); }
    ErrorOr<String> to_string() const { return "NSEC3PARAM"_string; }
};
struct TLSA {
    Messages::TLSA::CertUsage cert_usage;
    Messages::TLSA::Selector selector;
    Messages::TLSA::MatchingType matching_type;
    ByteBuffer certificate_association_data;

    static ErrorOr<TLSA> from_raw(ParseContext&);
    ErrorOr<void> to_raw(ByteBuffer&) const { return Error::from_string_literal("Not implemented: TLSA::to_raw"); }
    ErrorOr<String> to_string() const { return "TLSA"_string; }
};
struct HINFO {
    ByteString cpu;
    ByteString os;

    static constexpr ResourceType type = ResourceType::HINFO;
    static ErrorOr<HINFO> from_raw(ParseContext&);
    ErrorOr<void> to_raw(ByteBuffer&) const;
    ErrorOr<String> to_string() const { return String::formatted("HINFO CPU: '{}', OS: '{}'", StringView { cpu }, StringView { os }); }
};
struct OPT {
    struct Option {
        u16 code;
        ByteBuffer data;
    };

    //                                   1  1  1  1  1  1
    //     0  1  2  3  4  5  6  7  8  9  0  1  2  3  4  5
    //    +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
    //    |                UDP Payload Size               |
    //    +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
    //    |     Extended RCode    |    VER    |     ZZ    |
    //    +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
    //    |DO|                  Z                         |
    //    +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
    //    |  OPT-LEN  / OPT-DATA...

    NetworkOrdered<u16> udp_payload_size { 0 };
    NetworkOrdered<u32> extended_rcode_and_flags { 0 };
    Vector<Option> options;
    static constexpr u32 MaskExtendedRCode = 0b11111111000000000000000000000000;
    static constexpr u32 MaskVersion = 0b00000000111100000000000000000000;
    static constexpr u32 MaskDO = 0b00000000000000001000000000000000;

    static constexpr ResourceType type = ResourceType::OPT;

    constexpr u8 extended_rcode() const { return (extended_rcode_and_flags & MaskExtendedRCode) >> 24; }
    constexpr u8 version() const { return (extended_rcode_and_flags & MaskVersion) >> 20; }
    constexpr bool dnssec_ok() const { return extended_rcode_and_flags & MaskDO; }

    void set_extended_rcode(u8 value) { extended_rcode_and_flags = (extended_rcode_and_flags & ~MaskExtendedRCode) | (value << 24); }
    void set_version(u8 value) { extended_rcode_and_flags = (extended_rcode_and_flags & ~MaskVersion) | (value << 20); }
    void set_dnssec_ok(bool value) { extended_rcode_and_flags = (extended_rcode_and_flags & ~MaskDO) | (value ? MaskDO : 0); }

    static ErrorOr<OPT> from_raw(ParseContext&);
    ErrorOr<void> to_raw(ByteBuffer&) const;
    ErrorOr<String> to_string() const
    {
        StringBuilder builder;
        builder.appendff("OPT UDP Payload Size: {}, Extended RCode: {}, Version: {}, DNSSEC OK: {}", udp_payload_size, extended_rcode(), version(), dnssec_ok());
        for (auto& option : options)
            builder.appendff(", opt[{} = '{:hex-dump}']", option.code, option.data.bytes());

        return builder.to_string();
    }
};

}

using Record = Variant<
    Records::A,
    Records::AAAA,
    Records::TXT,
    Records::CNAME,
    Records::NS,
    Records::SOA,
    Records::MX,
    Records::PTR,
    Records::SRV,
    Records::DNSKEY,
    Records::CDNSKEY,
    Records::DS,
    Records::CDS,
    Records::RRSIG,
    Records::NSEC,
    Records::NSEC3,
    Records::NSEC3PARAM,
    Records::TLSA,
    Records::HINFO,
    Records::OPT,
    // TODO: Add more records.
    ByteBuffer>; // Fallback for unknown records.

struct DNS_API ResourceRecord {
    DomainName name;
    ResourceType type;
    Class class_;
    u32 ttl;
    Record record;
    Optional<ByteBuffer> raw;

    static ErrorOr<ResourceRecord> from_raw(ParseContext&);
    ErrorOr<void> to_raw(ByteBuffer&) const;
    ErrorOr<String> to_string() const;
};

struct ZoneAuthority {
    DomainName name;
    ByteString admin_mailbox;
    u32 serial;
    u32 refresh;
    u32 retry;
    u32 expire;
    u32 minimum_ttl;
};

struct DNS_API Message {
    Header header;
    Vector<Question> questions;
    Vector<ResourceRecord> answers;
    Vector<ResourceRecord> authorities;
    Vector<ResourceRecord> additional_records;

    static ErrorOr<Message> from_raw(ParseContext&);
    static ErrorOr<Message> from_raw(Stream&);
    ErrorOr<size_t> to_raw(ByteBuffer&) const;

    ErrorOr<String> format_for_log() const;
};

}

}
