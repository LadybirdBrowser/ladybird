/*
 * Copyright (c) 2024, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/HashMap.h>
#include <AK/IPv4Address.h>
#include <AK/MemoryStream.h>
#include <AK/QuickSort.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Socket.h>
#include <LibCore/UDPServer.h>
#include <LibCrypto/Curves/EdwardsCurve.h>
#include <LibCrypto/Hash/SHA1.h>
#include <LibCrypto/Hash/SHA2.h>
#include <LibDNS/Resolver.h>
#include <LibTest/TestCase.h>

#include <AK/Windows.h>

using namespace DNS::Messages;

namespace {

// A small authoritative server for signed zones, enough to drive the validator through every kind of response it
// has to authenticate: positive answers, CNAMEs, wildcards, NXDOMAIN and NODATA with NSEC or NSEC3, and
// delegations with and without DS records.

static u16 key_tag_for(Records::DNSKEY const& key)
{
    ByteBuffer rdata;
    MUST(key.to_raw(rdata));
    u32 sum = 0;
    for (size_t i = 0; i < rdata.size(); ++i)
        sum += (i & 1) ? rdata[i] : static_cast<u32>(rdata[i]) << 8;
    sum += (sum >> 16) & 0xffff;
    return sum & 0xffff;
}

static ByteString base32hex_encode(ByteBuffer const& input)
{
    static constexpr auto alphabet = "0123456789abcdefghijklmnopqrstuv"sv;
    StringBuilder builder;
    u32 bits = 0;
    size_t bit_count = 0;
    for (auto byte : input.bytes()) {
        bits = bits << 8 | byte;
        bit_count += 8;
        while (bit_count >= 5) {
            builder.append(alphabet[(bits >> (bit_count - 5)) & 0x1f]);
            bit_count -= 5;
        }
    }
    if (bit_count > 0)
        builder.append(alphabet[(bits << (5 - bit_count)) & 0x1f]);
    return builder.to_byte_string();
}

static ByteBuffer nsec3_hash(DomainName const& name)
{
    ByteBuffer input;
    MUST(name.canonicalized().to_raw(input));
    return MUST(ByteBuffer::copy(Crypto::Hash::SHA1::hash(input).bytes()));
}

static int compare_buffers(ByteBuffer const& a, ByteBuffer const& b)
{
    auto common = min(a.size(), b.size());
    auto order = memcmp(a.data(), b.data(), common);
    if (order != 0)
        return order;
    return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
}

struct Zone {
    DomainName apex;
    bool signed_ { true };
    bool use_nsec3 { false };
    ByteBuffer private_key;
    Records::DNSKEY dnskey;
    // owner name (canonical string) -> records at that name, grouped by nothing in particular
    HashMap<ByteString, Vector<ResourceRecord>> records;
    Vector<DomainName> names;
    // Names this zone delegates away.
    Vector<DomainName> delegations;

    // Test knobs.
    bool corrupt_signatures { false };
    bool omit_denial_records { false };
    bool omit_wildcard_proof { false };
    Optional<DomainName> spoof_answer_owner;

    static Zone create(StringView apex_name, bool use_nsec3 = false)
    {
        Zone zone;
        zone.apex = DomainName::from_string(apex_name);
        zone.use_nsec3 = use_nsec3;
        Crypto::Curves::Ed25519 ed25519;
        zone.private_key = MUST(ed25519.generate_private_key());
        auto public_key = MUST(ed25519.generate_public_key(zone.private_key));
        zone.dnskey = Records::DNSKEY { 257, 3, DNSSEC::Algorithm::ED25519, public_key, 0 };
        zone.dnskey.calculated_key_tag = key_tag_for(zone.dnskey);
        zone.add(zone.apex, ResourceType::DNSKEY, zone.dnskey, 3600);
        zone.add(zone.apex, ResourceType::SOA, Records::SOA { DomainName::from_string("ns"sv), DomainName::from_string("hostmaster"sv), 1, 3600, 600, 86400, 300 }, 3600);
        zone.add(zone.apex, ResourceType::NS, Records::NS { DomainName::from_string("ns"sv) }, 3600);
        return zone;
    }

    static Zone create_unsigned(StringView apex_name)
    {
        Zone zone;
        zone.apex = DomainName::from_string(apex_name);
        zone.signed_ = false;
        zone.add(zone.apex, ResourceType::SOA, Records::SOA { DomainName::from_string("ns"sv), DomainName::from_string("hostmaster"sv), 1, 3600, 600, 86400, 300 }, 3600);
        zone.add(zone.apex, ResourceType::NS, Records::NS { DomainName::from_string("ns"sv) }, 3600);
        return zone;
    }

    Records::DS ds() const
    {
        ByteBuffer input;
        MUST(apex.canonicalized().to_raw(input));
        MUST(dnskey.to_raw(input));
        return Records::DS { dnskey.calculated_key_tag, DNSSEC::Algorithm::ED25519, DNSSEC::DigestType::SHA256, MUST(ByteBuffer::copy(Crypto::Hash::SHA256::hash(input).bytes())) };
    }

    void add(DomainName const& owner, ResourceType type, Record record, u32 ttl)
    {
        auto key = owner.to_canonical_string();
        if (!records.contains(key))
            names.append(owner);
        records.ensure(key).append(ResourceRecord { owner, type, Class::IN, ttl, move(record), {} });
    }

    void add_a(StringView owner, IPv4Address address, u32 ttl = 300)
    {
        add(DomainName::from_string(owner), ResourceType::A, Records::A { address }, ttl);
    }

    void add_cname(StringView owner, StringView target)
    {
        add(DomainName::from_string(owner), ResourceType::CNAME, Records::CNAME { DomainName::from_string(target) }, 300);
    }

    // A delegation: the NS record at the child's apex, and a DS record for a signed child.
    void delegate(Zone const& child)
    {
        delegations.append(child.apex);
        add(child.apex, ResourceType::NS, Records::NS { DomainName::from_string("ns"sv) }, 3600);
        if (child.signed_)
            add(child.apex, ResourceType::DS, child.ds(), 3600);
    }

    // Generates the NSEC or NSEC3 chain. Call after every record has been added.
    void finalize()
    {
        if (!signed_)
            return;

        // Sort the names, then compute what types exist at each name.
        auto types_at = [&](DomainName const& name) {
            Vector<ResourceType> types;
            for (auto const& record : records.get(name.to_canonical_string()).value()) {
                if (!types.contains_slow(record.type))
                    types.append(record.type);
            }
            types.append(ResourceType::RRSIG);
            types.append(use_nsec3 ? ResourceType::NSEC3 : ResourceType::NSEC);
            return types;
        };

        auto sorted_names = names;
        if (use_nsec3) {
            quick_sort(sorted_names, [](auto const& a, auto const& b) { return compare_buffers(nsec3_hash(a), nsec3_hash(b)) < 0; });
            add(apex, ResourceType::NSEC3PARAM, Records::NSEC3PARAM { DNSSEC::NSEC3HashAlgorithm::SHA1, 0, 0, {} }, 0);
            for (size_t i = 0; i < sorted_names.size(); ++i) {
                auto const& name = sorted_names[i];
                auto next = nsec3_hash(sorted_names[(i + 1) % sorted_names.size()]);
                auto owner = apex.with_label_prepended(base32hex_encode(nsec3_hash(name)));
                auto types = types_at(name);
                if (delegations.contains_slow(name))
                    types.remove_all_matching([](auto type) { return type == ResourceType::NSEC3; });
                nsec_records.append(ResourceRecord { owner, ResourceType::NSEC3, Class::IN, 300, Records::NSEC3 { DNSSEC::NSEC3HashAlgorithm::SHA1, 0, 0, {}, next, types }, {} });
            }
            return;
        }

        quick_sort(sorted_names, [](auto const& a, auto const& b) { return DomainName::canonical_compare(a, b) < 0; });
        for (size_t i = 0; i < sorted_names.size(); ++i) {
            auto const& name = sorted_names[i];
            auto const& next = sorted_names[(i + 1) % sorted_names.size()];
            nsec_records.append(ResourceRecord { name, ResourceType::NSEC, Class::IN, 300, Records::NSEC { next, types_at(name) }, {} });
        }
    }

    Vector<ResourceRecord> nsec_records;

    ResourceRecord sign(Vector<ResourceRecord> const& rrset, DomainName const& signature_owner) const
    {
        auto now = UnixDateTime::now();
        auto owner = signature_owner.canonicalized();
        Records::RRSIG rrsig {
            rrset.first().type, DNSSEC::Algorithm::ED25519,
            static_cast<u8>(owner.is_wildcard() ? owner.labels.size() - 1 : owner.labels.size()),
            rrset.first().ttl, now + AK::Duration::from_seconds(3600), now - AK::Duration::from_seconds(3600),
            dnskey.calculated_key_tag, apex, ByteBuffer {}
        };

        ByteBuffer owner_wire;
        MUST(owner.to_raw(owner_wire));
        auto rdata_offset = owner_wire.size() + 2 + 2 + 4 + 2;
        Vector<ByteBuffer> encoded;
        for (auto const& record : rrset) {
            ByteBuffer buffer;
            MUST(record.to_canonical_raw(buffer, owner, rrsig.original_ttl));
            encoded.append(move(buffer));
        }
        quick_sort(encoded, [&](auto const& a, auto const& b) {
            auto a_rdata = a.bytes().slice(rdata_offset);
            auto b_rdata = b.bytes().slice(rdata_offset);
            auto common = min(a_rdata.size(), b_rdata.size());
            auto order = memcmp(a_rdata.data(), b_rdata.data(), common);
            return order != 0 ? order < 0 : a_rdata.size() < b_rdata.size();
        });

        ByteBuffer signed_data;
        MUST(rrsig.to_raw_excluding_signature(signed_data));
        for (auto const& buffer : encoded)
            MUST(signed_data.try_append(buffer));

        Crypto::Curves::Ed25519 ed25519;
        rrsig.signature = MUST(ed25519.sign(private_key, signed_data));
        if (corrupt_signatures)
            rrsig.signature[0] ^= 0x01;
        return ResourceRecord { rrset.first().name, ResourceType::RRSIG, Class::IN, rrset.first().ttl, move(rrsig), {} };
    }

    Vector<ResourceRecord> rrset(DomainName const& owner, ResourceType type) const
    {
        Vector<ResourceRecord> result;
        if (auto found = records.get(owner.to_canonical_string()); found.has_value()) {
            for (auto const& record : *found) {
                if (record.type == type)
                    result.append(record);
            }
        }
        return result;
    }

    bool has_name(DomainName const& name) const { return records.contains(name.to_canonical_string()); }

    // Appends an RRset and its signature to a section, optionally renaming the records (wildcard expansion or
    // a spoofed owner) while signing as `signature_owner`.
    void append_signed(Vector<ResourceRecord>& section, Vector<ResourceRecord> rrset, DomainName const& signature_owner, Optional<DomainName> presented_owner = {}) const
    {
        if (rrset.is_empty())
            return;
        auto signature = signed_ ? Optional<ResourceRecord>(sign(rrset, signature_owner)) : Optional<ResourceRecord> {};
        if (presented_owner.has_value()) {
            for (auto& record : rrset)
                record.name = *presented_owner;
            if (signature.has_value())
                signature->name = *presented_owner;
        }
        section.extend(move(rrset));
        if (signature.has_value())
            section.append(signature.release_value());
    }

    Vector<ResourceRecord> nsec_matching(DomainName const& name) const
    {
        Vector<ResourceRecord> result;
        if (use_nsec3) {
            auto owner = apex.with_label_prepended(base32hex_encode(nsec3_hash(name)));
            for (auto const& record : nsec_records) {
                if (record.name.equals_ignoring_case(owner))
                    result.append(record);
            }
            return result;
        }
        for (auto const& record : nsec_records) {
            if (record.name.equals_ignoring_case(name))
                result.append(record);
        }
        return result;
    }

    Vector<ResourceRecord> nsec_covering(DomainName const& name) const
    {
        Vector<ResourceRecord> result;
        if (use_nsec3) {
            auto hash = nsec3_hash(name);
            for (auto const& record : nsec_records) {
                auto const& nsec3 = record.record.get<Records::NSEC3>();
                auto owner_hash = nsec3_hash(DomainName {});
                // Recover the owner hash from the record's owner label.
                for (auto const& candidate : names) {
                    if (record.name.equals_ignoring_case(apex.with_label_prepended(base32hex_encode(nsec3_hash(candidate))))) {
                        owner_hash = nsec3_hash(candidate);
                        break;
                    }
                }
                auto after_owner = compare_buffers(owner_hash, hash) < 0;
                auto before_next = compare_buffers(hash, nsec3.next_hashed_owner_name) < 0;
                auto is_last = compare_buffers(nsec3.next_hashed_owner_name, owner_hash) <= 0;
                if (after_owner && (before_next || is_last))
                    result.append(record);
            }
            return result;
        }
        for (auto const& record : nsec_records) {
            auto const& nsec = record.record.get<Records::NSEC>();
            auto after_owner = DomainName::canonical_compare(record.name, name) < 0;
            auto before_next = DomainName::canonical_compare(name, nsec.next_domain_name) < 0;
            auto is_last = DomainName::canonical_compare(nsec.next_domain_name, record.name) <= 0;
            if (after_owner && (before_next || is_last))
                result.append(record);
        }
        return result;
    }

    DomainName closest_encloser(DomainName const& name) const
    {
        auto candidate = name;
        while (!has_name(candidate) && candidate.labels.size() > apex.labels.size())
            candidate = candidate.parent();
        return candidate;
    }

    void append_denial(Vector<ResourceRecord>& authority, Vector<ResourceRecord> denial_records) const
    {
        if (omit_denial_records)
            return;
        // Group by owner so each NSEC RRset gets one signature.
        while (!denial_records.is_empty()) {
            auto owner = denial_records.first().name;
            Vector<ResourceRecord> group;
            denial_records.remove_all_matching([&](auto const& record) {
                if (!record.name.equals_ignoring_case(owner))
                    return false;
                group.append(record);
                return true;
            });
            bool already_present = false;
            for (auto const& record : authority) {
                if (record.type == group.first().type && record.name.equals_ignoring_case(owner))
                    already_present = true;
            }
            if (!already_present)
                append_signed(authority, move(group), owner);
        }
    }

    Message respond(Question const& question) const
    {
        Message response;
        response.header.options.set_is_question(false);
        response.header.options.set_authenticated_data(true);
        response.header.question_count = 1;
        response.questions.append(question);

        auto name = question.name;
        Vector<ResourceRecord> answers;
        Vector<ResourceRecord> authority;

        for (size_t hops = 0; hops < 8; ++hops) {
            // A delegated name (or anything below it): a referral with the DS or a proof there is none.
            for (auto const& delegation : delegations) {
                if (delegation.is_ancestor_or_equal_of(name) && !(question.type == ResourceType::DS && delegation.equals_ignoring_case(name))) {
                    append_signed(authority, rrset(delegation, ResourceType::DS), delegation);
                    authority.extend(rrset(delegation, ResourceType::NS));
                    if (rrset(delegation, ResourceType::DS).is_empty())
                        append_denial(authority, nsec_matching(delegation));
                    goto done;
                }
            }

            if (has_name(name)) {
                auto exact = rrset(name, question.type);
                if (!exact.is_empty()) {
                    append_signed(answers, move(exact), name, spoof_answer_owner);
                    goto done;
                }
                auto cname = rrset(name, ResourceType::CNAME);
                if (!cname.is_empty() && question.type != ResourceType::CNAME) {
                    auto target = cname.first().record.get<Records::CNAME>().names;
                    append_signed(answers, move(cname), name);
                    if (!apex.is_ancestor_or_equal_of(target))
                        goto done;
                    name = target;
                    continue;
                }
                // No Data.
                append_signed(authority, rrset(apex, ResourceType::SOA), apex);
                append_denial(authority, nsec_matching(name));
                goto done;
            }

            {
                // Wildcard expansion, or Name Error.
                auto encloser = closest_encloser(name);
                auto wildcard = encloser.with_label_prepended("*"sv);
                auto next_closer = name.suffix(encloser.labels.size() + 1);
                if (has_name(wildcard) && !rrset(wildcard, question.type).is_empty()) {
                    append_signed(answers, rrset(wildcard, question.type), wildcard, name);
                    if (!omit_wildcard_proof)
                        append_denial(authority, nsec_covering(next_closer));
                    goto done;
                }
                if (has_name(wildcard)) {
                    // Wildcard No Data.
                    append_signed(authority, rrset(apex, ResourceType::SOA), apex);
                    if (use_nsec3)
                        append_denial(authority, nsec_matching(encloser));
                    append_denial(authority, nsec_covering(next_closer));
                    append_denial(authority, nsec_matching(wildcard));
                    goto done;
                }

                response.header.options.set_response_code(Options::ResponseCode::NameError);
                append_signed(authority, rrset(apex, ResourceType::SOA), apex);
                if (use_nsec3) {
                    append_denial(authority, nsec_matching(encloser));
                    append_denial(authority, nsec_covering(next_closer));
                } else {
                    append_denial(authority, nsec_covering(name));
                }
                append_denial(authority, nsec_covering(wildcard));
                goto done;
            }
        }

    done:
        response.answers = move(answers);
        response.authorities = move(authority);
        response.header.answer_count = response.answers.size();
        response.header.authority_count = response.authorities.size();
        return response;
    }
};

struct Server {
    Vector<Zone> zones;
    size_t queries { 0 };
    RefPtr<Core::UDPServer> udp;
    u16 port { 0 };

    Zone const& zone_for(Question const& question) const
    {
        Zone const* best = nullptr;
        for (auto const& zone : zones) {
            if (!zone.apex.is_ancestor_or_equal_of(question.name))
                continue;
            // DS records live in the parent zone.
            if (question.type == ResourceType::DS && zone.apex.equals_ignoring_case(question.name) && !zone.apex.labels.is_empty())
                continue;
            if (!best || zone.apex.labels.size() > best->apex.labels.size())
                best = &zone;
        }
        VERIFY(best);
        return *best;
    }

    void start()
    {
        for (auto& zone : zones)
            zone.finalize();

        udp = Core::UDPServer::construct();
        EXPECT(udp->bind(IPv4Address { 127, 0, 0, 1 }, 0));
        port = udp->local_port().value();
        udp->on_ready_to_receive = [this] {
            sockaddr_in from {};
            auto query_bytes = MUST(udp->receive(4096, from));
            ++queries;
            FixedMemoryStream stream { query_bytes.bytes() };
            auto query = MUST(Message::from_raw(stream));
            auto response = zone_for(query.questions.first()).respond(query.questions.first());
            response.header.id = query.header.id;
            ByteBuffer out;
            MUST(response.to_raw(out));
            MUST(udp->send(out.bytes(), from));
        };
    }

    Function<ErrorOr<Optional<DNS::Resolver::SocketResult>>()> socket_factory() const
    {
        return [port = port] -> ErrorOr<Optional<DNS::Resolver::SocketResult>> {
            Core::SocketAddress address { IPv4Address { 127, 0, 0, 1 }, port };
            return DNS::Resolver::SocketResult { TRY(Core::UDPSocket::connect(address)), DNS::Resolver::ConnectionMode::UDP };
        };
    }

    void anchor(DNS::Resolver& resolver) const
    {
        resolver.validator().set_trust_anchors({ zones.first().ds() });
    }
};

// Root zone, a signed child "example" with an address, a CNAME, a wildcard, and an unsigned grandchild.
static Server make_server(bool use_nsec3)
{
    Server server;
    auto root = Zone::create(""sv, use_nsec3);
    auto example = Zone::create("example"sv, use_nsec3);
    auto unsigned_child = Zone::create_unsigned("unsigned.example"sv);

    example.add_a("www.example"sv, IPv4Address { 192, 0, 2, 1 });
    example.add_a("other.example"sv, IPv4Address { 192, 0, 2, 2 });
    example.add_cname("alias.example"sv, "www.example"sv);
    example.add_a("*.wild.example"sv, IPv4Address { 192, 0, 2, 3 });
    example.add(DomainName::from_string("wild.example"sv), ResourceType::TXT, Records::TXT { "wild"sv }, 300);
    unsigned_child.add_a("host.unsigned.example"sv, IPv4Address { 192, 0, 2, 4 });

    example.delegate(unsigned_child);
    root.delegate(example);

    server.zones.append(move(root));
    server.zones.append(move(example));
    server.zones.append(move(unsigned_child));
    return server;
}

static ErrorOr<NonnullRefPtr<DNS::LookupResult const>> validated_lookup(DNS::Resolver& resolver, StringView name, ResourceType type = ResourceType::A)
{
    return resolver.lookup(name, Class::IN, Vector { type }, { .validate_dnssec_locally = true })->await();
}

static Optional<IPv4Address> first_address(DNS::LookupResult const& result)
{
    for (auto const& address : result.cached_addresses()) {
        if (auto const* v4 = address.get_pointer<IPv4Address>())
            return *v4;
    }
    return {};
}

}

TEST_CASE(secure_answer_validates)
{
    for (auto use_nsec3 : { false, true }) {
        Core::EventLoop loop;
        auto server = make_server(use_nsec3);
        server.start();
        DNS::Resolver resolver { server.socket_factory() };
        server.anchor(resolver);

        auto result = TRY_OR_FAIL(validated_lookup(resolver, "www.example"sv));
        EXPECT(result->is_dnssec_validated());
        EXPECT_EQ(first_address(*result), IPv4Address(192, 0, 2, 1));

        // The chain is cached: a second name in the zone needs only its own query and a DS probe at that name.
        auto queries_before = server.queries;
        auto other = TRY_OR_FAIL(validated_lookup(resolver, "other.example"sv));
        EXPECT(other->is_dnssec_validated());
        EXPECT_EQ(first_address(*other), IPv4Address(192, 0, 2, 2));
        EXPECT_EQ(server.queries - queries_before, 2u);
    }
}

TEST_CASE(cname_chain_validates)
{
    for (auto use_nsec3 : { false, true }) {
        Core::EventLoop loop;
        auto server = make_server(use_nsec3);
        server.start();
        DNS::Resolver resolver { server.socket_factory() };
        server.anchor(resolver);

        auto result = TRY_OR_FAIL(validated_lookup(resolver, "alias.example"sv));
        EXPECT(result->is_dnssec_validated());
        EXPECT_EQ(first_address(*result), IPv4Address(192, 0, 2, 1));
    }
}

TEST_CASE(wildcard_answer_validates_with_proof)
{
    for (auto use_nsec3 : { false, true }) {
        Core::EventLoop loop;
        auto server = make_server(use_nsec3);
        server.start();
        DNS::Resolver resolver { server.socket_factory() };
        server.anchor(resolver);

        auto result = TRY_OR_FAIL(validated_lookup(resolver, "anything.wild.example"sv));
        EXPECT(result->is_dnssec_validated());
        EXPECT_EQ(first_address(*result), IPv4Address(192, 0, 2, 3));
    }
}

TEST_CASE(wildcard_answer_without_proof_is_bogus)
{
    for (auto use_nsec3 : { false, true }) {
        Core::EventLoop loop;
        auto server = make_server(use_nsec3);
        server.zones[1].omit_wildcard_proof = true;
        server.start();
        DNS::Resolver resolver { server.socket_factory() };
        server.anchor(resolver);

        EXPECT(validated_lookup(resolver, "anything.wild.example"sv).is_error());
    }
}

TEST_CASE(name_error_is_proven)
{
    for (auto use_nsec3 : { false, true }) {
        Core::EventLoop loop;
        auto server = make_server(use_nsec3);
        server.start();
        DNS::Resolver resolver { server.socket_factory() };
        server.anchor(resolver);

        auto result = TRY_OR_FAIL(validated_lookup(resolver, "missing.example"sv));
        EXPECT(result->is_dnssec_validated());
        EXPECT(result->is_empty());
    }
}

TEST_CASE(name_error_without_proof_is_bogus)
{
    for (auto use_nsec3 : { false, true }) {
        Core::EventLoop loop;
        auto server = make_server(use_nsec3);
        server.zones[1].omit_denial_records = true;
        server.start();
        DNS::Resolver resolver { server.socket_factory() };
        server.anchor(resolver);

        EXPECT(validated_lookup(resolver, "missing.example"sv).is_error());
    }
}

TEST_CASE(no_data_is_proven)
{
    for (auto use_nsec3 : { false, true }) {
        Core::EventLoop loop;
        auto server = make_server(use_nsec3);
        server.start();
        DNS::Resolver resolver { server.socket_factory() };
        server.anchor(resolver);

        auto result = TRY_OR_FAIL(validated_lookup(resolver, "www.example"sv, ResourceType::AAAA));
        EXPECT(result->is_dnssec_validated());
        EXPECT(result->is_empty());
    }
}

TEST_CASE(no_data_without_proof_is_bogus)
{
    for (auto use_nsec3 : { false, true }) {
        Core::EventLoop loop;
        auto server = make_server(use_nsec3);
        server.zones[1].omit_denial_records = true;
        server.start();
        DNS::Resolver resolver { server.socket_factory() };
        server.anchor(resolver);

        EXPECT(validated_lookup(resolver, "www.example"sv, ResourceType::AAAA).is_error());
    }
}

TEST_CASE(bad_signature_is_bogus)
{
    Core::EventLoop loop;
    auto server = make_server(false);
    server.zones[1].corrupt_signatures = true;
    server.start();
    DNS::Resolver resolver { server.socket_factory() };
    server.anchor(resolver);

    EXPECT(validated_lookup(resolver, "www.example"sv).is_error());
}

TEST_CASE(answer_for_another_name_is_bogus)
{
    Core::EventLoop loop;
    auto server = make_server(false);
    // The signed RRset for other.example is presented under the queried name.
    server.zones[1].spoof_answer_owner = DomainName::from_string("www.example"sv);
    server.start();
    DNS::Resolver resolver { server.socket_factory() };
    server.anchor(resolver);

    EXPECT(validated_lookup(resolver, "other.example"sv).is_error());
}

TEST_CASE(child_key_must_match_ds)
{
    Core::EventLoop loop;
    auto server = make_server(false);
    // Re-key the child after the parent recorded its DS.
    auto rekeyed = Zone::create("example"sv);
    rekeyed.add_a("www.example"sv, IPv4Address { 192, 0, 2, 1 });
    server.zones[1] = move(rekeyed);
    server.start();
    DNS::Resolver resolver { server.socket_factory() };
    server.anchor(resolver);

    EXPECT(validated_lookup(resolver, "www.example"sv).is_error());
}

TEST_CASE(unsigned_delegation_is_insecure)
{
    for (auto use_nsec3 : { false, true }) {
        Core::EventLoop loop;
        auto server = make_server(use_nsec3);
        server.start();
        DNS::Resolver resolver { server.socket_factory() };
        server.anchor(resolver);

        auto result = TRY_OR_FAIL(validated_lookup(resolver, "host.unsigned.example"sv));
        EXPECT(!result->is_dnssec_validated());
        EXPECT_EQ(result->security_status(), DNS::DNSSEC::SecurityStatus::Insecure);
        EXPECT_EQ(first_address(*result), IPv4Address(192, 0, 2, 4));
    }
}

TEST_CASE(unsigned_delegation_without_proof_is_bogus)
{
    Core::EventLoop loop;
    auto server = make_server(false);
    server.zones[1].omit_denial_records = true;
    server.start();
    DNS::Resolver resolver { server.socket_factory() };
    server.anchor(resolver);

    EXPECT(validated_lookup(resolver, "host.unsigned.example"sv).is_error());
}

TEST_CASE(untrusted_root_key_is_bogus)
{
    Core::EventLoop loop;
    auto server = make_server(false);
    server.start();
    DNS::Resolver resolver { server.socket_factory() };
    server.anchor(resolver);
    // Anchor some other key instead of the server's root key.
    resolver.validator().set_trust_anchors({ Zone::create(""sv).ds() });

    EXPECT(validated_lookup(resolver, "www.example"sv).is_error());
}
