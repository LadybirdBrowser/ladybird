/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::cmp::Ordering;
use std::net::{Ipv4Addr, Ipv6Addr};

use super::*;

fn name(text: &str) -> DomainName {
    DomainName::from_string(text)
}

fn header(questions: u16, answers: u16, authorities: u16, additional: u16) -> Vec<u8> {
    let mut out = vec![0x12, 0x34, 0x81, 0x80];
    for count in [questions, answers, authorities, additional] {
        out.extend_from_slice(&count.to_be_bytes());
    }
    out
}

/// TYPE, CLASS IN, TTL 60 and RDLENGTH.
fn rr_fixed(rtype: ResourceType, rd_length: u16) -> Vec<u8> {
    let mut out = rtype.0.to_be_bytes().to_vec();
    out.extend_from_slice(&[0, 1, 0, 0, 0, 60]);
    out.extend_from_slice(&rd_length.to_be_bytes());
    out
}

fn record(owner: &str, rtype: ResourceType, data: RecordData) -> ResourceRecord {
    ResourceRecord {
        name: name(owner),
        rtype,
        class: Class::IN,
        ttl: 300,
        data,
        raw: None,
    }
}

// A label longer than 63 octets cannot be represented in the wire format, whose length field is a 6-bit value.
// Encoding such a label must fail gracefully. Reachable from a query for a host name that contains an over-long label.
#[test]
fn encoding_a_domain_name_with_an_overlong_label_does_not_crash() {
    let domain_name = name(&"a".repeat(64));
    assert_eq!(domain_name.to_wire(), Err(EncodeError::InvalidDomainName));
}

// A label length octet whose top two bits are 0b01 or 0b10 is reserved (RFC 1035, 4.1.4); accepting it as an ordinary
// label of up to 191 octets would produce a name that could no longer be re-encoded. Parsing such a message must fail.
#[test]
fn parsing_a_reserved_label_length_fails() {
    for length in [0x40u8, 0x80] {
        let mut message = header(1, 0, 0, 0);
        message.push(length);
        message.extend(std::iter::repeat_n(b'a', usize::from(length & 0x3f).max(64)));
        message.push(0);
        message.extend_from_slice(&[0, 1, 0, 1]); // QTYPE = A, QCLASS = IN
        assert_eq!(Message::from_wire(&message), Err(ParseError::LabelTooLong));
    }
}

#[test]
fn parsing_an_extended_rsa_exponent_length_uses_network_order() {
    let mut public_key = vec![0, 1, 0];
    public_key.extend(std::iter::repeat_n(0xee, 256));
    public_key.extend_from_slice(&[0xaa, 0xaa]);
    let key = Dnskey {
        flags: 257,
        protocol: 3,
        algorithm: Algorithm::RSASHA256,
        public_key,
    };
    let components = key.rsa_public_key_components().unwrap();
    assert_eq!(components.exponent, &key.public_key[3..259]);
    assert_eq!(components.modulus, &key.public_key[259..]);
}

#[test]
fn rsa_public_key_components_reject_truncated_keys() {
    let key = |public_key: Vec<u8>| Dnskey {
        algorithm: Algorithm::RSASHA256,
        public_key,
        ..Dnskey::default()
    };
    assert!(key(vec![]).rsa_public_key_components().is_none());
    assert!(key(vec![0, 1]).rsa_public_key_components().is_none());
    assert!(key(vec![0, 0, 0, 1]).rsa_public_key_components().is_none());
    // No modulus left after the exponent.
    assert!(key(vec![2, 1, 1]).rsa_public_key_components().is_none());
    assert!(key(vec![0, 1, 0, 9]).rsa_public_key_components().is_none());
    let short = key(vec![1, 3, 0xab, 0xcd]);
    let components = short.rsa_public_key_components().unwrap();
    assert_eq!(components.exponent, &[3]);
    assert_eq!(components.modulus, &[0xab, 0xcd]);
}

#[test]
fn domain_name_validity() {
    assert!(name("").is_valid());
    assert!(name(".").is_valid());
    assert!(name(".").labels.is_empty());
    assert!(name("example.com").is_valid());
    assert!(name("example.com.").is_valid());
    assert_eq!(name("example.com.").labels.len(), 2);
    assert!(!name("example.com..").is_valid());
    assert!(!name("example..com").is_valid());
    assert!(!name(".example.com").is_valid());
    assert!(!name(&"a".repeat(64)).is_valid());

    let overlong = format!("{}a", format!("{}.", "a".repeat(63)).repeat(5));
    assert!(!name(&overlong).is_valid());

    // Exactly 255 octets on the wire: 3 * 64 + 62 + 1.
    let longest = format!("{}{}", format!("{}.", "a".repeat(63)).repeat(3), "a".repeat(61));
    assert!(name(&longest).is_valid());
    assert_eq!(name(&longest).to_wire().unwrap().len(), 255);
    assert!(!name(&format!("{longest}a")).is_valid());
}

#[test]
fn domain_name_presentation() {
    assert_eq!(name("").to_string(), ".");
    assert_eq!(name("Example.COM").to_string(), "Example.COM.");
    assert_eq!(name("Example.COM").to_canonical_string(), b"example.com.");
    assert_eq!(DomainName::root().to_canonical_string(), b".");
    assert_eq!("a.b.".parse::<DomainName>().unwrap(), name("a.b"));
}

#[test]
fn domain_name_helpers() {
    let www = name("www.Example.com");
    assert_eq!(www.parent(), name("Example.com"));
    assert_eq!(DomainName::root().parent(), DomainName::root());
    assert_eq!(www.suffix(2), name("Example.com"));
    assert_eq!(www.suffix(0), DomainName::root());
    assert_eq!(www.suffix(9), www);
    assert_eq!(www.with_label_prepended("*"), name("*.www.Example.com"));
    assert!(www.with_label_prepended("*").is_wildcard());
    assert!(!www.is_wildcard());
    assert!(www.eq_ignore_case(&name("WWW.example.COM")));
    assert!(!www.eq_ignore_case(&name("example.com")));
    assert!(name("EXAMPLE.com").is_ancestor_or_equal_of(&www));
    assert!(www.is_ancestor_or_equal_of(&www));
    assert!(DomainName::root().is_ancestor_or_equal_of(&www));
    assert!(!www.is_ancestor_or_equal_of(&name("example.com")));
    assert!(!name("ample.com").is_ancestor_or_equal_of(&www));
    assert_eq!(www.common_suffix_length(&name("mail.example.com")), 2);
    assert_eq!(www.common_suffix_length(&name("example.org")), 0);
    assert_eq!(www.canonicalized(), name("www.example.com"));
}

