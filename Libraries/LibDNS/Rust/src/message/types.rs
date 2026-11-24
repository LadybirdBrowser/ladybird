/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::fmt;

/// Defines a protocol number as a newtype over its wire integer, so that values without a name round-trip.
macro_rules! wire_enum {
    (
        $(#[$meta:meta])*
        $name:ident($repr:ty), fallback = $fallback:literal,
        { $($(#[$const_meta:meta])* $constant:ident = $value:literal => $text:literal,)* }
    ) => {
        $(#[$meta])*
        #[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
        #[repr(transparent)]
        pub struct $name(pub $repr);

        impl $name {
            $($(#[$const_meta])* pub const $constant: Self = Self($value);)*

            pub const fn name(self) -> &'static str {
                match self.0 {
                    $($value => $text,)*
                    _ => $fallback,
                }
            }

            pub fn from_name(name: &str) -> Option<Self> {
                match name {
                    $($text => Some(Self::$constant),)*
                    _ => None,
                }
            }
        }

        impl fmt::Display for $name {
            fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                f.write_str(self.name())
            }
        }

        impl From<$repr> for $name {
            fn from(value: $repr) -> Self {
                Self(value)
            }
        }

        impl From<$name> for $repr {
            fn from(value: $name) -> Self {
                value.0
            }
        }
    };
}

wire_enum! {
    /// Listing from IANA https://www.iana.org/assignments/dns-parameters/dns-parameters.xhtml#dns-parameters-4.
    ResourceType(u16), fallback = "UNKNOWN", {
        /// [RFC6895]
        RESERVED = 0 => "Reserved",
        /// a host address [RFC1035]
        A = 1 => "A",
        /// an authoritative name server [RFC1035]
        NS = 2 => "NS",
        /// a mail destination (OBSOLETE - use MX) [RFC1035]
        MD = 3 => "MD",
        /// a mail forwarder (OBSOLETE - use MX) [RFC1035]
        MF = 4 => "MF",
        /// the canonical name for an alias [RFC1035]
        CNAME = 5 => "CNAME",
        /// marks the start of a zone of authority [RFC1035]
        SOA = 6 => "SOA",
        /// a mailbox domain name (EXPERIMENTAL) [RFC1035]
        MB = 7 => "MB",
        /// a mail group member (EXPERIMENTAL) [RFC1035]
        MG = 8 => "MG",
        /// a mail rename domain name (EXPERIMENTAL) [RFC1035]
        MR = 9 => "MR",
        /// a null RR (EXPERIMENTAL) [RFC1035]
        NULL = 10 => "NULL_",
        /// a well known service description [RFC1035]
        WKS = 11 => "WKS",
        /// a domain name pointer [RFC1035]
        PTR = 12 => "PTR",
        /// host information [RFC1035]
        HINFO = 13 => "HINFO",
        /// mailbox or mail list information [RFC1035]
        MINFO = 14 => "MINFO",
        /// mail exchange [RFC1035]
        MX = 15 => "MX",
        /// text strings [RFC1035]
        TXT = 16 => "TXT",
        /// for Responsible Person [RFC1183]
        RP = 17 => "RP",
        /// for AFS Data Base location [RFC1183][RFC5864]
        AFSDB = 18 => "AFSDB",
        /// for X.25 PSDN address [RFC1183]
        X25 = 19 => "X25",
        /// for ISDN address [RFC1183]
        ISDN = 20 => "ISDN",
        /// for Route Through [RFC1183]
        RT = 21 => "RT",
        /// for NSAP address, NSAP style A record (DEPRECATED) [RFC1706]
        NSAP = 22 => "NSAP",
        /// for domain name pointer, NSAP style (DEPRECATED) [RFC1706]
        NSAP_PTR = 23 => "NSAP_PTR",
        /// for security signature [RFC2536][RFC2931][RFC3110][RFC4034]
        SIG = 24 => "SIG",
        /// for security key [RFC2536][RFC2539][RFC3110][RFC4034]
        KEY = 25 => "KEY",
        /// X.400 mail mapping information [RFC2163]
        PX = 26 => "PX",
        /// Geographical Position [RFC1712]
        GPOS = 27 => "GPOS",
        /// IP6 Address [RFC3596]
        AAAA = 28 => "AAAA",
        /// Location Information [RFC1876]
        LOC = 29 => "LOC",
        /// Next Domain (OBSOLETE) [RFC2535][RFC3755]
        NXT = 30 => "NXT",
        /// Endpoint Identifier
        EID = 31 => "EID",
        /// Nimrod Locator
        NIMLOC = 32 => "NIMLOC",
        /// Server Selection [RFC2782]
        SRV = 33 => "SRV",
        /// ATM Address
        ATMA = 34 => "ATMA",
        /// Naming Authority Pointer [RFC3403]
        NAPTR = 35 => "NAPTR",
        /// Key Exchanger [RFC2230]
        KX = 36 => "KX",
        /// CERT [RFC4398]
        CERT = 37 => "CERT",
        /// A6 (OBSOLETE - use AAAA) [RFC2874][RFC3226][RFC6563]
        A6 = 38 => "A6",
        /// DNAME [RFC6672]
        DNAME = 39 => "DNAME",
        /// SINK [draft-eastlake-kitchen-sink]
        SINK = 40 => "SINK",
        /// OPT [RFC3225][RFC6891]
        OPT = 41 => "OPT",
        /// APL [RFC3123]
        APL = 42 => "APL",
        /// Delegation Signer [RFC4034]
        DS = 43 => "DS",
        /// SSH Key Fingerprint [RFC4255]
        SSHFP = 44 => "SSHFP",
        /// IPSECKEY [RFC4025]
        IPSECKEY = 45 => "IPSECKEY",
        /// RRSIG [RFC4034]
        RRSIG = 46 => "RRSIG",
        /// NSEC [RFC4034][RFC9077]
        NSEC = 47 => "NSEC",
        /// DNSKEY [RFC4034]
        DNSKEY = 48 => "DNSKEY",
        /// DHCID [RFC4701]
        DHCID = 49 => "DHCID",
        /// NSEC3 [RFC5155][RFC9077]
        NSEC3 = 50 => "NSEC3",
        /// NSEC3PARAM [RFC5155]
        NSEC3PARAM = 51 => "NSEC3PARAM",
        /// TLSA [RFC6698]
        TLSA = 52 => "TLSA",
        /// S/MIME cert association [RFC8162]
        SMIMEA = 53 => "SMIMEA",
        /// Host Identity Protocol [RFC8005]
        HIP = 55 => "HIP",
        /// NINFO
        NINFO = 56 => "NINFO",
        /// RKEY
        RKEY = 57 => "RKEY",
        /// Trust Anchor LINK
        TALINK = 58 => "TALINK",
        /// Child DS [RFC7344]
        CDS = 59 => "CDS",
        /// DNSKEY(s) the Child wants reflected in DS [RFC7344]
        CDNSKEY = 60 => "CDNSKEY",
        /// OpenPGP Key [RFC7929]
        OPENPGPKEY = 61 => "OPENPGPKEY",
        /// Child-To-Parent Synchronization [RFC7477]
        CSYNC = 62 => "CSYNC",
        /// Message Digest Over Zone Data [RFC8976]
        ZONEMD = 63 => "ZONEMD",
        /// General-purpose service binding [RFC9460]
        SVCB = 64 => "SVCB",
        /// SVCB-compatible type for use with HTTP [RFC9460]
        HTTPS = 65 => "HTTPS",
        /// [RFC7208]
        SPF = 99 => "SPF",
        /// [IANA-Reserved]
        UINFO = 100 => "UINFO",
        /// [IANA-Reserved]
        UID = 101 => "UID",
        /// [IANA-Reserved]
        GID = 102 => "GID",
        /// [IANA-Reserved]
        UNSPEC = 103 => "UNSPEC",
        /// [RFC6742]
        NID = 104 => "NID",
        /// [RFC6742]
        L32 = 105 => "L32",
        /// [RFC6742]
        L64 = 106 => "L64",
        /// [RFC6742]
        LP = 107 => "LP",
        /// an EUI-48 address [RFC7043]
        EUI48 = 108 => "EUI48",
        /// an EUI-64 address [RFC7043]
        EUI64 = 109 => "EUI64",
        /// NXDOMAIN indicator for Compact Denial of Existence [draft-ietf-dnsop-compact-denial-of-existence-04]
        NXNAME = 128 => "NXNAME",
        /// Transaction Key [RFC2930]
        TKEY = 249 => "TKEY",
        /// Transaction Signature [RFC8945]
        TSIG = 250 => "TSIG",
        /// incremental transfer [RFC1995]
        IXFR = 251 => "IXFR",
        /// transfer of an entire zone [RFC1035][RFC5936]
        AXFR = 252 => "AXFR",
        /// mailbox-related RRs (MB, MG or MR) [RFC1035]
        MAILB = 253 => "MAILB",
        /// mail agent RRs (OBSOLETE - see MX) [RFC1035]
        MAILA = 254 => "MAILA",
        /// A request for some or all records the server has available [RFC1035][RFC6895][RFC8482]
        ANY = 255 => "ANY",
        /// URI [RFC7553]
        URI = 256 => "URI",
        /// Certification Authority Restriction [RFC8659]
        CAA = 257 => "CAA",
        /// Application Visibility and Control
        AVC = 258 => "AVC",
        /// Digital Object Architecture [draft-durand-doa-over-dns]
        DOA = 259 => "DOA",
        /// Automatic Multicast Tunneling Relay [RFC8777]
        AMTRELAY = 260 => "AMTRELAY",
        /// Resolver Information as Key/Value Pairs [RFC9606]
        RESINFO = 261 => "RESINFO",
        /// Public wallet address
        WALLET = 262 => "WALLET",
        /// BP Convergence Layer Adapter [draft-johnson-dns-ipn-cla-07]
        CLA = 263 => "CLA",
        /// BP Node Number [draft-johnson-dns-ipn-cla-07]
        IPN = 264 => "IPN",
        /// DNSSEC Trust Authorities
        TA = 32768 => "TA",
        /// DNSSEC Lookaside Validation (OBSOLETE) [RFC8749][RFC4431]
        DLV = 32769 => "DLV",
    }
}

wire_enum! {
    /// Listing from IANA https://www.iana.org/assignments/dns-parameters/dns-parameters.xhtml#dns-parameters-2.
    Class(u16), fallback = "UNKNOWN", {
        /// the Internet [RFC1035]
        IN = 1 => "IN",
        /// the CHAOS class [Moon1981]
        CH = 3 => "CH",
        /// Hesiod [Dyer1987]
        HS = 4 => "HS",
    }
}

wire_enum! {
    /// The header RCODE.
    ResponseCode(u16), fallback = "UNKNOWN", {
        NO_ERROR = 0 => "NoError",
        FORMAT_ERROR = 1 => "FormatError",
        SERVER_FAILURE = 2 => "ServerFailure",
        NAME_ERROR = 3 => "NameError",
        NOT_IMPLEMENTED = 4 => "NotImplemented",
        REFUSED = 5 => "Refused",
    }
}

/// Listing from IANA https://www.iana.org/assignments/dns-parameters/dns-parameters.xhtml#dns-parameters-3.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
#[repr(transparent)]
pub struct OpCode(pub u8);

impl OpCode {
    /// a standard query (QUERY)
    pub const QUERY: Self = Self(0);
    /// an inverse query (IQUERY)
    pub const IQUERY: Self = Self(1);
    /// a server status request (STATUS)
    pub const STATUS: Self = Self(2);
    /// NOTIFY
    pub const NOTIFY: Self = Self(4);
    /// dynamic update (RFC 2136)
    pub const UPDATE: Self = Self(5);
    /// DNS Stateful Operations (DSO) [RFC8490]
    pub const DSO: Self = Self(6);

    pub const fn name(self) -> &'static str {
        match self.0 {
            0 => "Query",
            1 => "IQuery",
            2 => "Status",
            4 => "Notify",
            5 => "Update",
            6 => "DSO",
            // [RFC6895]
            3 | 7..=15 => "Reserved",
            _ => "UNKNOWN",
        }
    }
}

impl fmt::Display for OpCode {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.name())
    }
}

wire_enum! {
    /// Listing from IANA https://www.iana.org/assignments/dns-sec-alg-numbers/dns-sec-alg-numbers.xhtml.
    Algorithm(u8), fallback = "Invalid", {
        /// RSA/MD5 [RFC4034][RFC3110]
        RSAMD5 = 1 => "RSAMD5",
        /// DSA/SHA-1 [RFC3755][RFC2536]
        DSA = 3 => "DSA",
        /// RSA/SHA-1 [RFC3110]
        RSASHA1 = 5 => "RSASHA1",
        /// [RFC5155]
        RSASHA1NSEC3SHA1 = 7 => "RSASHA1NSEC3SHA1",
        /// RSA/SHA-256 [RFC5702]
        RSASHA256 = 8 => "RSASHA256",
        /// RSA/SHA-512 [RFC5702]
        RSASHA512 = 10 => "RSASHA512",
        /// ECDSA Curve P-256 with SHA-256 [RFC6605]
        ECDSAP256SHA256 = 13 => "ECDSAP256SHA256",
        /// ECDSA Curve P-384 with SHA-384 [RFC6605]
        ECDSAP384SHA384 = 14 => "ECDSAP384SHA384",
        /// Ed25519 [RFC8080]
        ED25519 = 15 => "ED25519",
        /// Reserved for Private Use
        UNKNOWN = 255 => "Unknown",
    }
}

wire_enum! {
    /// Listing from IANA https://www.iana.org/assignments/dns-sec-alg-numbers/dns-sec-alg-numbers.xhtml.
    DigestType(u8), fallback = "Invalid", {
        /// SHA-1 [RFC3658]
        SHA1 = 1 => "SHA1",
        /// SHA-256 [RFC4509]
        SHA256 = 2 => "SHA256",
        /// GOST R 34.11-94 [RFC5933]
        GOST3411 = 3 => "GOST3411",
        /// SHA-384 [RFC6605]
        SHA384 = 4 => "SHA384",
        /// SHA-512 [RFC6605]
        SHA512 = 5 => "SHA512",
        /// SHA-224 [RFC6605]
        SHA224 = 6 => "SHA224",
        /// Reserved for Private Use
        UNKNOWN = 255 => "Unknown",
    }
}

wire_enum! {
    /// Listing from IANA https://www.iana.org/assignments/dns-sec-alg-numbers/dns-sec-alg-numbers.xhtml.
    Nsec3HashAlgorithm(u8), fallback = "Invalid", {
        /// [RFC5155]
        SHA1 = 1 => "SHA1",
        /// [RFC6605]
        SHA256 = 2 => "SHA256",
        /// [RFC5933]
        GOST3411 = 3 => "GOST3411",
        /// [RFC6605]
        SHA384 = 4 => "SHA384",
        /// [RFC6605]
        SHA512 = 5 => "SHA512",
        /// [RFC6605]
        SHA224 = 6 => "SHA224",
        /// Reserved for Private Use
        UNKNOWN = 255 => "Unknown",
    }
}

// Listings from IANA https://www.iana.org/assignments/dane-parameters/dane-parameters.xhtml.
wire_enum! {
    TlsaCertUsage(u8), fallback = "UNKNOWN", {
        CA_CONSTRAINT = 0 => "CAConstraint",
        SERVICE_CERTIFICATE_CONSTRAINT = 1 => "ServiceCertificateConstraint",
        TRUST_ANCHOR_ASSERTION = 2 => "TrustAnchorAssertion",
        DOMAIN_ISSUED_CERTIFICATE = 3 => "DomainIssuedCertificate",
        PRIVATE = 255 => "Private",
    }
}

wire_enum! {
    TlsaSelector(u8), fallback = "UNKNOWN", {
        FULL_CERTIFICATE = 0 => "FullCertificate",
        SUBJECT_PUBLIC_KEY_INFO = 1 => "SubjectPublicKeyInfo",
        PRIVATE = 255 => "Private",
    }
}

wire_enum! {
    TlsaMatchingType(u8), fallback = "UNKNOWN", {
        FULL = 0 => "Full",
        SHA256 = 1 => "SHA256",
        SHA512 = 2 => "SHA512",
        PRIVATE = 255 => "Private",
    }
}
