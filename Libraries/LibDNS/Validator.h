/*
 * Copyright (c) 2024, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/RefCounted.h>
#include <AK/Time.h>
#include <LibCore/Promise.h>
#include <LibDNS/Message.h>

namespace DNS::DNSSEC {

// RFC 4035, 4.3. Determining Security Status of Data.
enum class SecurityStatus : u8 {
    Secure,
    Insecure,
    Bogus,
};
StringView to_string(SecurityStatus);

struct ValidationResult {
    SecurityStatus status { SecurityStatus::Bogus };
    // The records answering the question, following CNAMEs. Authenticated when the status is Secure.
    Vector<Messages::ResourceRecord> records;
    StringView reason;
};

class DNS_API Validator {
public:
    using QueryFunction = Function<NonnullRefPtr<Core::Promise<Messages::Message>>(Messages::DomainName const&, Messages::ResourceType)>;
    using ValidationPromise = Core::Promise<ValidationResult>;

    explicit Validator(QueryFunction);

    static Vector<Messages::Records::DS> const& root_trust_anchors();
    void set_trust_anchors(Vector<Messages::Records::DS>);

    // Authenticates the answer to `question` in `response`, fetching DS and DNSKEY RRsets through the query
    // function as needed.
    NonnullRefPtr<ValidationPromise> validate(Messages::Message response, Messages::Question question);

    void clear_caches();

    struct SignedRRSet {
        Messages::DomainName owner;
        Messages::ResourceType type;
        Messages::Class class_;
        Vector<Messages::ResourceRecord> records;
        Vector<Messages::Records::RRSIG> rrsigs;
        u32 rrsig_ttl { 0 };
    };
    struct VerifiedRRSet {
        SignedRRSet rrset;
        Messages::DomainName zone;
        Optional<size_t> wildcard_label_count;
        u32 ttl { 0 };
    };

private:
    // The DS and DNSKEY responses one validation has fetched so far. Validation never waits for a query: when it
    // needs a response it does not have yet, it records which one and fails, and runs again once that has arrived.
    struct Fetches {
        HashMap<ByteString, Messages::Message> responses;
        Optional<Messages::Question> missing;
    };
    struct PendingValidation;
    void run(NonnullRefPtr<PendingValidation>);
    static ErrorOr<Messages::Message> fetched(Fetches&, Messages::DomainName const&, Messages::ResourceType);
    ErrorOr<ValidationResult> try_validate(Messages::Message const& response, Messages::Question const& question, Fetches&);

    static Vector<SignedRRSet> group_rrsets(Vector<Messages::ResourceRecord> const&);

    // The closest enclosing zone of a name for which a chain of trust has been established (or found broken).
    struct Chain {
        SecurityStatus status;
        Messages::DomainName zone;
        Vector<Messages::Records::DNSKEY> keys;
        StringView reason;
    };
    ErrorOr<Chain> chain_for(Messages::DomainName const& name, Fetches&);

    struct ZoneKeys {
        Vector<Messages::Records::DNSKEY> keys;
        AK::UnixDateTime expires;
    };
    // Empty when none of the DS records can be used to authenticate the zone, which makes the zone insecure.
    ErrorOr<Optional<ZoneKeys>> zone_keys(Messages::DomainName const& zone, Vector<Messages::Records::DS> const&, Fetches&);

    struct Delegation {
        enum class Kind : u8 {
            Secure,
            Insecure,
            NotACut,
            NonExistent,
        };
        Kind kind;
        Vector<Messages::Records::DS> ds;
        AK::UnixDateTime expires;
    };
    ErrorOr<Delegation> delegation(Messages::DomainName const& name, Chain const& parent, Fetches&);

    enum class Verification : u8 {
        Verified,
        Failed,
    };
    // Tries every RRSIG of the set against `keys`, which must be the apex keys of `zone`.
    ErrorOr<Verification> verify_rrset(SignedRRSet const&, Messages::DomainName const& zone, Vector<Messages::Records::DNSKEY> const& keys, VerifiedRRSet& out);
    ErrorOr<Vector<VerifiedRRSet>> verify_denial_records(Vector<SignedRRSet> const& authority, Messages::DomainName const& zone, Vector<Messages::Records::DNSKEY> const& keys);

    struct DenialProofs {
        Vector<VerifiedRRSet> const& rrsets;

        bool proves_name_error(Messages::DomainName const& name) const;
        bool proves_no_data(Messages::DomainName const& name, Messages::ResourceType type) const;
        bool proves_next_closer_absent(Messages::DomainName const& name, size_t closest_encloser_label_count) const;
        bool proves_wildcard_absent(Messages::DomainName const& closest_encloser) const;
        bool proves_insecure_delegation(Messages::DomainName const& name) const;
        bool proves_name_exists_without_delegation(Messages::DomainName const& name) const;
    };

    QueryFunction m_query;
    Vector<Messages::Records::DS> m_trust_anchors;
    HashMap<ByteString, ZoneKeys> m_zone_keys;
    HashMap<ByteString, Delegation> m_delegations;
};

}