#[test]
fn canonical_name_order() {
    // RFC 4034, 6.1. Canonical DNS Name Order.
    let ordered: Vec<DomainName> = [
        vec![b"example".to_vec()],
        vec![b"a".to_vec(), b"example".to_vec()],
        vec![b"yljkjljk".to_vec(), b"a".to_vec(), b"example".to_vec()],
        vec![b"Z".to_vec(), b"a".to_vec(), b"example".to_vec()],
        vec![b"zABC".to_vec(), b"a".to_vec(), b"EXAMPLE".to_vec()],
        vec![b"z".to_vec(), b"example".to_vec()],
        vec![vec![1], b"z".to_vec(), b"example".to_vec()],
        vec![b"*".to_vec(), b"z".to_vec(), b"example".to_vec()],
        vec![vec![200], b"z".to_vec(), b"example".to_vec()],
    ]
    .into_iter()
    .map(|labels| DomainName { labels })
    .collect();

    for (i, a) in ordered.iter().enumerate() {
        for (j, b) in ordered.iter().enumerate() {
            assert_eq!(a.canonical_cmp(b), i.cmp(&j), "{a} vs {b}");
        }
    }
    assert_eq!(name("EXAMPLE").canonical_cmp(&name("example")), Ordering::Equal);
}

#[test]
fn enum_names_round_trip() {
    assert_eq!(ResourceType::AAAA.to_string(), "AAAA");
    assert_eq!(ResourceType::NULL.name(), "NULL_");
    assert_eq!(ResourceType::from_name("NSAP_PTR"), Some(ResourceType::NSAP_PTR));
    assert_eq!(ResourceType::from_name("DLV"), Some(ResourceType(32769)));
    assert_eq!(ResourceType::from_name("aaaa"), None);
    assert_eq!(ResourceType(65280).name(), "UNKNOWN");
    assert_eq!(Class(2).name(), "UNKNOWN");
    assert_eq!(Class::IN.to_string(), "IN");
    assert_eq!(ResponseCode::NAME_ERROR.to_string(), "NameError");
    assert_eq!(ResponseCode(9).to_string(), "UNKNOWN");
    assert_eq!(OpCode::NOTIFY.to_string(), "Notify");
    assert_eq!(OpCode(3).to_string(), "Reserved");
    assert_eq!(OpCode(12).to_string(), "Reserved");
    assert_eq!(Algorithm::ED25519.to_string(), "ED25519");
    assert_eq!(Algorithm(2).to_string(), "Invalid");
    assert_eq!(Algorithm::UNKNOWN.to_string(), "Unknown");
    assert_eq!(DigestType::SHA384.to_string(), "SHA384");
    assert_eq!(Nsec3HashAlgorithm(9).to_string(), "Invalid");
}

#[test]
fn header_options() {
    let mut options = Options::default();
    assert!(options.is_question());
    options.set_is_question(false);
    options.set_op_code(OpCode::UPDATE);
    options.set_recursion_desired(true);
    options.set_checking_disabled(true);
    options.set_response_code(ResponseCode::REFUSED);
    assert!(!options.is_question());
    assert_eq!(options.op_code(), OpCode::UPDATE);
    assert!(options.recursion_desired());
    assert!(options.checking_disabled());
    assert!(!options.authenticated_data());
    assert_eq!(options.response_code(), ResponseCode::REFUSED);
    assert_eq!(
        options.to_string(),
        "QR: R, Opcode: Update, AA: false, TC: false, RD: true, RA: false, AD: false, CD: true, RCODE: Refused"
    );

    // Out-of-range codes do not spill into neighbouring fields.
    let mut options = Options::default();
    options.set_op_code(OpCode(0xff));
    options.set_response_code(ResponseCode(0xfff0));
    assert!(options.is_question());
    assert_eq!(options.0, Options::OP_CODE_MASK);
}

