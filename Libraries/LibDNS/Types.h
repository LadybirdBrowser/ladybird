/*
 * Copyright (c) 2024, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <LibDNS/Export.h>

namespace DNS::Messages {

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
DNS_API String to_string(ResourceType);
DNS_API Optional<ResourceType> resource_type_from_string(StringView);

// Listing from IANA https://www.iana.org/assignments/dns-parameters/dns-parameters.xhtml#dns-parameters-2.
enum class Class : u16 {
    IN = 1, // the Internet [RFC1035]
    CH = 3, // the CHAOS class [Moon1981]
    HS = 4, // Hesiod [Dyer1987]
};

}
