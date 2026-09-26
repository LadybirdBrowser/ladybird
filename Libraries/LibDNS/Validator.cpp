/*
 * Copyright (c) 2024, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Hex.h>
#include <AK/IterationDecision.h>
#include <AK/NeverDestroyed.h>
#include <AK/QuickSort.h>
#include <LibCrypto/Curves/EdwardsCurve.h>
#include <LibCrypto/Curves/SECPxxxr1.h>
#include <LibCrypto/Hash/SHA1.h>
#include <LibCrypto/Hash/SHA2.h>
#include <LibCrypto/PK/RSA.h>
#include <LibDNS/Validator.h>

namespace DNS::DNSSEC {

using namespace Messages;
using Messages::DNSSEC::Algorithm;
using Messages::DNSSEC::DigestType;
using Messages::DNSSEC::NSEC3HashAlgorithm;

StringView to_string(SecurityStatus status)
{
    switch (status) {
    case SecurityStatus::Secure:
        return "Secure"sv;
    case SecurityStatus::Insecure:
        return "Insecure"sv;
    case SecurityStatus::Bogus:
        return "Bogus"sv;
    }
    VERIFY_NOT_REACHED();
}

// https://data.iana.org/root-anchors/root-anchors.xml
Vector<Records::DS> const& Validator::root_trust_anchors()
{
    static NeverDestroyed<Vector<Records::DS>> anchors {
        Vector<Records::DS> {
            { .key_tag = 20326, .algorithm = Algorithm::RSASHA256, .digest_type = DigestType::SHA256, .digest = MUST(decode_hex("E06D44B80B8F1D39A95C0B0D7C65D08458E880409BBC683457104237C7F8EC8D"sv)) },
            { .key_tag = 38696, .algorithm = Algorithm::RSASHA256, .digest_type = DigestType::SHA256, .digest = MUST(decode_hex("683D2D0ACB8C9B712A1948B27F741219298D0A450D612C483AF444A4C0FB2B16"sv)) },
        }
    };
    return *anchors;
}

Validator::Validator(QueryFunction query)
    : m_query(move(query))
    , m_trust_anchors(root_trust_anchors())
{
}

void Validator::set_trust_anchors(Vector<Records::DS> anchors)
{
    m_trust_anchors = move(anchors);
    clear_caches();
}

void Validator::clear_caches()
{
    m_zone_keys.clear();
    m_delegations.clear();
}

static constexpr size_t max_cached_entries = 1024;
static constexpr size_t max_cname_chain_length = 16;
// RFC 9276, 3.2. Recommendation for Validating Resolvers.
// Validating resolvers MAY return an insecure response to their clients when processing NSEC3 records with
// iterations larger than 0.
static constexpr u16 max_nsec3_iterations = 100;

static bool is_supported_algorithm(Algorithm algorithm)
{
    switch (algorithm) {
    case Algorithm::RSASHA1:
    case Algorithm::RSASHA1NSEC3SHA1:
    case Algorithm::RSASHA256:
    case Algorithm::RSASHA512:
    case Algorithm::ECDSAP256SHA256:
    case Algorithm::ECDSAP384SHA384:
    case Algorithm::ED25519:
        return true;
    default:
        return false;
    }
}

static bool is_supported_digest_type(DigestType digest_type)
{
    switch (digest_type) {
    case DigestType::SHA1:
    case DigestType::SHA256:
    case DigestType::SHA384:
        return true;
    default:
        return false;
    }
}

// RFC 4034, 2.1.1. The Flags Field / 2.1.2. The Protocol Field.
// If bit 7 has value 0, then the DNSKEY record holds some other type of DNS public key and MUST NOT be used to
// verify RRSIGs that cover RRsets.
// The Protocol Field MUST have value 3, and the DNSKEY RR MUST be treated as invalid during signature verification
// if it is found to be some value other than 3.
static bool is_usable_zone_key(Records::DNSKEY const& key)
{
    return key.is_zone_key() && key.protocol == 3 && !key.is_revoked();
}

static ErrorOr<bool> verify_signature(Records::DNSKEY const& key, ReadonlyBytes signed_data, ReadonlyBytes signature)
{
    switch (key.algorithm) {
    case Algorithm::RSASHA1:
    case Algorithm::RSASHA1NSEC3SHA1:
    case Algorithm::RSASHA256:
    case Algorithm::RSASHA512: {
        auto hash_kind = key.algorithm == Algorithm::RSASHA256 ? Crypto::Hash::HashKind::SHA256
            : key.algorithm == Algorithm::RSASHA512            ? Crypto::Hash::HashKind::SHA512
                                                               : Crypto::Hash::HashKind::SHA1;
        auto components = TRY(key.rsa_public_key_components());
        Crypto::PK::RSAPublicKey public_key {
            Crypto::UnsignedBigInteger::import_data(components.modulus),
            Crypto::UnsignedBigInteger::import_data(components.exponent),
        };
        if (!TRY(public_key.is_valid()))
            return false;
        Crypto::PK::RSA_PKCS1_EMSA rsa { hash_kind, move(public_key) };
        return rsa.verify(signed_data, signature);
    }
    case Algorithm::ECDSAP256SHA256:
    case Algorithm::ECDSAP384SHA384: {
        // RFC 6605, 4. DNSKEY and RRSIG Resource Records for ECDSA.
        // ECDSA public keys consist of a single value, called "Q" in FIPS 186-3.  In DNSSEC keys, Q is a simple bit
        // string that represents the uncompressed form of a curve point, "x | y".
        // The two integers, both of which have length n bits, are concatenated to form "r | s".
        auto is_p256 = key.algorithm == Algorithm::ECDSAP256SHA256;
        size_t scalar_size = is_p256 ? 32 : 48;
        if (key.public_key.size() != scalar_size * 2 || signature.size() != scalar_size * 2)
            return false;
        Crypto::Curves::SECPxxxr1Point point {
            Crypto::UnsignedBigInteger::import_data(key.public_key.bytes().slice(0, scalar_size)),
            Crypto::UnsignedBigInteger::import_data(key.public_key.bytes().slice(scalar_size, scalar_size)),
            scalar_size,
        };
        Crypto::Curves::SECPxxxr1Signature ecdsa_signature {
            Crypto::UnsignedBigInteger::import_data(signature.slice(0, scalar_size)),
            Crypto::UnsignedBigInteger::import_data(signature.slice(scalar_size, scalar_size)),
            scalar_size,
        };
        if (is_p256) {
            auto digest = Crypto::Hash::SHA256::hash(signed_data);
            Crypto::Curves::SECP256r1 curve;
            return curve.verify(digest.bytes(), point, ecdsa_signature);
        }
        auto digest = Crypto::Hash::SHA384::hash(signed_data);
        Crypto::Curves::SECP384r1 curve;
        return curve.verify(digest.bytes(), point, ecdsa_signature);
    }
    case Algorithm::ED25519: {
        Crypto::Curves::Ed25519 ed25519;
        return ed25519.verify(key.public_key.bytes(), signature, signed_data);
    }
    default:
        return false;
    }
}

static ErrorOr<ByteBuffer> ds_digest(DomainName const& owner, Records::DNSKEY const& key, DigestType digest_type)
{
    // RFC 4034, 5.1.4. The Digest Field.
    // digest = digest_algorithm( DNSKEY owner name | DNSKEY RDATA);
    // DNSKEY RDATA = Flags | Protocol | Algorithm | Public Key.
    ByteBuffer input;
    TRY(owner.canonicalized().to_raw(input));
    TRY(key.to_raw(input));
    switch (digest_type) {
    case DigestType::SHA1:
        return ByteBuffer::copy(Crypto::Hash::SHA1::hash(input).bytes());
    case DigestType::SHA256:
        return ByteBuffer::copy(Crypto::Hash::SHA256::hash(input).bytes());
    case DigestType::SHA384:
        return ByteBuffer::copy(Crypto::Hash::SHA384::hash(input).bytes());
    default:
        return Error::from_string_literal("Unsupported DS digest type");
    }
}

static bool ds_matches_key(DomainName const& owner, Records::DS const& ds, Records::DNSKEY const& key)
{
    if (ds.key_tag != key.calculated_key_tag || ds.algorithm != key.algorithm)
        return false;
    auto digest = ds_digest(owner, key, ds.digest_type);
    return !digest.is_error() && digest.value().bytes() == ds.digest.bytes();
}

Vector<Validator::SignedRRSet> Validator::group_rrsets(Vector<ResourceRecord> const& section)
{
    Vector<SignedRRSet> rrsets;
    auto find = [&](DomainName const& owner, ResourceType type, Class class_) -> SignedRRSet& {
        for (auto& rrset : rrsets) {
            if (rrset.type == type && rrset.class_ == class_ && rrset.owner.equals_ignoring_case(owner))
                return rrset;
        }
        rrsets.append({ owner, type, class_, {}, {}, NumericLimits<u32>::max() });
        return rrsets.last();
    };

    for (auto const& record : section) {
        if (record.type == ResourceType::RRSIG) {
            auto const& rrsig = record.record.get<Records::RRSIG>();
            auto& rrset = find(record.name, rrsig.type_covered, record.class_);
            rrset.rrsigs.append(rrsig);
            rrset.rrsig_ttl = min(rrset.rrsig_ttl, record.ttl);
            continue;
        }
        find(record.name, record.type, record.class_).records.append(record);
    }

    // Sets that only have signatures and no data are of no use.
    rrsets.remove_all_matching([](auto const& rrset) { return rrset.records.is_empty(); });
    return rrsets;
}

static size_t label_count_excluding_wildcard(DomainName const& name)
{
    // RFC 4034, 3.1.3. The Labels Field.
    // The Labels field MUST be an unsigned integer between 1 and 255 inclusive, and does not count the null (root)
    // label nor a leading "*" label.
    return name.is_wildcard() ? name.labels.size() - 1 : name.labels.size();
}

ErrorOr<Validator::Verification> Validator::verify_rrset(SignedRRSet const& rrset, DomainName const& zone, Vector<Records::DNSKEY> const& keys, VerifiedRRSet& out)
{
    auto now = AK::UnixDateTime::now();

    for (auto const& rrsig : rrset.rrsigs) {
        // RFC 4035, 5.3.1. Checking the RRSIG RR Validity.
        // o  The RRSIG RR's Signer's Name field MUST be the name of the zone that contains the RRset.
        // o  The RRSIG RR's Type Covered field MUST equal the RRset's type.
        // o  The number of labels in the RRset owner name MUST be greater than or equal to the value in the RRSIG
        //    RR's Labels field.
        // o  The validator's notion of the current time MUST be less than or equal to the time listed in the RRSIG
        //    RR's Expiration field.
        // o  The validator's notion of the current time MUST be greater than or equal to the time listed in the
        //    RRSIG RR's Inception field.
        if (!rrsig.signers_name.equals_ignoring_case(zone) || rrsig.type_covered != rrset.type)
            continue;
        auto owner_labels = label_count_excluding_wildcard(rrset.owner);
        if (rrsig.label_count > owner_labels)
            continue;
        if (now > rrsig.expiration || now < rrsig.inception)
            continue;
        if (!is_supported_algorithm(rrsig.algorithm))
            continue;

        // RFC 4035, 5.3.2. Reconstructing the Signed Data.
        // if rrsig_labels < fqdn_labels, name = "*." | the rightmost rrsig_label labels of the fqdn
        auto canonical_owner = rrset.owner.canonicalized();
        Optional<size_t> wildcard_label_count;
        if (rrsig.label_count < owner_labels) {
            canonical_owner = rrset.owner.suffix(rrsig.label_count).with_label_prepended("*"sv).canonicalized();
            wildcard_label_count = rrsig.label_count;
        }

        // RR(i) = name | type | class | OrigTTL | RDATA length | RDATA
        // The set of all RR(i) is sorted into canonical order.
        ByteBuffer owner_wire;
        TRY(canonical_owner.to_raw(owner_wire));
        auto rdata_offset = owner_wire.size() + 2 + 2 + 4 + 2;
        Vector<ByteBuffer> encoded_records;
        for (auto const& record : rrset.records) {
            ByteBuffer encoded;
            TRY(record.to_canonical_raw(encoded, canonical_owner, rrsig.original_ttl));
            if (encoded_records.contains_slow(encoded))
                continue;
            encoded_records.append(move(encoded));
        }
        // RFC 4034, 6.3. Canonical RR Ordering within an RRset.
        // ... treating the RDATA portion of the canonical form of each RR as a left-justified unsigned octet sequence
        // in which the absence of an octet sorts before a zero octet.
        quick_sort(encoded_records, [&](auto const& a, auto const& b) {
            auto a_rdata = a.bytes().slice(rdata_offset);
            auto b_rdata = b.bytes().slice(rdata_offset);
            auto common = min(a_rdata.size(), b_rdata.size());
            auto order = memcmp(a_rdata.data(), b_rdata.data(), common);
            if (order != 0)
                return order < 0;
            return a_rdata.size() < b_rdata.size();
        });

        // signed_data = RRSIG_RDATA | RR(1) | RR(2)...
        ByteBuffer signed_data;
        auto canonical_rrsig = rrsig;
        canonical_rrsig.signers_name = rrsig.signers_name.canonicalized();
        TRY(canonical_rrsig.to_raw_excluding_signature(signed_data));
        for (auto const& encoded : encoded_records)
            TRY(signed_data.try_append(encoded));

        // o  The RRSIG RR's Signer's Name, Algorithm, and Key Tag fields MUST match the owner name, algorithm, and
        //    key tag for some DNSKEY RR in the zone's apex DNSKEY RRset.
        // It is possible for more than one DNSKEY RR to match the conditions above. ... it MUST try each matching
        // DNSKEY RR until either the signature is validated or the validator has run out of matching public keys
        // to try.
        for (auto const& key : keys) {
            if (key.algorithm != rrsig.algorithm || key.calculated_key_tag != rrsig.key_tag || !is_usable_zone_key(key))
                continue;
            auto verified = verify_signature(key, signed_data, rrsig.signature);
            if (verified.is_error() || !verified.value()) {
                dbgln_if(DNS_DEBUG, "DNSSEC: Signature over {} {} by key {} did not verify", rrset.owner.to_string(), Messages::to_string(rrset.type), key.calculated_key_tag);
                continue;
            }

            // RFC 4035, 5.3.3. Checking the Signature.
            // If the resolver accepts the RRset as authentic, the validator MUST set the TTL of the RRSIG RR and each
            // RR in the authenticated RRset to a value no greater than the minimum of:
            // o  the RRset's TTL as received in the response;
            // o  the RRSIG RR's TTL as received in the response;
            // o  the value in the RRSIG RR's Original TTL field; and
            // o  the difference of the RRSIG RR's Signature Expiration time and the current time.
            u32 ttl = min(rrsig.original_ttl, rrset.rrsig_ttl);
            for (auto const& record : rrset.records)
                ttl = min(ttl, record.ttl);
            auto until_expiration = (rrsig.expiration - now).to_seconds();
            ttl = min(ttl, static_cast<u32>(min<i64>(until_expiration, NumericLimits<u32>::max())));

            out = VerifiedRRSet { rrset, zone, wildcard_label_count, ttl };
            for (auto& record : out.rrset.records)
                record.ttl = min(record.ttl, ttl);
            return Verification::Verified;
        }
    }

    return Verification::Failed;
}

ErrorOr<Vector<Validator::VerifiedRRSet>> Validator::verify_denial_records(Vector<SignedRRSet> const& authority, DomainName const& zone, Vector<Records::DNSKEY> const& keys)
{
    Vector<VerifiedRRSet> verified;
    for (auto const& rrset : authority) {
        if (rrset.type != ResourceType::NSEC && rrset.type != ResourceType::NSEC3)
            continue;
        if (!zone.is_ancestor_or_equal_of(rrset.owner))
            continue;
        VerifiedRRSet out;
        if (TRY(verify_rrset(rrset, zone, keys, out)) == Verification::Verified)
            verified.append(move(out));
    }
    return verified;
}

// RFC 4648, 7. Base 32 Encoding with Extended Hex Alphabet.
static ErrorOr<ByteBuffer> base32hex_decode(StringView input)
{
    ByteBuffer output;
    u32 bits = 0;
    size_t bit_count = 0;
    for (auto ch : input.bytes()) {
        u8 value;
        if (ch >= '0' && ch <= '9')
            value = ch - '0';
        else if (ch >= 'A' && ch <= 'V')
            value = ch - 'A' + 10;
        else if (ch >= 'a' && ch <= 'v')
            value = ch - 'a' + 10;
        else
            return Error::from_string_literal("Invalid base32hex character");
        bits = bits << 5 | value;
        bit_count += 5;
        if (bit_count >= 8) {
            TRY(output.try_append(static_cast<u8>(bits >> (bit_count - 8))));
            bit_count -= 8;
        }
    }
    return output;
}

// RFC 5155, 5. Calculation of the Hash.
// IH(salt, x, 0) = H(x || salt), and
// IH(salt, x, k) = H(IH(salt, x, k-1) || salt), if k > 0
static ErrorOr<ByteBuffer> nsec3_hash(DomainName const& name, Records::NSEC3 const& params)
{
    if (params.hash_algorithm != NSEC3HashAlgorithm::SHA1)
        return Error::from_string_literal("Unsupported NSEC3 hash algorithm");

    ByteBuffer input;
    TRY(name.canonicalized().to_raw(input));
    TRY(input.try_append(params.salt));
    auto digest = Crypto::Hash::SHA1::hash(input);
    for (size_t i = 0; i < params.iterations; ++i) {
        ByteBuffer next;
        TRY(next.try_append(digest.bytes()));
        TRY(next.try_append(params.salt));
        digest = Crypto::Hash::SHA1::hash(next);
    }
    return ByteBuffer::copy(digest.bytes());
}

static int compare_bytes(ReadonlyBytes a, ReadonlyBytes b)
{
    auto common = min(a.size(), b.size());
    auto order = memcmp(a.data(), b.data(), common);
    if (order != 0)
        return order;
    if (a.size() == b.size())
        return 0;
    return a.size() < b.size() ? -1 : 1;
}

namespace {

struct NSECRecord {
    DomainName const& owner;
    Records::NSEC const& nsec;
    DomainName const& zone;

    bool matches(DomainName const& name) const { return owner.equals_ignoring_case(name); }

    // RFC 4035, 5.4. Authenticated Denial of Existence.
    // If the requested RR name would appear after an authenticated NSEC RR's owner name and before the name listed
    // in that NSEC RR's Next Domain Name field according to the canonical DNS name order defined in [RFC4034], then
    // no RRsets with the requested name exist in the zone.
    bool covers(DomainName const& name) const
    {
        if (!zone.is_ancestor_or_equal_of(name))
            return false;
        auto after_owner = DomainName::canonical_compare(owner, name) < 0;
        auto before_next = DomainName::canonical_compare(name, nsec.next_domain_name) < 0;
        // The last NSEC in the zone points back at the apex, which sorts before its owner.
        auto is_last = DomainName::canonical_compare(nsec.next_domain_name, owner) <= 0;
        return after_owner && (before_next || is_last);
    }
};

struct NSEC3Record {
    ByteBuffer owner_hash;
    Records::NSEC3 const& nsec3;
    DomainName const& zone;

    ErrorOr<bool> matches(DomainName const& name) const
    {
        auto hash = TRY(nsec3_hash(name, nsec3));
        return hash.bytes() == owner_hash.bytes();
    }

    ErrorOr<bool> covers(DomainName const& name) const
    {
        if (!zone.is_ancestor_or_equal_of(name))
            return false;
        auto hash = TRY(nsec3_hash(name, nsec3));
        auto after_owner = compare_bytes(owner_hash, hash) < 0;
        auto before_next = compare_bytes(hash, nsec3.next_hashed_owner_name) < 0;
        auto is_last = compare_bytes(nsec3.next_hashed_owner_name, owner_hash) <= 0;
        return after_owner && (before_next || is_last);
    }
};

}

template<typename Callback>
static void for_each_nsec(Vector<Validator::VerifiedRRSet> const& rrsets, Callback callback)
{
    for (auto const& verified : rrsets) {
        if (verified.rrset.type != ResourceType::NSEC)
            continue;
        for (auto const& record : verified.rrset.records) {
            if (callback(NSECRecord { record.name, record.record.get<Records::NSEC>(), verified.zone }) == IterationDecision::Break)
                return;
        }
    }
}

template<typename Callback>
static void for_each_nsec3(Vector<Validator::VerifiedRRSet> const& rrsets, Callback callback)
{
    for (auto const& verified : rrsets) {
        if (verified.rrset.type != ResourceType::NSEC3)
            continue;
        for (auto const& record : verified.rrset.records) {
            auto const& nsec3 = record.record.get<Records::NSEC3>();
            // RFC 5155, 8.1 / 8.2: unknown hash types, unknown flags and excessive iterations are ignored.
            if (nsec3.hash_algorithm != NSEC3HashAlgorithm::SHA1 || (nsec3.flags & ~Records::NSEC3::FlagOptOut) != 0 || nsec3.iterations > max_nsec3_iterations)
                continue;
            if (record.name.labels.is_empty() || !record.name.parent().equals_ignoring_case(verified.zone))
                continue;
            auto owner_hash = base32hex_decode(record.name.labels.first());
            if (owner_hash.is_error() || owner_hash.value().size() != nsec3.next_hashed_owner_name.size())
                continue;
            if (callback(NSEC3Record { owner_hash.release_value(), nsec3, verified.zone }) == IterationDecision::Break)
                return;
        }
    }
}

// RFC 5155, 8.3. Closest Encloser Proof.
// In order to verify a closest encloser proof, the validator MUST find the longest name, X, such that
// o  X is an ancestor of QNAME that is matched by an NSEC3 RR present in the response.  This is a candidate for the
//    closest encloser, and
// o  The name one label longer than X (but still an ancestor of -- or equal to -- QNAME) is covered by an NSEC3 RR
//    present in the response.
struct ClosestEncloserProof {
    DomainName closest_encloser;
    bool next_closer_opt_out { false };
};
static Optional<ClosestEncloserProof> nsec3_closest_encloser_proof(Vector<Validator::VerifiedRRSet> const& rrsets, DomainName const& name)
{
    bool previous_covered = false;
    bool previous_opt_out = false;
    for (size_t labels = name.labels.size(); labels > 0; --labels) {
        auto sname = name.suffix(labels);
        Optional<ClosestEncloserProof> proof;
        bool bogus = false;
        bool covered = false;
        bool opt_out = false;
        for_each_nsec3(rrsets, [&](NSEC3Record const& record) {
            if (auto matches = record.matches(sname); !matches.is_error() && matches.value()) {
                // Once the closest encloser has been discovered, the validator MUST check that the NSEC3 RR that has
                // the closest encloser as the original owner name is from the proper zone.  The DNAME type bit must
                // not be set and the NS type bit may only be set if the SOA type bit is set.
                if (!previous_covered || record.nsec3.has_type(ResourceType::DNAME) || (record.nsec3.has_type(ResourceType::NS) && !record.nsec3.has_type(ResourceType::SOA))) {
                    bogus = true;
                    return IterationDecision::Break;
                }
                proof = ClosestEncloserProof { sname, previous_opt_out };
                return IterationDecision::Break;
            }
            if (auto covers = record.covers(sname); !covers.is_error() && covers.value()) {
                covered = true;
                opt_out = record.nsec3.is_opt_out();
            }
            return IterationDecision::Continue;
        });
        if (bogus)
            return {};
        if (proof.has_value())
            return proof;
        previous_covered = covered;
        previous_opt_out = opt_out;
    }
    return {};
}

static Optional<DomainName> nsec_closest_encloser(Vector<Validator::VerifiedRRSet> const& rrsets, DomainName const& name)
{
    // The closest encloser is the longest ancestor of the name that exists. Every ancestor of an NSEC's owner and
    // next name exists, so the longest suffix the name shares with either side of a covering NSEC is it.
    Optional<size_t> best;
    for_each_nsec(rrsets, [&](NSECRecord const& record) {
        if (!record.covers(name))
            return IterationDecision::Continue;
        auto shared = max(name.common_suffix_length(record.owner), name.common_suffix_length(record.nsec.next_domain_name));
        if (!best.has_value() || shared > *best)
            best = shared;
        return IterationDecision::Continue;
    });
    if (!best.has_value())
        return {};
    return name.suffix(*best);
}

bool Validator::DenialProofs::proves_name_error(DomainName const& name) const
{
    // RFC 4035, 5.4. Authenticated Denial of Existence.
    // To prove the non-existence of an RRset, the resolver must be able to verify both that the queried RRset does
    // not exist and that no relevant wildcard RRset exists.
    if (auto closest_encloser = nsec_closest_encloser(rrsets, name); closest_encloser.has_value()) {
        if (proves_wildcard_absent(*closest_encloser))
            return true;
    }

    // RFC 5155, 8.4. Validating Name Error Responses.
    // A validator MUST verify that there is a closest encloser proof for QNAME present in the response and that
    // there is an NSEC3 RR that covers the wildcard at the closest encloser.
    if (auto proof = nsec3_closest_encloser_proof(rrsets, name); proof.has_value())
        return proves_wildcard_absent(proof->closest_encloser);

    return false;
}

bool Validator::DenialProofs::proves_wildcard_absent(DomainName const& closest_encloser) const
{
    auto wildcard = closest_encloser.with_label_prepended("*"sv);
    bool proven = false;
    for_each_nsec(rrsets, [&](NSECRecord const& record) {
        if (!record.covers(wildcard))
            return IterationDecision::Continue;
        proven = true;
        return IterationDecision::Break;
    });
    if (proven)
        return true;
    for_each_nsec3(rrsets, [&](NSEC3Record const& record) {
        if (auto covers = record.covers(wildcard); covers.is_error() || !covers.value())
            return IterationDecision::Continue;
        proven = true;
        return IterationDecision::Break;
    });
    return proven;
}

bool Validator::DenialProofs::proves_next_closer_absent(DomainName const& name, size_t closest_encloser_label_count) const
{
    // RFC 4035, 5.3.4 / RFC 5155, 8.8. Validating Wildcard Answer Responses.
    // Validators MUST verify that there is an NSEC3 RR that covers the "next closer" name to QNAME present in the
    // response.  This proves that QNAME itself did not exist and that the correct wildcard was used to generate the
    // response.
    if (closest_encloser_label_count >= name.labels.size())
        return false;
    auto next_closer = name.suffix(closest_encloser_label_count + 1);
    bool proven = false;
    for_each_nsec(rrsets, [&](NSECRecord const& record) {
        if (!record.covers(next_closer))
            return IterationDecision::Continue;
        proven = true;
        return IterationDecision::Break;
    });
    if (proven)
        return true;
    for_each_nsec3(rrsets, [&](NSEC3Record const& record) {
        if (auto covers = record.covers(next_closer); covers.is_error() || !covers.value())
            return IterationDecision::Continue;
        proven = true;
        return IterationDecision::Break;
    });
    return proven;
}

bool Validator::DenialProofs::proves_no_data(DomainName const& name, ResourceType type) const
{
    // RFC 4035, 5.4. Authenticated Denial of Existence.
    // If the requested RR name matches the owner name of an authenticated NSEC RR, then the NSEC RR's type bit map
    // field lists all RR types present at that owner name, and a resolver can prove that the requested RR type does
    // not exist by checking for the RR type in the bit map.
    // RFC 4035, 5.2: A security-aware resolver MUST use the parent NSEC RR when attempting to prove that a DS RRset
    // does not exist.
    auto bitmap_denies = [&](auto const& record) {
        if (record.has_type(type) || record.has_type(ResourceType::CNAME))
            return false;
        if (type == ResourceType::DS && record.has_type(ResourceType::SOA))
            return false;
        return true;
    };

    bool proven = false;
    for_each_nsec(rrsets, [&](NSECRecord const& record) {
        if (!record.matches(name) || !bitmap_denies(record.nsec))
            return IterationDecision::Continue;
        proven = true;
        return IterationDecision::Break;
    });
    if (proven)
        return true;

    // RFC 5155, 8.5. Validating No Data Responses, QTYPE is not DS.
    // The validator MUST verify that an NSEC3 RR that matches QNAME is present and that both the QTYPE and the CNAME
    // type are not set in its Type Bit Maps field.
    for_each_nsec3(rrsets, [&](NSEC3Record const& record) {
        if (auto matches = record.matches(name); matches.is_error() || !matches.value() || !bitmap_denies(record.nsec3))
            return IterationDecision::Continue;
        proven = true;
        return IterationDecision::Break;
    });
    if (proven)
        return true;

    // Wildcard No Data: the name does not exist, but a wildcard does, and it lacks the type.
    // RFC 5155, 8.7. Validating Wildcard No Data Responses.
    Optional<DomainName> closest_encloser = nsec_closest_encloser(rrsets, name);
    if (!closest_encloser.has_value()) {
        if (auto proof = nsec3_closest_encloser_proof(rrsets, name); proof.has_value())
            closest_encloser = proof->closest_encloser;
    }
    if (!closest_encloser.has_value())
        return false;
    auto wildcard = closest_encloser->with_label_prepended("*"sv);
    for_each_nsec(rrsets, [&](NSECRecord const& record) {
        if (!record.matches(wildcard) || !bitmap_denies(record.nsec))
            return IterationDecision::Continue;
        proven = true;
        return IterationDecision::Break;
    });
    if (proven)
        return true;
    for_each_nsec3(rrsets, [&](NSEC3Record const& record) {
        if (auto matches = record.matches(wildcard); matches.is_error() || !matches.value() || !bitmap_denies(record.nsec3))
            return IterationDecision::Continue;
        proven = true;
        return IterationDecision::Break;
    });
    return proven;
}

bool Validator::DenialProofs::proves_insecure_delegation(DomainName const& name) const
{
    // RFC 4035, 5.2. Authenticating Referrals.
    // If the validator authenticates an NSEC RRset that proves that no DS RRset is present for this zone, then there
    // is no authentication path leading from the parent to the child.
    // RFC 5155, 8.9. Validating Referrals to Unsigned Subzones.
    // If there is an NSEC3 RR present in the response that matches the delegation name, then the validator MUST
    // ensure that the NS bit is set and that the DS bit is not set in the Type Bit Maps field of the NSEC3 RR.  The
    // validator MUST also ensure that the NSEC3 RR is from the correct (i.e., parent) zone.  This is done by
    // ensuring that the SOA bit is not set in the Type Bit Maps field of this NSEC3 RR.
    auto is_unsigned_delegation = [](auto const& record) {
        return record.has_type(ResourceType::NS) && !record.has_type(ResourceType::DS) && !record.has_type(ResourceType::SOA);
    };

    bool proven = false;
    for_each_nsec(rrsets, [&](NSECRecord const& record) {
        if (!record.matches(name) || !is_unsigned_delegation(record.nsec))
            return IterationDecision::Continue;
        proven = true;
        return IterationDecision::Break;
    });
    if (proven)
        return true;
    for_each_nsec3(rrsets, [&](NSEC3Record const& record) {
        if (auto matches = record.matches(name); matches.is_error() || !matches.value() || !is_unsigned_delegation(record.nsec3))
            return IterationDecision::Continue;
        proven = true;
        return IterationDecision::Break;
    });
    if (proven)
        return true;

    // If there is no NSEC3 RR present that matches the delegation name, then the validator MUST verify a closest
    // provable encloser proof for the delegation name.  The validator MUST verify that the Opt-Out bit is set in the
    // NSEC3 RR that covers the "next closer" name to the delegation name.
    if (auto proof = nsec3_closest_encloser_proof(rrsets, name); proof.has_value())
        return proof->next_closer_opt_out;
    return false;
}

bool Validator::DenialProofs::proves_name_exists_without_delegation(DomainName const& name) const
{
    bool proven = false;
    for_each_nsec(rrsets, [&](NSECRecord const& record) {
        if (!record.matches(name) || record.nsec.has_type(ResourceType::NS) || record.nsec.has_type(ResourceType::DS))
            return IterationDecision::Continue;
        proven = true;
        return IterationDecision::Break;
    });
    if (proven)
        return true;
    for_each_nsec3(rrsets, [&](NSEC3Record const& record) {
        if (auto matches = record.matches(name); matches.is_error() || !matches.value() || record.nsec3.has_type(ResourceType::NS) || record.nsec3.has_type(ResourceType::DS))
            return IterationDecision::Continue;
        proven = true;
        return IterationDecision::Break;
    });
    return proven;
}

static bool has_excessive_nsec3_iterations(Vector<Validator::VerifiedRRSet> const& rrsets)
{
    for (auto const& verified : rrsets) {
        if (verified.rrset.type != ResourceType::NSEC3)
            continue;
        for (auto const& record : verified.rrset.records) {
            if (record.record.get<Records::NSEC3>().iterations > max_nsec3_iterations)
                return true;
        }
    }
    return false;
}

static bool is_positive_or_name_error(Message const& message)
{
    auto rcode = message.header.options.response_code();
    return rcode == Options::ResponseCode::NoError || rcode == Options::ResponseCode::NameError;
}

ErrorOr<Optional<Validator::ZoneKeys>> Validator::zone_keys(DomainName const& zone, Vector<Records::DS> const& ds_set)
{
    auto now = AK::UnixDateTime::now();
    auto cache_key = zone.to_canonical_string();
    if (auto cached = m_zone_keys.get(cache_key); cached.has_value()) {
        if (cached->expires > now)
            return Optional<ZoneKeys> { *cached };
        m_zone_keys.remove(cache_key);
    }

    // RFC 4035, 5.2. Authenticating Referrals.
    // If the validator does not support any of the algorithms listed in an authenticated DS RRset, then the resolver
    // has no supported authentication path leading from the parent to the child.  The resolver should treat this
    // case as it would the case of an authenticated NSEC RRset proving that no DS RRset exists, as described above.
    Vector<Records::DS const&> usable_ds;
    for (auto const& ds : ds_set) {
        if (is_supported_algorithm(ds.algorithm) && is_supported_digest_type(ds.digest_type))
            usable_ds.append(ds);
    }
    if (usable_ds.is_empty())
        return Optional<ZoneKeys> {};

    auto response = TRY(m_query(zone, ResourceType::DNSKEY)->await());
    if (!is_positive_or_name_error(response))
        return Error::from_string_literal("DNSKEY query failed");

    auto rrsets = group_rrsets(response.answers);
    SignedRRSet const* dnskey_rrset = nullptr;
    for (auto const& rrset : rrsets) {
        if (rrset.type == ResourceType::DNSKEY && rrset.owner.equals_ignoring_case(zone)) {
            dnskey_rrset = &rrset;
            break;
        }
    }
    if (!dnskey_rrset)
        return Error::from_string_literal("Zone has no DNSKEY RRset");

    // Given a DS RR for a delegation, the child zone's apex DNSKEY RRset can be authenticated if all of the following
    // hold:
    // o  The Algorithm and Key Tag in the DS RR match the Algorithm field and the key tag of a DNSKEY RR in the child
    //    zone's apex DNSKEY RRset, and, when the DNSKEY RR's owner name and RDATA are hashed using the digest
    //    algorithm specified in the DS RR's Digest Type field, the resulting digest value matches the Digest field
    //    of the DS RR.
    // o  The matching DNSKEY RR in the child zone has the Zone Flag bit set, the corresponding private key has
    //    signed the child zone's apex DNSKEY RRset, and the resulting RRSIG RR authenticates the child zone's apex
    //    DNSKEY RRset.
    for (auto const& record : dnskey_rrset->records) {
        auto const& key = record.record.get<Records::DNSKEY>();
        if (!is_usable_zone_key(key))
            continue;
        bool matches_ds = false;
        for (auto const& ds : usable_ds) {
            if (ds_matches_key(zone, ds, key)) {
                matches_ds = true;
                break;
            }
        }
        if (!matches_ds)
            continue;

        VerifiedRRSet verified;
        if (TRY(verify_rrset(*dnskey_rrset, zone, { key }, verified)) != Verification::Verified)
            continue;

        ZoneKeys keys;
        for (auto const& verified_record : verified.rrset.records) {
            auto const& zone_key = verified_record.record.get<Records::DNSKEY>();
            if (is_usable_zone_key(zone_key))
                keys.keys.append(zone_key);
        }
        keys.expires = now + AK::Duration::from_seconds(verified.ttl);
        if (m_zone_keys.size() >= max_cached_entries)
            m_zone_keys.clear();
        m_zone_keys.set(cache_key, keys);
        return Optional<ZoneKeys> { move(keys) };
    }

    return Error::from_string_literal("No DNSKEY matches an authenticated DS record");
}

ErrorOr<Validator::Delegation> Validator::delegation(DomainName const& name, Chain const& parent)
{
    auto now = AK::UnixDateTime::now();
    auto cache_key = name.to_canonical_string();
    if (auto cached = m_delegations.get(cache_key); cached.has_value()) {
        if (cached->expires > now)
            return *cached;
        m_delegations.remove(cache_key);
    }

    auto remember = [&](Delegation delegation) {
        if (m_delegations.size() >= max_cached_entries)
            m_delegations.clear();
        m_delegations.set(cache_key, delegation);
        return delegation;
    };

    auto response = TRY(m_query(name, ResourceType::DS)->await());
    if (!is_positive_or_name_error(response))
        return Error::from_string_literal("DS query failed");

    auto answers = group_rrsets(response.answers);
    for (auto const& rrset : answers) {
        if (!rrset.owner.equals_ignoring_case(name))
            continue;
        // RFC 2181, 10.1: a name that is an alias cannot have any other data, so it is not a zone cut either.
        if (rrset.type == ResourceType::CNAME) {
            VerifiedRRSet verified;
            if (TRY(verify_rrset(rrset, parent.zone, parent.keys, verified)) != Verification::Verified)
                return Error::from_string_literal("CNAME RRset did not validate");
            return remember({ Delegation::Kind::NotACut, {}, now + AK::Duration::from_seconds(verified.ttl) });
        }
        if (rrset.type != ResourceType::DS)
            continue;
        // RFC 4035, 5.2: The DS RR has been authenticated using some DNSKEY RR in the parent's apex DNSKEY RRset.
        VerifiedRRSet verified;
        if (TRY(verify_rrset(rrset, parent.zone, parent.keys, verified)) != Verification::Verified)
            return Error::from_string_literal("DS RRset did not validate");
        Delegation delegation { Delegation::Kind::Secure, {}, now + AK::Duration::from_seconds(verified.ttl) };
        for (auto const& record : verified.rrset.records)
            delegation.ds.append(record.record.get<Records::DS>());
        return remember(move(delegation));
    }

    // No DS: the parent has to prove that, with NSEC or NSEC3 records signed by its own keys.
    auto authority = group_rrsets(response.authorities);
    auto verified = TRY(verify_denial_records(authority, parent.zone, parent.keys));
    if (has_excessive_nsec3_iterations(verified))
        return remember({ Delegation::Kind::Insecure, {}, now + AK::Duration::from_seconds(60) });

    u32 ttl = NumericLimits<u32>::max();
    for (auto const& rrset : verified)
        ttl = min(ttl, rrset.ttl);
    auto expires = now + AK::Duration::from_seconds(verified.is_empty() ? 0 : ttl);

    DenialProofs proofs { verified };
    if (proofs.proves_insecure_delegation(name))
        return remember({ Delegation::Kind::Insecure, {}, expires });
    if (proofs.proves_name_exists_without_delegation(name))
        return remember({ Delegation::Kind::NotACut, {}, expires });
    if (response.header.options.response_code() == Options::ResponseCode::NameError && proofs.proves_name_error(name))
        return remember({ Delegation::Kind::NonExistent, {}, expires });
    // A No Data answer for DS without a matching NSEC means the name is an empty non-terminal or is covered by
    // a wildcard; either way it is not a zone cut.
    if (proofs.proves_no_data(name, ResourceType::DS))
        return remember({ Delegation::Kind::NotACut, {}, expires });

    return Error::from_string_literal("Absence of DS record is not proven");
}

ErrorOr<Validator::Chain> Validator::chain_for(DomainName const& name)
{
    DomainName root;
    auto root_keys = TRY(zone_keys(root, m_trust_anchors));
    if (!root_keys.has_value())
        return Chain { SecurityStatus::Insecure, root, {}, "No usable trust anchor"sv };

    Chain chain { SecurityStatus::Secure, root, move(root_keys->keys), {} };
    for (size_t labels = 1; labels <= name.labels.size(); ++labels) {
        auto candidate = name.suffix(labels);
        auto delegation = TRY(this->delegation(candidate, chain));
        switch (delegation.kind) {
        case Delegation::Kind::Secure: {
            auto keys = TRY(zone_keys(candidate, delegation.ds));
            if (!keys.has_value())
                return Chain { SecurityStatus::Insecure, candidate, {}, "No supported DS algorithm"sv };
            chain = Chain { SecurityStatus::Secure, candidate, move(keys->keys), {} };
            break;
        }
        case Delegation::Kind::Insecure:
            return Chain { SecurityStatus::Insecure, candidate, {}, "Unsigned delegation"sv };
        case Delegation::Kind::NotACut:
            break;
        case Delegation::Kind::NonExistent:
            return chain;
        }
    }
    return chain;
}

ErrorOr<ValidationResult> Validator::validate(Message const& response, Question const& question)
{
    if (!is_positive_or_name_error(response))
        return Error::from_string_literal("DNS query failed");

    auto insecure = [&](StringView reason) {
        return ValidationResult { SecurityStatus::Insecure, response.answers_to(question), reason };
    };
    auto bogus = [&](StringView reason) {
        dbgln_if(DNS_DEBUG, "DNSSEC: {} {} is bogus: {}", question.name.to_string(), Messages::to_string(question.type), reason);
        return ValidationResult { SecurityStatus::Bogus, {}, reason };
    };

    auto answers = group_rrsets(response.answers);
    auto authority = group_rrsets(response.authorities);

    ValidationResult result { SecurityStatus::Secure, {}, {} };
    auto current = question.name;
    for (size_t hops = 0; hops <= max_cname_chain_length; ++hops) {
        auto chain = TRY(chain_for(current));
        if (chain.status == SecurityStatus::Insecure)
            return insecure(chain.reason);
        if (chain.status == SecurityStatus::Bogus)
            return bogus(chain.reason);

        // RFC 4035, 5.2: a DS RRset is authoritative data of the parent zone.
        auto zone_for_type = [&](ResourceType type) -> ErrorOr<Chain> {
            if (type != ResourceType::DS || current.labels.is_empty())
                return chain;
            return chain_for(current.parent());
        };

        SignedRRSet const* answer = nullptr;
        SignedRRSet const* cname = nullptr;
        for (auto const& rrset : answers) {
            if (rrset.class_ != question.class_ || !rrset.owner.equals_ignoring_case(current))
                continue;
            if (rrset.type == question.type)
                answer = &rrset;
            else if (rrset.type == ResourceType::CNAME)
                cname = &rrset;
        }

        auto denial_records = TRY(verify_denial_records(authority, chain.zone, chain.keys));
        if (has_excessive_nsec3_iterations(denial_records))
            return insecure("NSEC3 iteration count is too high"sv);
        DenialProofs proofs { denial_records };

        auto accept = [&](SignedRRSet const& rrset) -> ErrorOr<Optional<ValidationResult>> {
            auto zone = TRY(zone_for_type(rrset.type));
            if (zone.status == SecurityStatus::Insecure)
                return insecure(zone.reason);
            if (zone.status == SecurityStatus::Bogus)
                return bogus(zone.reason);
            VerifiedRRSet verified;
            if (TRY(verify_rrset(rrset, zone.zone, zone.keys, verified)) != Verification::Verified)
                return bogus("RRset signature did not validate"sv);
            // RFC 4035, 5.3.4. Authenticating a Wildcard Expanded RRset Positive Response.
            // it must take additional steps to verify the non-existence of an exact match or closer wildcard match
            // for the query.
            if (verified.wildcard_label_count.has_value() && !proofs.proves_next_closer_absent(current, *verified.wildcard_label_count))
                return bogus("Wildcard expansion is not proven"sv);
            result.records.extend(verified.rrset.records);
            return Optional<ValidationResult> {};
        };

        if (answer) {
            if (auto early = TRY(accept(*answer)); early.has_value())
                return early.release_value();
            return result;
        }

        if (cname && question.type != ResourceType::CNAME) {
            if (auto early = TRY(accept(*cname)); early.has_value())
                return early.release_value();
            current = cname->records.first().record.get<Records::CNAME>().names;
            continue;
        }

        // Nothing for this name: the zone has to prove that.
        if (response.header.options.response_code() == Options::ResponseCode::NameError) {
            if (proofs.proves_name_error(current))
                return result;
            return bogus("Name error is not proven"sv);
        }
        if (proofs.proves_no_data(current, question.type))
            return result;
        return bogus("Absence of the requested type is not proven"sv);
    }

    return bogus("CNAME chain is too long"sv);
}

}