fn sample_message() -> Message {
    let mut opt = Opt {
        udp_payload_size: 4096,
        ..Opt::default()
    };
    opt.set_dnssec_ok(true);
    opt.options.push(OptOption {
        code: 10,
        data: vec![1, 2, 3, 4, 5, 6, 7, 8],
    });

    let mut options = Options::default();
    options.set_is_question(false);
    options.set_recursion_desired(true);

    Message {
        header: Header {
            id: 0xbeef,
            options,
            ..Header::default()
        },
        questions: vec![Question {
            name: name("www.example.com"),
            rtype: ResourceType::A,
            class: Class::IN,
        }],
        answers: vec![
            record(
                "www.example.com",
                ResourceType::CNAME,
                RecordData::Cname(name("web.example.com")),
            ),
            record(
                "web.example.com",
                ResourceType::A,
                RecordData::A(Ipv4Addr::new(192, 0, 2, 1)),
            ),
            record(
                "web.example.com",
                ResourceType::AAAA,
                RecordData::Aaaa(Ipv6Addr::new(0x2001, 0xdb8, 0, 0, 0, 0, 0, 1)),
            ),
            record(
                "example.com",
                ResourceType::TXT,
                RecordData::Txt(Txt {
                    strings: vec![b"v=spf1 -all".to_vec()],
                }),
            ),
            record(
                "example.com",
                ResourceType::MX,
                RecordData::Mx(Mx {
                    preference: 10,
                    exchange: name("mail.example.com"),
                }),
            ),
            record(
                "_sip._tcp.example.com",
                ResourceType::SRV,
                RecordData::Srv(Srv {
                    priority: 1,
                    weight: 2,
                    port: 5060,
                    target: name("sip.example.com"),
                }),
            ),
            record(
                "example.com",
                ResourceType::HINFO,
                RecordData::Hinfo(Hinfo {
                    cpu: b"x86".to_vec(),
                    os: b"Ladybird".to_vec(),
                }),
            ),
            record(
                "_443._tcp.example.com",
                ResourceType::TLSA,
                RecordData::Tlsa(Tlsa {
                    cert_usage: TlsaCertUsage::DOMAIN_ISSUED_CERTIFICATE,
                    selector: TlsaSelector::SUBJECT_PUBLIC_KEY_INFO,
                    matching_type: TlsaMatchingType::SHA256,
                    certificate_association_data: vec![0xab; 32],
                }),
            ),
            record("example.com", ResourceType(65280), RecordData::Unknown(vec![9, 8, 7])),
        ],
        authorities: vec![
            record(
                "example.com",
                ResourceType::SOA,
                RecordData::Soa(Soa {
                    mname: name("ns1.example.com"),
                    rname: name("hostmaster.example.com"),
                    serial: 2026112401,
                    refresh: 7200,
                    retry: 3600,
                    expire: 1209600,
                    minimum: 300,
                }),
            ),
            record("example.com", ResourceType::NS, RecordData::Ns(name("ns1.example.com"))),
            record(
                "example.com",
                ResourceType::DNSKEY,
                RecordData::Dnskey(Dnskey {
                    flags: 257,
                    protocol: 3,
                    algorithm: Algorithm::ECDSAP256SHA256,
                    public_key: vec![0x42; 64],
                }),
            ),
            record(
                "example.com",
                ResourceType::DS,
                RecordData::Ds(Ds {
                    key_tag: 12345,
                    algorithm: Algorithm::ECDSAP256SHA256,
                    digest_type: DigestType::SHA256,
                    digest: vec![0x11; 32],
                }),
            ),
            record(
                "example.com",
                ResourceType::RRSIG,
                RecordData::Rrsig(Sig {
                    type_covered: ResourceType::SOA,
                    algorithm: Algorithm::ECDSAP256SHA256,
                    label_count: 2,
                    original_ttl: 3600,
                    expiration: 1_700_000_000,
                    inception: 1_690_000_000,
                    key_tag: 12345,
                    signers_name: name("example.com"),
                    signature: vec![0x55; 64],
                }),
            ),
            record(
                "example.com",
                ResourceType::NSEC,
                RecordData::Nsec(Nsec {
                    next_domain_name: name("a.example.com"),
                    types: vec![
                        ResourceType::A,
                        ResourceType::NS,
                        ResourceType::SOA,
                        ResourceType::RRSIG,
                        ResourceType::NSEC,
                        ResourceType::DNSKEY,
                        ResourceType::CAA,
                    ],
                }),
            ),
            record(
                "0p9mhaveqvm6t7vbl5lop2u3t2rp3tom.example.com",
                ResourceType::NSEC3,
                RecordData::Nsec3(Nsec3 {
                    hash_algorithm: Nsec3HashAlgorithm::SHA1,
                    flags: Nsec3::FLAG_OPT_OUT,
                    iterations: 0,
                    salt: vec![0xaa, 0xbb],
                    next_hashed_owner_name: vec![0x77; 20],
                    types: vec![ResourceType::NS, ResourceType::DS, ResourceType::RRSIG],
                }),
            ),
            record(
                "example.com",
                ResourceType::NSEC3PARAM,
                RecordData::Nsec3Param(Nsec3Param {
                    hash_algorithm: Nsec3HashAlgorithm::SHA1,
                    flags: 0,
                    iterations: 0,
                    salt: vec![],
                }),
            ),
            record(
                "ptr.example.com",
                ResourceType::PTR,
                RecordData::Ptr(name("host.example.com")),
            ),
            record(
                "example.com",
                ResourceType::CDNSKEY,
                RecordData::Cdnskey(Dnskey {
                    flags: 256,
                    protocol: 3,
                    algorithm: Algorithm::ED25519,
                    public_key: vec![0x01; 32],
                }),
            ),
            record(
                "example.com",
                ResourceType::CDS,
                RecordData::Cds(Ds {
                    key_tag: 1,
                    algorithm: Algorithm::ED25519,
                    digest_type: DigestType::SHA1,
                    digest: vec![0x22; 20],
                }),
            ),
        ],
        additional_records: vec![ResourceRecord {
            name: DomainName::root(),
            rtype: ResourceType::OPT,
            class: Class::IN,
            ttl: 0,
            data: RecordData::Opt(opt),
            raw: None,
        }],
    }
}

fn without_raw(mut message: Message) -> Message {
    for record in message
        .answers
        .iter_mut()
        .chain(&mut message.authorities)
        .chain(&mut message.additional_records)
    {
        record.raw = None;
    }
    message
}

#[test]
fn message_round_trips() {
    let message = sample_message();
    let wire = message.to_wire().unwrap();
    let parsed = Message::from_wire(&wire).unwrap();

    assert_eq!(parsed.header.question_count, 1);
    assert_eq!(parsed.header.answer_count, 9);
    assert_eq!(parsed.header.authority_count, 11);
    assert_eq!(parsed.header.additional_count, 1);

    let mut expected = message;
    expected.header = parsed.header;
    assert_eq!(without_raw(parsed.clone()), expected);
    assert_eq!(parsed.to_wire().unwrap(), wire);

    // Without compression, every record's raw bytes are its own encoding.
    for record in parsed
        .answers
        .iter()
        .chain(&parsed.authorities)
        .chain(&parsed.additional_records)
    {
        let mut encoded = Vec::new();
        record.write_wire(&mut encoded).unwrap();
        assert_eq!(record.raw.as_deref(), Some(encoded.as_slice()), "{record}");
    }

    let opt = match &parsed.additional_records[0].data {
        RecordData::Opt(opt) => opt.clone(),
        other => panic!("unexpected {other:?}"),
    };
    assert!(opt.dnssec_ok());
    assert_eq!(opt.udp_payload_size, 4096);
    assert_eq!(opt.version(), 0);
}

#[test]
fn query_with_dnssec_ok_encodes_like_the_resolver_expects() {
    let mut opt = Opt {
        udp_payload_size: 4096,
        ..Opt::default()
    };
    opt.set_dnssec_ok(true);
    let mut options = Options::default();
    options.set_recursion_desired(true);
    options.set_checking_disabled(true);
    options.set_authenticated_data(true);
    let query = Message {
        header: Header {
            id: 0x0102,
            options,
            ..Header::default()
        },
        questions: vec![Question {
            name: name("a.b"),
            rtype: ResourceType::AAAA,
            class: Class::IN,
        }],
        additional_records: vec![ResourceRecord {
            name: DomainName::root(),
            rtype: ResourceType::OPT,
            class: Class::IN,
            ttl: 0,
            data: RecordData::Opt(opt),
            raw: None,
        }],
        ..Message::default()
    };

    let mut expected = vec![1, 2, 0x01, 0x30, 0, 1, 0, 0, 0, 0, 0, 1];
    expected.extend_from_slice(&[1, b'a', 1, b'b', 0, 0, 28, 0, 1]);
    expected.extend_from_slice(&[0, 0, 41, 0x10, 0x00, 0, 0, 0x80, 0, 0, 0]);
    assert_eq!(query.to_wire().unwrap(), expected);
}

#[test]
fn opt_type_and_data_must_agree() {
    let mismatched = record("example.com", ResourceType::OPT, RecordData::Unknown(vec![]));
    assert_eq!(mismatched.write_wire(&mut Vec::new()), Err(EncodeError::MismatchedOpt));
    let mismatched = record("example.com", ResourceType::A, RecordData::Opt(Opt::default()));
    assert_eq!(mismatched.write_wire(&mut Vec::new()), Err(EncodeError::MismatchedOpt));
}

#[test]
fn encoding_rejects_oversized_fields() {
    let txt = record(
        "example.com",
        ResourceType::TXT,
        RecordData::Txt(Txt {
            strings: vec![vec![b'a'; 256]],
        }),
    );
    assert_eq!(txt.write_wire(&mut Vec::new()), Err(EncodeError::FieldTooLong));

    let unknown = record("example.com", ResourceType(65280), RecordData::Unknown(vec![0; 65536]));
    assert_eq!(unknown.write_wire(&mut Vec::new()), Err(EncodeError::TooLarge));

    let opt = Opt {
        options: vec![
            OptOption {
                code: 1,
                data: vec![0; 40000],
            },
            OptOption {
                code: 2,
                data: vec![0; 40000],
            },
        ],
        ..Opt::default()
    };
    assert_eq!(opt.write_wire(&mut Vec::new()), Err(EncodeError::TooLarge));
}

#[test]
fn compressed_names_resolve() {
    // Question: www.example.com A IN at offset 12. Answer owner points at it (0xc00c), CNAME RDATA is "web" followed
    // by a pointer to "example.com" (offset 16), and a second answer's owner points into that RDATA.
    let mut message = header(1, 2, 0, 0);
    message.extend_from_slice(b"\x03www\x07example\x03com\x00");
    message.extend_from_slice(&[0, 1, 0, 1]);
    message.extend_from_slice(&[0xc0, 0x0c]);
    message.extend(rr_fixed(ResourceType::CNAME, 6));
    let cname_rdata_offset = message.len();
    message.extend_from_slice(&[3, b'w', b'e', b'b', 0xc0, 16]);
    message.extend_from_slice(&[0xc0, cname_rdata_offset as u8]);
    message.extend(rr_fixed(ResourceType::A, 4));
    message.extend_from_slice(&[192, 0, 2, 1]);

    let parsed = Message::from_wire(&message).unwrap();
    assert_eq!(parsed.answers[0].name, name("www.example.com"));
    assert_eq!(parsed.answers[0].data, RecordData::Cname(name("web.example.com")));
    assert_eq!(parsed.answers[1].name, name("web.example.com"));
    assert_eq!(parsed.answers[1].data, RecordData::A(Ipv4Addr::new(192, 0, 2, 1)));
    assert_eq!(parsed.answers[0].raw.as_deref().unwrap()[..2], [0xc0, 0x0c]);
    assert_eq!(parsed.answers[0].raw.as_ref().unwrap().len(), 2 + 10 + 6);

    // Pointing at the terminating zero octet of a name yields the root.
    let mut message = header(2, 0, 0, 0);
    message.extend_from_slice(b"\x01a\x00");
    message.extend_from_slice(&[0, 1, 0, 1]);
    message.extend_from_slice(&[0xc0, 14]);
    message.extend_from_slice(&[0, 1, 0, 1]);
    assert_eq!(
        Message::from_wire(&message).unwrap().questions[1].name,
        DomainName::root()
    );

    // A pointer to a name that itself ends in a pointer.
    let mut message = header(3, 0, 0, 0);
    message.extend_from_slice(b"\x01a\x01b\x00\x00\x01\x00\x01"); // a.b at 12
    message.extend_from_slice(b"\x01c\xc0\x0c\x00\x01\x00\x01"); // c.a.b at 21, pointer at 23
    message.extend_from_slice(b"\xc0\x17\x00\x01\x00\x01"); // -> 23
    assert_eq!(Message::from_wire(&message).unwrap().questions[2].name, name("a.b"));
}

fn question_with_name(name_bytes: &[u8]) -> Vec<u8> {
    let mut message = header(1, 0, 0, 0);
    message.extend_from_slice(name_bytes);
    message.extend_from_slice(&[0, 1, 0, 1]);
    message
}

#[test]
fn compression_pointer_to_itself_is_rejected() {
    assert_eq!(
        Message::from_wire(&question_with_name(&[0xc0, 12])),
        Err(ParseError::InvalidPointer)
    );
}

#[test]
fn compression_pointer_into_the_same_name_is_rejected() {
    // "a" then a pointer back to "a": a loop through the name being decoded.
    assert_eq!(
        Message::from_wire(&question_with_name(&[1, b'a', 0xc0, 12])),
        Err(ParseError::InvalidPointer)
    );
}

#[test]
fn compression_pointer_loop_between_names_is_rejected() {
    // Two questions whose names point at each other.
    let mut message = header(2, 0, 0, 0);
    message.extend_from_slice(&[0xc0, 18, 0, 1, 0, 1]);
    message.extend_from_slice(&[0xc0, 12, 0, 1, 0, 1]);
    assert_eq!(Message::from_wire(&message), Err(ParseError::InvalidPointer));
}

#[test]
fn forward_compression_pointer_is_rejected() {
    let mut message = header(2, 0, 0, 0);
    message.extend_from_slice(&[0xc0, 18, 0, 1, 0, 1]);
    message.extend_from_slice(b"\x01a\x00\x00\x01\x00\x01");
    assert_eq!(Message::from_wire(&message), Err(ParseError::InvalidPointer));
}

#[test]
fn compression_pointer_past_end_is_rejected() {
    assert_eq!(
        Message::from_wire(&question_with_name(&[0xff, 0xff])),
        Err(ParseError::InvalidPointer)
    );
    // Truncated pointer.
    assert_eq!(
        Message::from_wire(&question_with_name(&[0xc0])[..13]),
        Err(ParseError::UnexpectedEnd)
    );
}

#[test]
fn compression_pointer_into_the_middle_of_a_label_is_rejected() {
    let mut message = header(2, 0, 0, 0);
    message.extend_from_slice(b"\x07example\x00\x00\x01\x00\x01");
    message.extend_from_slice(&[0xc0, 14, 0, 1, 0, 1]);
    assert_eq!(Message::from_wire(&message), Err(ParseError::InvalidPointer));
}

#[test]
fn compression_pointer_past_a_name_into_other_fields_is_rejected() {
    // Offset 15 is QTYPE of the first question, right after its name.
    let mut message = header(2, 0, 0, 0);
    message.extend_from_slice(b"\x01a\x00\x00\x01\x00\x01");
    message.extend_from_slice(&[0xc0, 15, 0, 1, 0, 1]);
    assert_eq!(Message::from_wire(&message), Err(ParseError::InvalidPointer));
}

#[test]
fn compression_pointer_into_unparsed_rdata_is_rejected() {
    // RDATA of an unknown type is opaque and never registers names.
    let mut message = header(1, 1, 0, 0);
    message.extend_from_slice(b"\x00\x00\x01\x00\x01");
    message.push(0);
    message.extend(rr_fixed(ResourceType(65280), 3));
    let opaque_offset = message.len();
    message.extend_from_slice(b"\x01a\x00");
    let mut with_pointer = message.clone();
    with_pointer[7] = 2;
    with_pointer.extend_from_slice(&[0xc0, opaque_offset as u8]);
    with_pointer.extend(rr_fixed(ResourceType::A, 4));
    with_pointer.extend_from_slice(&[1, 2, 3, 4]);
    assert!(Message::from_wire(&message).is_ok());
    assert_eq!(Message::from_wire(&with_pointer), Err(ParseError::InvalidPointer));
}

#[test]
fn oversized_names_are_rejected() {
    let label = |out: &mut Vec<u8>| {
        out.push(63);
        out.extend(std::iter::repeat_n(b'a', 63));
    };

    // 4 * 64 + 1 = 257 octets.
    let mut long = Vec::new();
    for _ in 0..4 {
        label(&mut long);
    }
    long.push(0);
    assert_eq!(
        Message::from_wire(&question_with_name(&long)),
        Err(ParseError::NameTooLong)
    );

    // 3 * 64 + 1 = 193 octets is fine, but prepending two labels through a pointer is not.
    let mut message = header(2, 0, 0, 0);
    for _ in 0..3 {
        label(&mut message);
    }
    message.push(0);
    message.extend_from_slice(&[0, 1, 0, 1]);
    label(&mut message);
    message.extend_from_slice(&[0xc0, 12, 0, 1, 0, 1]);
    assert_eq!(Message::from_wire(&message), Err(ParseError::NameTooLong));
}

#[test]
fn many_short_labels_are_rejected() {
    let mut long = Vec::new();
    for _ in 0..128 {
        long.extend_from_slice(&[1, b'a']);
    }
    long.push(0);
    assert_eq!(
        Message::from_wire(&question_with_name(&long)),
        Err(ParseError::NameTooLong)
    );
}

fn single_answer(rtype: ResourceType, rdata: &[u8], rd_length: u16) -> Vec<u8> {
    let mut message = header(0, 1, 0, 0);
    message.push(0);
    message.extend(rr_fixed(rtype, rd_length));
    message.extend_from_slice(rdata);
    message
}

#[test]
fn truncated_rdata_is_rejected() {
    // RDLENGTH runs past the end of the message.
    assert_eq!(
        Message::from_wire(&single_answer(ResourceType::A, &[1, 2, 3, 4], 5)),
        Err(ParseError::UnexpectedEnd)
    );
    // RDLENGTH is too short for the fields, which may not be read from beyond it.
    let mut message = single_answer(ResourceType::A, &[1, 2, 3], 3);
    message.push(4);
    assert_eq!(Message::from_wire(&message), Err(ParseError::UnexpectedEnd));
    // A name in RDATA may not run past RDLENGTH either.
    let mut message = single_answer(ResourceType::NS, b"\x01a", 2);
    message.push(0);
    assert_eq!(Message::from_wire(&message), Err(ParseError::UnexpectedEnd));
    assert_eq!(
        Message::from_wire(&single_answer(ResourceType::TXT, &[5, b'a'], 2)),
        Err(ParseError::UnexpectedEnd)
    );
    assert_eq!(
        Message::from_wire(&single_answer(ResourceType::TXT, &[], 0)),
        Err(ParseError::UnexpectedEnd)
    );
    assert_eq!(
        Message::from_wire(&single_answer(ResourceType::SOA, b"\x00\x00\x00\x00", 4)),
        Err(ParseError::UnexpectedEnd)
    );
}

#[test]
fn trailing_rdata_is_rejected() {
    assert_eq!(
        Message::from_wire(&single_answer(ResourceType::A, &[1, 2, 3, 4, 5], 5)),
        Err(ParseError::TrailingRecordData(ResourceType::A))
    );
}

#[test]
fn lying_counts_are_rejected() {
    let mut message = header(0, 0xffff, 0xffff, 0xffff);
    message.push(0);
    message.extend(rr_fixed(ResourceType::A, 4));
    message.extend_from_slice(&[1, 2, 3, 4]);
    assert_eq!(Message::from_wire(&message), Err(ParseError::UnexpectedEnd));
    assert_eq!(Message::from_wire(&header(1, 0, 0, 0)), Err(ParseError::UnexpectedEnd));
    assert_eq!(Message::from_wire(&[0; 11]), Err(ParseError::UnexpectedEnd));
}

#[test]
fn record_specific_checks() {
    assert_eq!(
        Message::from_wire(&single_answer(ResourceType::DNSKEY, &[1, 1, 3, 8], 4)),
        Err(ParseError::EmptyPublicKey)
    );

    // A DS with a digest type we do not know keeps its digest as it is.
    let mut ds = vec![0, 1, 8, 7];
    ds.extend_from_slice(&[0xcc; 32]);
    let message = Message::from_wire(&single_answer(ResourceType::DS, &ds, 36)).unwrap();
    let RecordData::Ds(parsed) = &message.answers[0].data else {
        panic!("not a DS")
    };
    assert_eq!(parsed.digest_type, DigestType(7));
    assert_eq!(parsed.digest, vec![0xcc; 32]);
    // A SHA-256 digest that is too short.
    let ds = [0, 1, 8, 2, 0xaa, 0xbb];
    assert_eq!(
        Message::from_wire(&single_answer(ResourceType::DS, &ds, 6)),
        Err(ParseError::UnexpectedEnd)
    );

    let nsec3 = [1, 0, 0, 0, 0, 0];
    assert_eq!(
        Message::from_wire(&single_answer(ResourceType::NSEC3, &nsec3, 6)),
        Err(ParseError::EmptyNextHashedOwnerName)
    );
    let nsec3 = [1, 0, 0, 0, 4, 1, 2];
    assert_eq!(
        Message::from_wire(&single_answer(ResourceType::NSEC3, &nsec3, 7)),
        Err(ParseError::UnexpectedEnd)
    );
}

#[test]
fn type_bit_maps() {
    let types = [
        ResourceType::A,
        ResourceType::MX,
        ResourceType::RRSIG,
        ResourceType::CAA,
        ResourceType(65535),
    ];
    let mut wire = Vec::new();
    type_bit_maps_to_wire(&types, &mut wire);
    assert_eq!(&wire[..8], &[0, 6, 0x40, 0x01, 0, 0, 0, 0x02]);
    assert_eq!(&wire[8..11], &[1, 1, 0x40]);
    assert_eq!(&wire[11..13], &[255, 32]);
    assert_eq!(wire.len(), 13 + 32);
    assert_eq!(type_bit_maps_from_wire(&wire).unwrap(), types);

    assert_eq!(type_bit_maps_from_wire(&[]).unwrap(), []);
    // Windows out of order, repeated, empty or too long.
    assert_eq!(
        type_bit_maps_from_wire(&[1, 1, 0x40, 0, 1, 0x40]),
        Err(ParseError::InvalidTypeBitMap)
    );
    assert_eq!(
        type_bit_maps_from_wire(&[0, 1, 0x40, 0, 1, 0x20]),
        Err(ParseError::InvalidTypeBitMap)
    );
    assert_eq!(type_bit_maps_from_wire(&[0, 0]), Err(ParseError::InvalidTypeBitMap));
    let mut too_long = vec![0, 33];
    too_long.extend_from_slice(&[0xff; 33]);
    assert_eq!(type_bit_maps_from_wire(&too_long), Err(ParseError::InvalidTypeBitMap));
    // Truncated bitmap and truncated window header.
    assert_eq!(type_bit_maps_from_wire(&[0, 2, 0x40]), Err(ParseError::UnexpectedEnd));
    assert_eq!(type_bit_maps_from_wire(&[0]), Err(ParseError::UnexpectedEnd));

    // The same checks apply inside an NSEC record.
    let nsec = [0, 1, 1, 0x40, 0, 1, 0x40];
    assert_eq!(
        Message::from_wire(&single_answer(ResourceType::NSEC, &nsec, 7)),
        Err(ParseError::InvalidTypeBitMap)
    );
}

#[test]
fn opt_options_must_fill_rdata() {
    let opt = |rdata: &[u8], rd_length: u16| {
        let mut message = header(0, 0, 0, 1);
        message.extend_from_slice(&[0, 0, 41, 0x10, 0, 0, 0, 0, 0]);
        message.extend_from_slice(&rd_length.to_be_bytes());
        message.extend_from_slice(rdata);
        message
    };
    // An option whose length runs past RDLENGTH, even though the message continues.
    assert_eq!(
        Message::from_wire(&opt(&[0, 10, 0, 8, 1, 2, 3, 4, 5, 6, 7, 8], 8)),
        Err(ParseError::InvalidOpt)
    );
    // RDLENGTH too short for an option header.
    assert_eq!(Message::from_wire(&opt(&[0, 10, 0], 3)), Err(ParseError::InvalidOpt));
    // RDLENGTH past the end of the message.
    assert_eq!(
        Message::from_wire(&opt(&[0, 10, 0, 0], 8)),
        Err(ParseError::UnexpectedEnd)
    );

    let parsed = Message::from_wire(&opt(&[0, 10, 0, 2, 0xab, 0xcd], 6)).unwrap();
    let record = &parsed.additional_records[0];
    assert_eq!(record.class, Class::IN);
    assert_eq!(record.ttl, 0);
    assert_eq!(record.raw.as_ref().unwrap().len(), 1 + 2 + 2 + 4 + 2 + 6);
    assert_eq!(
        record.to_string(),
        "[IN OPT OPT UDP Payload Size: 4096, Extended RCode: 0, Version: 0, DNSSEC OK: false, opt[10 = 'abcd'] | ttl=0, name=.]"
    );
}

#[test]
fn unknown_types_round_trip() {
    let message = single_answer(ResourceType(65280), &[0xde, 0xad], 2);
    let parsed = Message::from_wire(&message).unwrap();
    assert_eq!(parsed.answers[0].rtype, ResourceType(65280));
    assert_eq!(parsed.answers[0].data, RecordData::Unknown(vec![0xde, 0xad]));
    assert_eq!(parsed.answers[0].to_string(), "[IN UNKNOWN dead | ttl=60, name=.]");
    assert_eq!(parsed.to_wire().unwrap(), message);

    let parsed = Message::from_wire(&single_answer(ResourceType::SIG, &[1], 1)).unwrap();
    assert_eq!(parsed.answers[0].data, RecordData::Unknown(vec![1]));
}

#[test]
fn truncated_or_corrupted_messages_never_panic() {
    let wire = sample_message().to_wire().unwrap();
    for length in 0..wire.len() {
        assert!(Message::from_wire(&wire[..length]).is_err(), "length {length}");
    }
    let mut state: u32 = 0x1234_5678;
    for _ in 0..20000 {
        let mut corrupted = wire.clone();
        for _ in 0..4 {
            state = state.wrapping_mul(1_103_515_245).wrapping_add(12345);
            let index = (state >> 8) as usize % corrupted.len();
            corrupted[index] = (state >> 24) as u8;
        }
        if let Ok(message) = Message::from_wire(&corrupted) {
            let _ = message.to_wire();
            for record in &message.answers {
                let _ = record.to_string();
            }
        }
    }
}

#[test]
fn key_tag() {
    // RFC 4034, Appendix B. Key Tag Calculation, worked by hand.
    let key = Dnskey {
        flags: 0x0101,
        protocol: 3,
        algorithm: Algorithm::RSASHA256,
        public_key: vec![1, 2, 3],
    };
    assert_eq!(key.calculated_key_tag(), 0x080b);
    let key = Dnskey {
        public_key: vec![0xff; 4],
        ..key
    };
    assert_eq!(key.calculated_key_tag(), 0x0409);

    // The 2017 root KSK.
    let root = Dnskey {
        flags: 257,
        protocol: 3,
        algorithm: Algorithm::RSASHA256,
        public_key: decode_base64(
            "AwEAAaz/tAm8yTn4Mfeh5eyI96WSVexTBAvkMgJzkKTOiW1vkIbzxeF3+/4RgWOq7HrxRixHlFlExOLAJr5emLvN7SWXgnLh4+B5xQlNVz8Og8kvArMtNROxVQuCaSnIDdD5LKyWbRd2n9WGe2R8PzgCmr3EgVLrjyBxWezF0jLHwVN8efS3rCj/EWgvIWgb9tarpVUDK/b58Da+sqqls3eNbuv7pr+eoZG+SrDK6nWeL3c6H5Apxz7LjVc1uTIdsIXxuOLYA4/ilBmSVIzuDWfdRUfhHdY6+cn8HFRm+2hM8AnXGXws9555KrUB5qihylGa8subX2Nn6UwNR1AkUTV74bU=",
        ),
    };
    assert_eq!(root.calculated_key_tag(), 20326);
    assert!(root.is_key_signing_key());
    let components = root.rsa_public_key_components().unwrap();
    assert_eq!(components.exponent, &[1, 0, 1]);
    assert_eq!(components.modulus.len(), 256);
}

fn decode_base64(text: &str) -> Vec<u8> {
    const ALPHABET: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = Vec::new();
    let mut accumulator = 0u32;
    let mut bits = 0;
    for byte in text.bytes().filter(|byte| *byte != b'=') {
        let value = ALPHABET.iter().position(|symbol| *symbol == byte).unwrap() as u32;
        accumulator = accumulator << 6 | value;
        bits += 6;
        if bits >= 8 {
            bits -= 8;
            out.push((accumulator >> bits) as u8);
        }
    }
    out
}

#[test]
fn record_presentation() {
    let message = sample_message();
    let texts: Vec<String> = message
        .answers
        .iter()
        .chain(&message.authorities)
        .map(ToString::to_string)
        .collect();
    let expected = [
        "[IN CNAME web.example.com. | ttl=300, name=www.example.com.]",
        "[IN A 192.0.2.1 | ttl=300, name=web.example.com.]",
        "[IN AAAA 2001:db8::1 | ttl=300, name=web.example.com.]",
        "[IN TXT Text: 'v=spf1 -all' | ttl=300, name=example.com.]",
        "[IN MX MX Preference: 10, Exchange: 'mail.example.com.' | ttl=300, name=example.com.]",
        "[IN SRV SRV Priority: 1, Weight: 2, Port: 5060, Target: 'sip.example.com.' | ttl=300, name=_sip._tcp.example.com.]",
        "[IN HINFO HINFO CPU: 'x86', OS: 'Ladybird' | ttl=300, name=example.com.]",
        "[IN TLSA TLSA Usage: 3, Selector: 1, Matching: 1, Data: abababababababababababababababababababababababababababababababab | ttl=300, name=_443._tcp.example.com.]",
        "[IN UNKNOWN 090807 | ttl=300, name=example.com.]",
        "[IN SOA SOA MName: 'ns1.example.com.', RName: 'hostmaster.example.com.', Serial: 2026112401, Refresh: 7200, Retry: 3600, Expire: 1209600, Minimum: 300 | ttl=300, name=example.com.]",
        "[IN NS ns1.example.com. | ttl=300, name=example.com.]",
        "[IN DNSKEY DNSKEY Flags: sep zone ksk (257), Protocol: 3, Algorithm: ECDSAP256SHA256, Public Key: QkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQg==, Tag: 19542 | ttl=300, name=example.com.]",
        "[IN DS DS Key Tag: 12345, Algorithm: ECDSAP256SHA256, Digest Type: SHA256, Digest: ERERERERERERERERERERERERERERERERERERERERERE= | ttl=300, name=example.com.]",
        "[IN RRSIG SIG Type covered: SOA, Algorithm: ECDSAP256SHA256, Labels: 2, Original TTL: 3600, Signature expiration: 2023-11-14 22:13:20, Signature inception: 2023-07-22 04:26:40, Key tag: 12345, Signer's name: 'example.com.', Signature: VVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVQ== | ttl=300, name=example.com.]",
        "[IN NSEC NSEC Next: 'a.example.com.', Types: A NS SOA RRSIG NSEC DNSKEY CAA | ttl=300, name=example.com.]",
        "[IN NSEC3 NSEC3 Hash: SHA1, Flags: 1, Iterations: 0, Salt: aabb, Next: 7777777777777777777777777777777777777777, Types: NS DS RRSIG | ttl=300, name=0p9mhaveqvm6t7vbl5lop2u3t2rp3tom.example.com.]",
        "[IN NSEC3PARAM NSEC3PARAM Hash: SHA1, Flags: 0, Iterations: 0, Salt:  | ttl=300, name=example.com.]",
        "[IN PTR host.example.com. | ttl=300, name=ptr.example.com.]",
        "[IN CDNSKEY DNSKEY Flags: zone (256), Protocol: 3, Algorithm: ED25519, Public Key: AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE=, Tag: 5151 | ttl=300, name=example.com.]",
        "[IN CDS DS Key Tag: 1, Algorithm: ED25519, Digest Type: SHA1, Digest: IiIiIiIiIiIiIiIiIiIiIiIiIiI= | ttl=300, name=example.com.]",
    ];
    assert_eq!(texts.len(), expected.len());
    for (text, expected) in texts.iter().zip(expected) {
        assert_eq!(text, expected);
    }

    let a = record("x", ResourceType::A, RecordData::A(Ipv4Addr::new(127, 0, 0, 1)));
    assert_eq!(a.data.to_string(), "127.0.0.1");
    let sig = Sig {
        expiration: 0,
        inception: u32::MAX,
        ..Sig::default()
    };
    assert!(
        sig.to_string()
            .contains("Signature expiration: 1970-01-01 00:00:00, Signature inception: 2106-02-07 06:28:15")
    );
}

#[test]
fn canonical_form() {
    let owner = name("example.com");
    let ns = record("EXAMPLE.com", ResourceType::NS, RecordData::Ns(name("NS1.Example.COM")));
    let mut canonical = Vec::new();
    ns.write_canonical_wire(&mut canonical, &owner, 3600).unwrap();
    let mut expected = owner.to_wire().unwrap();
    expected.extend_from_slice(&[0, 2, 0, 1, 0, 0, 0x0e, 0x10, 0, 17]);
    expected.extend_from_slice(b"\x03ns1\x07example\x03com\x00");
    assert_eq!(canonical, expected);

    // RFC 6840, 5.1: NSEC RDATA names keep their case, RRSIG signer names do not.
    let nsec = RecordData::Nsec(Nsec {
        next_domain_name: name("A.Example"),
        types: vec![ResourceType::A],
    });
    assert_eq!(nsec.canonicalized(), nsec);
    let rrsig = RecordData::Rrsig(Sig {
        signers_name: name("Example"),
        ..Sig::default()
    });
    let RecordData::Rrsig(canonical) = rrsig.canonicalized() else {
        panic!("not an RRSIG");
    };
    assert_eq!(canonical.signers_name.labels, [b"example".to_vec()]);

    let mut excluding = Vec::new();
    canonical.write_wire_excluding_signature(&mut excluding).unwrap();
    assert_eq!(excluding.len(), 18 + 9);
}

#[test]
fn dname_records() {
    // An answer owned by the question name, with a DNAME target of "Target" followed by a pointer to "example.com".
    let mut message = header(1, 1, 0, 0);
    message.extend_from_slice(b"\x03old\x07example\x03com\x00");
    message.extend_from_slice(&[0, 39, 0, 1]);
    message.extend_from_slice(&[0xc0, 0x0c]);
    message.extend(rr_fixed(ResourceType::DNAME, 9));
    message.extend_from_slice(b"\x06Target\xc0\x10");

    let parsed = Message::from_wire(&message).unwrap();
    let dname = &parsed.answers[0];
    assert_eq!(dname.data, RecordData::Dname(name("Target.example.com")));
    assert_eq!(
        dname.to_string(),
        "[IN DNAME Target.example.com. | ttl=60, name=old.example.com.]"
    );
    assert_eq!(
        dname.data.canonicalized(),
        RecordData::Dname(name("target.example.com"))
    );

    // The target is written out in full.
    let mut rdata = Vec::new();
    dname.data.write_rdata(&mut rdata).unwrap();
    assert_eq!(rdata, b"\x06Target\x07example\x03com\x00");
    assert_eq!(
        without_raw(Message::from_wire(&parsed.to_wire().unwrap()).unwrap()),
        without_raw(parsed)
    );
}

#[test]
fn answers_to_follows_cname_chains() {
    let question = Question {
        name: name("www.example.com"),
        rtype: ResourceType::A,
        class: Class::IN,
    };
    let mut cname = record(
        "WWW.example.com",
        ResourceType::CNAME,
        RecordData::Cname(name("web.example.com")),
    );
    cname.ttl = 30;
    let cname_sig = record(
        "www.example.com",
        ResourceType::RRSIG,
        RecordData::Rrsig(Sig {
            type_covered: ResourceType::CNAME,
            ..Sig::default()
        }),
    );
    let a = record("web.example.com", ResourceType::A, RecordData::A(Ipv4Addr::LOCALHOST));
    let a_sig = record(
        "web.example.com",
        ResourceType::RRSIG,
        RecordData::Rrsig(Sig {
            type_covered: ResourceType::A,
            ..Sig::default()
        }),
    );
    let aaaa = record(
        "web.example.com",
        ResourceType::AAAA,
        RecordData::Aaaa(Ipv6Addr::LOCALHOST),
    );
    let unrelated = record("evil.example", ResourceType::A, RecordData::A(Ipv4Addr::LOCALHOST));
    let mut chaos = a.clone();
    chaos.class = Class::CH;

    let message = Message {
        answers: vec![
            cname.clone(),
            cname_sig.clone(),
            a.clone(),
            a_sig,
            aaaa,
            unrelated,
            chaos,
        ],
        ..Message::default()
    };
    let answers = message.answers_to(&question);
    assert_eq!(answers.len(), 4);
    assert_eq!(answers[0], cname);
    assert_eq!(answers[1], cname_sig);
    assert_eq!(answers[2].ttl, 30);
    assert_eq!(answers[2].data, a.data);
    assert_eq!(answers[3].rtype, ResourceType::RRSIG);
    assert_eq!(answers[3].ttl, 30);

    let any = Question {
        rtype: ResourceType::ANY,
        ..question
    };
    assert_eq!(message.answers_to(&any).len(), 5);
}

fn alias(owner: &str, target: &str, ttl: u32) -> ResourceRecord {
    let mut record = record(owner, ResourceType::CNAME, RecordData::Cname(name(target)));
    record.ttl = ttl;
    record
}

#[test]
fn answers_to_follows_cname_chains_in_any_order() {
    let question = Question {
        name: name("a.example"),
        rtype: ResourceType::A,
        class: Class::IN,
    };
    let target = record("c.example", ResourceType::A, RecordData::A(Ipv4Addr::LOCALHOST));
    let message = Message {
        answers: vec![
            target,
            alias("b.example", "c.example", 300),
            alias("a.example", "b.example", 5),
        ],
        ..Message::default()
    };
    let answers = message.answers_to(&question);
    assert_eq!(answers.len(), 3);
    // The alias at the queried name keeps its TTL; what it leads to lives no longer than it.
    assert_eq!(answers[0].name, name("a.example"));
    assert_eq!(answers[0].ttl, 5);
    assert_eq!(answers[1].name, name("b.example"));
    assert_eq!(answers[1].ttl, 5);
    assert_eq!(answers[2].data, RecordData::A(Ipv4Addr::LOCALHOST));
    assert_eq!(answers[2].ttl, 5);
}

#[test]
fn answers_to_limits_targets_by_the_shortest_ttl_of_an_alias_rrset() {
    let question = Question {
        name: name("a.example"),
        rtype: ResourceType::A,
        class: Class::IN,
    };
    let mut target = record("b.example", ResourceType::A, RecordData::A(Ipv4Addr::LOCALHOST));
    target.ttl = 600;
    let message = Message {
        answers: vec![
            alias("a.example", "b.example", 300),
            alias("a.example", "b.example", 5),
            target,
        ],
        ..Message::default()
    };
    let answers = message.answers_to(&question);
    assert_eq!(answers.last().unwrap().ttl, 5);
}

#[test]
fn answers_to_stops_at_a_cname_loop() {
    let question = Question {
        name: name("a.example"),
        rtype: ResourceType::A,
        class: Class::IN,
    };
    let message = Message {
        answers: vec![alias("a.example", "b.example", 60), alias("b.example", "a.example", 60)],
        ..Message::default()
    };
    assert_eq!(message.answers_to(&question).len(), 2);
}

#[test]
fn txt_with_several_strings() {
    let rdata = [3, b'a', b'b', b'c', 0, 2, b'd', b'e'];
    let message = Message::from_wire(&single_answer(ResourceType::TXT, &rdata, 8)).unwrap();
    let RecordData::Txt(txt) = &message.answers[0].data else {
        panic!("not a TXT")
    };
    assert_eq!(txt.strings, vec![b"abc".to_vec(), Vec::new(), b"de".to_vec()]);
    assert_eq!(txt.to_string(), "Text: 'abc' '' 'de'");

    let mut out = Vec::new();
    message.answers[0].data.write_rdata(&mut out).unwrap();
    assert_eq!(out, rdata);

    // A string running past RDLENGTH.
    assert!(Message::from_wire(&single_answer(ResourceType::TXT, &[3, b'a', b'b', b'c', 4, b'd'], 6)).is_err());
}

#[test]
fn ttl_with_the_high_bit_set_is_zero() {
    for (ttl, expected) in [(0x8000_0000u32, 0), (0xffff_ffff, 0), (0x7fff_ffff, 0x7fff_ffff)] {
        let mut message = header(0, 1, 0, 0);
        message.push(0);
        message.extend_from_slice(&ResourceType::A.0.to_be_bytes());
        message.extend_from_slice(&[0, 1]);
        message.extend_from_slice(&ttl.to_be_bytes());
        message.extend_from_slice(&[0, 4, 192, 0, 2, 1]);
        assert_eq!(Message::from_wire(&message).unwrap().answers[0].ttl, expected);
    }
}
