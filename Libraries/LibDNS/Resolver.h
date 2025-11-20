/*
 * Copyright (c) 2024, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/CountingStream.h>
#include <AK/HashTable.h>
#include <AK/MaybeOwned.h>
#include <AK/MemoryStream.h>
#include <AK/QuickSort.h>
#include <AK/Random.h>
#include <AK/StringView.h>
#include <AK/TemporaryChange.h>
#include <AK/Time.h>
#include <LibCore/Promise.h>
#include <LibCore/Socket.h>
#include <LibCore/Timer.h>
#include <LibCrypto/Certificate/Certificate.h>
#include <LibCrypto/Curves/EdwardsCurve.h>
#include <LibCrypto/PK/RSA.h>
#include <LibDNS/Message.h>
#include <LibThreading/RWLockProtected.h>

#define TRY_OR_REJECT_PROMISE(promise, expr)          \
    ({                                                \
        auto _result = (expr);                        \
        if (_result.is_error()) {                     \
            promise->reject(_result.release_error()); \
            return promise;                           \
        }                                             \
        _result.release_value();                      \
    })

namespace DNS {

// FIXME: Load these keys from a file (likely something trusted by the system, e.g. "whatever systemd does").
// https://data.iana.org/root-anchors/root-anchors.xml
static Vector<Messages::Records::DNSKEY> s_root_zone_dnskeys = {
    {
        .flags = 257,
        .protocol = 3,
        .algorithm = Messages::DNSSEC::Algorithm::RSASHA256,
        .public_key = decode_base64("AwEAAaz/tAm8yTn4Mfeh5eyI96WSVexTBAvkMgJzkKTOiW1vkIbzxeF3+/4RgWOq7HrxRixHlFlExOLAJr5emLvN7SWXgnLh4+B5xQlNVz8Og8kvArMtNROxVQuCaSnIDdD5LKyWbRd2n9WGe2R8PzgCmr3EgVLrjyBxWezF0jLHwVN8efS3rCj/EWgvIWgb9tarpVUDK/b58Da+sqqls3eNbuv7pr+eoZG+SrDK6nWeL3c6H5Apxz7LjVc1uTIdsIXxuOLYA4/ilBmSVIzuDWfdRUfhHdY6+cn8HFRm+2hM8AnXGXws9555KrUB5qihylGa8subX2Nn6UwNR1AkUTV74bU="sv).release_value(),
        .calculated_key_tag = 20326,
    },
    {
        .flags = 256,
        .protocol = 3,
        .algorithm = Messages::DNSSEC::Algorithm::RSASHA256,
        .public_key = decode_base64("AwEAAa96jeuknZlaeSrvyAJj6ZHv28hhOKkx3rLGXVaC6rXTsDc449/cidltpkyGwCJNnOAlFNKF2jBosZBU5eeHspaQWOmOElZsjICMQMC3aeHbGiShvZsx4wMYSjH8e7Vrhbu6irwCzVBApESjbUdpWWmEnhathWu1jo+siFUiRAAxm9qyJNg/wOZqqzL/dL/q8PkcRU5oUKEpUge71M3ej2/7CPqpdVwuMoTvoB+ZOT4YeGyxMvHmbrxlFzGOHOijtzN+u1TQNatX2XBuzZNQ1K+s2CXkPIZo7s6JgZyvaBevYtxPvYLw4z9mR7K2vaF18UYH9Z9GNUUeayffKC73PYc="sv).release_value(),
        .calculated_key_tag = 38696,
    },
};

class Resolver;

class LookupResult : public AtomicRefCounted<LookupResult>
    , public Weakable<LookupResult> {
public:
    explicit LookupResult(Messages::DomainName name)
        : m_name(move(name))
    {
    }

    Vector<Variant<IPv4Address, IPv6Address>> cached_addresses() const
    {
        Vector<Variant<IPv4Address, IPv6Address>> result;
        for (auto& re : m_cached_records) {
            re.record.record.visit(
                [&](Messages::Records::A const& a) { result.append(a.address); },
                [&](Messages::Records::AAAA const& aaaa) { result.append(aaaa.address); },
                [](auto&) {});
        }
        return result;
    }

    bool has_cached_addresses() const
    {
        return has_record_of_type(Messages::ResourceType::A) || has_record_of_type(Messages::ResourceType::AAAA);
    }

    void check_expiration()
    {
        if (!m_valid)
            return;

        auto now = AK::UnixDateTime::now();
        for (size_t i = 0; i < m_cached_records.size();) {
            auto& record = m_cached_records[i];
            if (record.expiration.has_value() && record.expiration.value() < now) {
                dbgln_if(DNS_DEBUG, "DNS: Removing expired record for {}", m_name.to_string());
                m_cached_records.remove(i);
            } else {
                dbgln_if(DNS_DEBUG, "DNS: Keeping record for {} (expires in {})", m_name.to_string(),
                    record.expiration.has_value() ? record.expiration.value().to_string() : "never"_string);
                ++i;
            }
        }

        if (m_cached_records.is_empty() && m_request_done)
            m_valid = false;
    }

    void add_record(Messages::ResourceRecord record)
    {
        m_valid = true;
        auto expiration = record.ttl > 0 ? Optional<AK::UnixDateTime>(AK::UnixDateTime::now() + AK::Duration::from_seconds(record.ttl)) : OptionalNone();
        m_cached_records.append({ move(record), move(expiration) });
    }

    Vector<Messages::ResourceRecord> records() const
    {
        Vector<Messages::ResourceRecord> result;
        result.ensure_capacity(m_cached_records.size());
        for (auto& re : m_cached_records)
            result.unchecked_append(re.record);
        return result;
    }

    Vector<Messages::ResourceRecord> records(Messages::ResourceType type) const
    {
        Vector<Messages::ResourceRecord> result;
        for (auto& re : m_cached_records) {
            if (re.record.type == type)
                result.append(re.record);
        }
        return result;
    }

    Messages::ResourceRecord const& record(Messages::ResourceType type) const
    {
        for (auto const& re : m_cached_records) {
            if (re.record.type == type)
                return re.record;
        }
        VERIFY_NOT_REACHED();
    }

    template<typename RR>
    RR const& record() const
    {
        for (auto const& re : m_cached_records) {
            if (re.record.type == RR::type)
                return re.record.record.get<RR>();
        }
        VERIFY_NOT_REACHED();
    }

    bool has_record_of_type(Messages::ResourceType type, bool later = false) const
    {
        if (later && m_desired_types.contains(type))
            return true;

        for (auto const& re : m_cached_records) {
            if (re.record.type == type)
                return true;
        }
        return false;
    }

    void will_add_record_of_type(Messages::ResourceType type) { m_desired_types.set(type); }
    void finished_request() { m_request_done = true; }

    void set_id(u16 id) { m_id = id; }
    u16 id() { return m_id; }

    bool can_be_removed() const { return !m_valid && m_request_done; }
    bool is_done() const { return m_request_done; }
    bool is_empty() const { return m_cached_records.is_empty(); }
    void set_dnssec_validated(bool validated) { m_dnssec_validated = validated; }
    bool is_dnssec_validated() const { return m_dnssec_validated; }
    void set_being_dnssec_validated(bool validated) { m_being_dnssec_validated = validated; }
    bool is_being_dnssec_validated() const { return m_being_dnssec_validated; }
    Messages::DomainName const& name() const { return m_name; }

    Vector<Messages::Records::DNSKEY> const& used_dnskeys() const { return m_used_dnskeys; }
    void add_dnskey(Messages::Records::DNSKEY key)
    {
        if (m_seen_key_tags.set(key.calculated_key_tag) == AK::HashSetResult::InsertedNewEntry)
            m_used_dnskeys.append(move(key));
    }

private:
    bool m_valid { false };
    bool m_request_done { false };
    bool m_dnssec_validated { false };
    bool m_being_dnssec_validated { false };
    Messages::DomainName m_name;

    struct RecordWithExpiration {
        Messages::ResourceRecord record;
        Optional<AK::UnixDateTime> expiration;
    };

    Vector<RecordWithExpiration> m_cached_records;
    HashTable<Messages::ResourceType> m_desired_types;
    Vector<Messages::Records::DNSKEY> m_used_dnskeys {};
    HashTable<u16> m_seen_key_tags;
    u16 m_id { 0 };
};

class Resolver {
    struct PendingLookup {
        u16 id { 0 };
        ByteString name;
        Messages::DomainName parsed_name;
        WeakPtr<LookupResult> result;
        NonnullRefPtr<Core::Promise<NonnullRefPtr<LookupResult const>>> promise;
        NonnullRefPtr<Core::Timer> repeat_timer;
        size_t times_repeated { 0 };
    };

public:
    enum class ConnectionMode {
        TCP,
        UDP,
    };

    struct LookupOptions {
        bool validate_dnssec_locally { false };
        PendingLookup* repeating_lookup { nullptr };

        static LookupOptions default_() { return {}; }
    };

    struct SocketResult {
        MaybeOwned<Core::Socket> socket;
        ConnectionMode mode;
    };

    using CreateSocketFunction = Function<NonnullRefPtr<Core::Promise<SocketResult>>()>;

    Resolver(CreateSocketFunction create_socket)
        : m_pending_lookups(make<RedBlackTree<u16, PendingLookup>>())
        , m_create_socket(move(create_socket))
    {
        m_cache.with_write_locked([&](auto& cache) {
            auto add_v4v6_entry = [&cache](StringView name_string, IPv4Address v4, IPv6Address v6) {
                auto name = Messages::DomainName::from_string(name_string);
                auto ptr = make_ref_counted<LookupResult>(name);
                ptr->will_add_record_of_type(Messages::ResourceType::A);
                ptr->will_add_record_of_type(Messages::ResourceType::AAAA);
                cache.set(name_string, ptr);

                ptr->add_record({ .name = {}, .type = Messages::ResourceType::A, .class_ = Messages::Class::IN, .ttl = 0, .record = Messages::Records::A { v4 }, .raw = {} });
                ptr->add_record({ .name = {}, .type = Messages::ResourceType::AAAA, .class_ = Messages::Class::IN, .ttl = 0, .record = Messages::Records::AAAA { v6 }, .raw = {} });
                ptr->finished_request();
            };

            add_v4v6_entry("localhost"sv, { 127, 0, 0, 1 }, IPv6Address::loopback());
        });
    }

    NonnullRefPtr<Core::Promise<Empty>> when_socket_ready()
    {
        auto promise = Core::Promise<Empty>::construct();

        auto has_connection_without_restart_promise = has_connection(false);
        has_connection_without_restart_promise->when_resolved([this, promise](bool ready) {
            if (ready) {
                promise->resolve({});
                return;
            }

            auto has_connection_with_restart_promise = has_connection();
            has_connection_with_restart_promise->when_resolved([promise](bool ready) {
                if (ready) {
                    promise->resolve({});
                    return;
                }

                promise->reject(Error::from_string_literal("Failed to create socket"));
            });

            has_connection_with_restart_promise->when_rejected([promise](Error const& error) {
                promise->reject(Error::copy(error));
            });

            promise->add_child(move(has_connection_with_restart_promise));
        });

        has_connection_without_restart_promise->when_rejected([promise](Error const& error) {
            promise->reject(Error::copy(error));
        });

        promise->add_child(move(has_connection_without_restart_promise));
        return promise;
    }

    void reset_connection()
    {
        m_socket.with_write_locked([&](auto& socket) { socket = {}; });
    }

    NonnullRefPtr<LookupResult const> expect_cached(StringView name, Messages::Class class_ = Messages::Class::IN)
    {
        return expect_cached(name, class_, Array { Messages::ResourceType::A, Messages::ResourceType::AAAA });
    }

    NonnullRefPtr<LookupResult const> expect_cached(StringView name, Messages::Class class_, Span<Messages::ResourceType const> desired_types)
    {
        auto result = lookup_in_cache(name, class_, desired_types);
        VERIFY(!result.is_null());
        dbgln_if(DNS_DEBUG, "DNS::expect({}) -> OK", name);
        return *result;
    }

    RefPtr<LookupResult const> lookup_in_cache(StringView name, Messages::Class class_ = Messages::Class::IN)
    {
        return lookup_in_cache(name, class_, Array { Messages::ResourceType::A, Messages::ResourceType::AAAA });
    }

    RefPtr<LookupResult const> lookup_in_cache(StringView name, Messages::Class, Span<Messages::ResourceType const> desired_types)
    {
        return m_cache.with_read_locked([&](auto& cache) -> RefPtr<LookupResult const> {
            auto it = cache.find(name);
            if (it == cache.end())
                return {};

            auto& result = *it->value;
            for (auto const& type : desired_types) {
                if (!result.has_record_of_type(type))
                    return {};
            }

            return result;
        });
    }

    NonnullRefPtr<Core::Promise<NonnullRefPtr<LookupResult const>>> lookup(ByteString name, Messages::Class class_, Vector<Vector<Messages::ResourceType>> desired_types, LookupOptions options = LookupOptions::default_())
    {
        using ResultPromise = Core::Promise<NonnullRefPtr<LookupResult const>>;
        Vector<NonnullRefPtr<ResultPromise>> promises;
        promises.ensure_capacity(desired_types.size());

        for (auto& types : desired_types)
            promises.unchecked_append(lookup(name, class_, types, options));

        auto result_promise = Core::Promise<NonnullRefPtr<LookupResult const>>::construct();
        result_promise->add_child(Core::Promise<Empty>::after(promises)
                ->when_resolved([promises, result_promise = result_promise->make_weak_ptr<ResultPromise>()](auto&&) {
                    if (!result_promise.ptr())
                        return;
                    VERIFY(promises.first()->is_resolved());

                    // NOTE: Since this is already resolved, this will be called immediately.
                    promises.first()->when_resolved([result_promise](NonnullRefPtr<LookupResult const> const& result) {
                        result_promise->resolve(result);
                    });
                })
                .when_rejected([promises, result_promise = result_promise->make_weak_ptr<ResultPromise>()](auto&& error) {
                    if (!result_promise.ptr())
                        return;
                    for (auto& promise : promises) {
                        if (promise->is_resolved()) {
                            // NOTE: Since this is already resolved, this will be called immediately.
                            promise->when_resolved([result_promise](NonnullRefPtr<LookupResult const> const& result) {
                                result_promise->resolve(result);
                            });
                            return;
                        }
                    }
                    result_promise->reject(move(error));
                }));
        return result_promise;
    }

    NonnullRefPtr<Core::Promise<NonnullRefPtr<LookupResult const>>> lookup(ByteString name, Messages::Class class_ = Messages::Class::IN, LookupOptions options = LookupOptions::default_())
    {
        return lookup(move(name), class_, { Messages::ResourceType::A, Messages::ResourceType::AAAA }, options);
    }

    NonnullRefPtr<Core::Promise<NonnullRefPtr<LookupResult const>>> lookup(ByteString name, Messages::Class class_, Vector<Messages::ResourceType> desired_types, LookupOptions options = LookupOptions::default_())
    {
        flush_cache();

        if (options.repeating_lookup && options.repeating_lookup->times_repeated >= 5) {
            dbgln_if(DNS_DEBUG, "DNS: Repeating lookup for {} timed out", name);
            auto promise = options.repeating_lookup->promise;
            promise->reject(Error::from_string_literal("DNS lookup timed out"));
            m_pending_lookups.with_write_locked([&](auto& lookups) {
                lookups->remove(options.repeating_lookup->id);
            });
            return promise;
        }

        auto lookup_promise = options.repeating_lookup ? options.repeating_lookup->promise : Core::Promise<NonnullRefPtr<LookupResult const>>::construct();

        if (auto maybe_ipv4 = IPv4Address::from_string(name); maybe_ipv4.has_value()) {
            dbgln_if(DNS_DEBUG, "DNS: Resolving {} as IPv4", name);
            if (desired_types.contains_slow(Messages::ResourceType::A)) {
                auto result = make_ref_counted<LookupResult>(Messages::DomainName {});
                result->add_record({ .name = {}, .type = Messages::ResourceType::A, .class_ = Messages::Class::IN, .ttl = 0, .record = Messages::Records::A { maybe_ipv4.release_value() }, .raw = {} });
                result->finished_request();
                lookup_promise->resolve(move(result));
                return lookup_promise;
            }
        }

        if (auto maybe_ipv6 = IPv6Address::from_string(name); maybe_ipv6.has_value()) {
            dbgln_if(DNS_DEBUG, "DNS: Resolving {} as IPv6", name);
            if (desired_types.contains_slow(Messages::ResourceType::AAAA)) {
                auto result = make_ref_counted<LookupResult>(Messages::DomainName {});
                result->add_record({ .name = {}, .type = Messages::ResourceType::AAAA, .class_ = Messages::Class::IN, .ttl = 0, .record = Messages::Records::AAAA { maybe_ipv6.release_value() }, .raw = {} });
                result->finished_request();
                lookup_promise->resolve(move(result));
                return lookup_promise;
            }
        }

        if (auto result = lookup_in_cache(name, class_, desired_types)) {
            dbgln_if(DNS_DEBUG, "DNS: Resolving {} from cache...", name);
            if (!options.validate_dnssec_locally || result->is_dnssec_validated()) {
                dbgln_if(DNS_DEBUG, "DNS: Resolved {} from cache", name);
                lookup_promise->resolve(result.release_nonnull());
                return lookup_promise;
            }
            dbgln_if(DNS_DEBUG, "DNS: Cache entry for {} is not DNSSEC validated (and we expect that), re-resolving", name);
        }

        auto domain_name = Messages::DomainName::from_string(name);

        auto has_established_connection = [=, this] {
            auto already_in_cache = false;
            auto result = m_cache.with_write_locked([&](auto& cache) -> NonnullRefPtr<LookupResult> {
                dbgln_if(DNS_DEBUG, "DNS: Resolving {}...", name);
                auto existing = [&] -> RefPtr<LookupResult> {
                    if (cache.contains(name)) {
                        dbgln_if(DNS_DEBUG, "DNS: Resolving {} from cache...", name);
                        auto ptr = *cache.get(name);

                        already_in_cache = (!options.validate_dnssec_locally && !ptr->is_being_dnssec_validated()) || ptr->is_dnssec_validated();
                        for (auto const& type : desired_types) {
                            if (!ptr->has_record_of_type(type, !options.validate_dnssec_locally && !ptr->is_being_dnssec_validated())) {
                                already_in_cache = false;
                                break;
                            }
                        }

                        dbgln_if(DNS_DEBUG, "DNS: Found {} in cache, already_in_cache={}", name, already_in_cache);
                        dbgln_if(DNS_DEBUG, "DNS: That entry is {} DNSSEC validated", ptr->is_dnssec_validated() ? "already" : "not");
                        for (auto const& entry : ptr->records())
                            dbgln_if(DNS_DEBUG, "DNS: Found record of type {}", Messages::to_string(entry.type));
                        return ptr;
                    }
                    return nullptr;
                }();

                if (existing) {
                    dbgln_if(DNS_DEBUG, "DNS: Resolved {} from cache", name);
                    return *existing;
                }

                dbgln_if(DNS_DEBUG, "DNS: Adding {} to cache", name);
                auto ptr = make_ref_counted<LookupResult>(domain_name);
                if (!ptr->is_dnssec_validated())
                    ptr->set_dnssec_validated(options.validate_dnssec_locally);
                for (auto const& type : desired_types)
                    ptr->will_add_record_of_type(type);
                cache.set(name, ptr);
                return ptr;
            });

            Optional<u16> cached_result_id;
            if (already_in_cache) {
                auto id = result->id();
                cached_result_id = id;
                auto existing_promise = m_pending_lookups.with_write_locked(
                    [&](auto& lookups) -> RefPtr<Core::Promise<NonnullRefPtr<LookupResult const>>> {
                        if (auto* lookup = lookups->find(id))
                            return lookup->promise;
                        return nullptr;
                    });
                if (existing_promise) {
                    auto previous_on_resolution = move(existing_promise->on_resolution);
                    existing_promise->on_resolution = [lookup_promise, previous_on_resolution = move(previous_on_resolution)](NonnullRefPtr<LookupResult const> result) -> ErrorOr<void> {
                        TRY(previous_on_resolution(result));
                        lookup_promise->resolve(move(result));
                        return {};
                    };

                    auto previous_on_rejection = move(existing_promise->on_rejection);
                    existing_promise->on_rejection = [lookup_promise, previous_on_rejection = move(previous_on_rejection)](Error& error) -> void {
                        previous_on_rejection(error);
                        lookup_promise->reject(Error::copy(error));
                    };

                    existing_promise->add_child(lookup_promise);
                    return;
                }

                // Something has gone wrong if there are no pending lookups but the result isn't done.
                // Continue on and hope that we eventually resolve or timeout in that case.
                if (result->is_done()) {
                    lookup_promise->resolve(*result);
                    return;
                }
            }

            Messages::Message query;
            if (cached_result_id.has_value()) {
                query.header.id = cached_result_id.value();
            } else if (options.repeating_lookup) {
                query.header.id = options.repeating_lookup->id;
                options.repeating_lookup->times_repeated++;
            } else {
                m_pending_lookups.with_read_locked([&](auto& lookups) {
                    do
                        fill_with_random({ &query.header.id, sizeof(query.header.id) });
                    while (lookups->find(query.header.id) != nullptr);
                });
            }
            query.header.question_count = max(1u, desired_types.size());
            query.header.options.set_response_code(Messages::Options::ResponseCode::NoError);
            query.header.options.set_recursion_desired(true);
            query.header.options.set_op_code(Messages::OpCode::Query);
            for (auto const& type : desired_types) {
                query.questions.append(Messages::Question {
                    .name = domain_name,
                    .type = type,
                    .class_ = class_,
                });
            }

            if (query.questions.is_empty()) {
                query.questions.append(Messages::Question {
                    .name = Messages::DomainName::from_string(name),
                    .type = Messages::ResourceType::A,
                    .class_ = class_,
                });
            }

            if (options.validate_dnssec_locally) {
                query.header.additional_count = 1;
                query.header.options.set_checking_disabled(true);
                query.header.options.set_authenticated_data(true);
                auto opt = Messages::Records::OPT {
                    .udp_payload_size = 4096,
                    .extended_rcode_and_flags = 0,
                    .options = {},
                };
                opt.set_dnssec_ok(true);

                query.additional_records.append(Messages::ResourceRecord {
                    .name = Messages::DomainName::from_string(""sv),
                    .type = Messages::ResourceType::OPT,
                    .class_ = class_,
                    .ttl = 0,
                    .record = move(opt),
                    .raw = {},
                });
            }

            result->set_id(query.header.id);

            auto cached_entry = options.repeating_lookup
                ? nullptr
                : m_pending_lookups.with_write_locked([&](auto& pending_lookups) -> PendingLookup* {
                      // One more try to make sure we're not overwriting an existing lookup
                      if (cached_result_id.has_value()) {
                          if (auto* lookup = pending_lookups->find(*cached_result_id))
                              return lookup;
                      }

                      pending_lookups->insert(query.header.id, { query.header.id, name, domain_name, result->make_weak_ptr(), lookup_promise, Core::Timer::create(), 0 });
                      auto p = pending_lookups->find(query.header.id);
                      p->repeat_timer->set_single_shot(true);
                      p->repeat_timer->set_interval(1000);
                      p->repeat_timer->on_timeout = [=, this] {
                          (void)lookup(name, class_, desired_types, { .validate_dnssec_locally = options.validate_dnssec_locally, .repeating_lookup = p });
                      };

                      return nullptr;
                  });

            if (cached_entry) {
                dbgln_if(DNS_DEBUG, "DNS::lookup({}) -> Lookup already underway", name);
                auto previous_on_resolution = move(cached_entry->promise->on_resolution);
                cached_entry->promise->on_resolution = [lookup_promise, previous_on_resolution = move(previous_on_resolution)](NonnullRefPtr<LookupResult const> result) -> ErrorOr<void> {
                    TRY(previous_on_resolution(result));
                    lookup_promise->resolve(move(result));
                    return {};
                };

                auto previous_on_rejection = move(cached_entry->promise->on_rejection);
                cached_entry->promise->on_rejection = [lookup_promise, previous_on_rejection = move(previous_on_rejection)](Error& error) -> void {
                    previous_on_rejection(error);
                    lookup_promise->reject(Error::copy(error));
                };

                cached_entry->promise->add_child(lookup_promise);
                return;
            }

            auto pending_lookup = m_pending_lookups.with_write_locked([&](auto& lookups) -> PendingLookup* {
                return lookups->find(query.header.id);
            });

            ByteBuffer query_bytes;
            MUST(query.to_raw(query_bytes));

            if (m_mode == ConnectionMode::TCP) {
                auto original_query_bytes = query_bytes;
                query_bytes = MUST(ByteBuffer::create_uninitialized(query_bytes.size() + sizeof(u16)));
                NetworkOrdered<u16> size = original_query_bytes.size();
                query_bytes.overwrite(0, &size, sizeof(size));
                query_bytes.overwrite(sizeof(size), original_query_bytes.data(), original_query_bytes.size());
            }

            auto write_result = m_socket.with_write_locked([&](auto& socket) {
                return (*socket)->write_until_depleted(query_bytes.bytes());
            });
            if (write_result.is_error()) {
                lookup_promise->reject(write_result.release_error());
                return;
            }

            pending_lookup->repeat_timer->start();
        };

        auto has_connection_with_restart_promise = has_connection();
        has_connection_with_restart_promise->when_resolved([options, name, domain_name, lookup_promise, has_established_connection = move(has_established_connection)](bool has_connection) {
            if (has_connection) {
                has_established_connection();
                return;
            }

            if (options.validate_dnssec_locally) {
                lookup_promise->reject(Error::from_string_literal("No connection available to validate DNSSEC"));
                return;
            }

            // Use system resolver
            // FIXME: Use an underlying resolver instead.
            dbgln_if(DNS_DEBUG, "Not ready to resolve, using system resolver and skipping cache for {}", name);
            auto record_or_error = Core::Socket::resolve_host(name, Core::Socket::SocketType::Stream);
            if (record_or_error.is_error()) {
                lookup_promise->reject(record_or_error.release_error());
                return;
            }
            auto result = make_ref_counted<LookupResult>(domain_name);
            auto records = record_or_error.release_value();

            for (auto const& record : records) {
                record.visit(
                    [&](IPv4Address const& address) {
                        result->add_record({ .name = {}, .type = Messages::ResourceType::A, .class_ = Messages::Class::IN, .ttl = 0, .record = Messages::Records::A { address }, .raw = {} });
                    },
                    [&](IPv6Address const& address) {
                        result->add_record({ .name = {}, .type = Messages::ResourceType::AAAA, .class_ = Messages::Class::IN, .ttl = 0, .record = Messages::Records::AAAA { address }, .raw = {} });
                    });
            }
            result->finished_request();
            lookup_promise->resolve(result);
        });

        has_connection_with_restart_promise->when_rejected([lookup_promise](Error const& error) {
            lookup_promise->reject(Error::copy(error));
        });

        lookup_promise->add_child(move(has_connection_with_restart_promise));
        return lookup_promise;
    }

private:
    ErrorOr<Messages::Message> parse_one_message()
    {
        if (m_mode == ConnectionMode::UDP)
            return m_socket.with_write_locked([&](auto& socket) { return Messages::Message::from_raw(**socket); });

        return m_socket.with_write_locked([&](auto& socket) -> ErrorOr<Messages::Message> {
            if (!TRY((*socket)->can_read_without_blocking()))
                return Error::from_errno(EAGAIN);

            auto size = TRY((*socket)->template read_value<NetworkOrdered<u16>>());
            auto buffer = TRY(ByteBuffer::create_uninitialized(size));
            TRY((*socket)->read_until_filled(buffer));
            FixedMemoryStream stream { static_cast<ReadonlyBytes>(buffer) };
            return Messages::Message::from_raw(stream);
        });
    }

    void process_incoming_messages()
    {
        while (true) {
            if (auto result = m_socket.with_read_locked([](auto& socket) {
                    return (*socket)->can_read_without_blocking();
                });
                result.is_error() || !result.value())
                break;
            auto message_or_err = parse_one_message();
            if (message_or_err.is_error()) {
                if (!message_or_err.error().is_errno() || message_or_err.error().code() != EAGAIN)
                    dbgln("DNS: Failed to receive message: {}", message_or_err.error());
                break;
            }

            auto message = message_or_err.release_value();
            auto result = m_pending_lookups.with_write_locked([&](auto& lookups) -> ErrorOr<void> {
                auto* lookup = lookups->find(message.header.id);
                if (!lookup)
                    return Error::from_string_literal("No pending lookup found for this message");

                if (lookup->result.is_null()) {
                    dbgln_if(DNS_DEBUG, "DNS: Received a message with no pending lookup (id={})", message.header.id);
                    return {}; // Message is a response to a lookup that's been purged from the cache, ignore it
                }

                lookup->repeat_timer->stop();

                auto result = lookup->result.strong_ref();
                if (result->is_dnssec_validated())
                    return validate_dnssec(move(message), *lookup, *result);

                if constexpr (DNS_DEBUG) {
                    switch (message.header.options.response_code()) {
                    case Messages::Options::ResponseCode::FormatError:
                        dbgln("DNS: Received FormatError response code");
                        break;
                    case Messages::Options::ResponseCode::ServerFailure:
                        dbgln("DNS: Received ServerFailure response code");
                        break;
                    case Messages::Options::ResponseCode::NameError:
                        dbgln("DNS: Received NameError response code");
                        break;
                    default:
                        break;
                    }
                }

                for (auto& record : message.answers)
                    result->add_record(move(record));

                result->finished_request();
                lookup->promise->resolve(*result);
                lookups->remove(message.header.id);
                return {};
            });
            if (result.is_error())
                dbgln_if(DNS_DEBUG, "DNS: Received a message with no pending lookup: {}", result.error());
        }
    }

    using RRSet = Vector<Messages::ResourceRecord>;
    struct CanonicalizedRRSetWithRRSIG {
        RRSet rrset;
        Messages::Records::RRSIG rrsig;
        Vector<Messages::Records::DNSKEY> dnskeys;
    };

    // https://www.rfc-editor.org/rfc/rfc2535
    NonnullRefPtr<Core::Promise<bool>> validate_dnssec_chain_step(Messages::DomainName const& name, bool top_level = false)
    {
        dbgln_if(DNS_DEBUG, "DNS: Validating DNSSEC chain for {}", name.to_string());
        auto promise = Core::Promise<bool>::construct();
        //  6.3.1. authentication leads to chains of alternating SIG and KEY RRs with the first SIG
        //         signing the original data whose authenticity is to be shown and the final KEY
        //         being some trusted key staticly configured at the resolver performing
        //         the authentication.
        // If this is the root, we're done, just return true.
        if (name.labels.size() == 0) {
            promise->resolve(true);
            return promise;
        }

        // 2.3. Every name in a secured zone will have associated with it at least
        //      one SIG resource record for each resource type under that name except
        //      for glue address RRs and delegation point NS RRs.  A security aware
        //      server will attempt to return, with RRs retrieved, the corresponding
        //      SIGs.  If a server is not security aware, the resolver must retrieve
        //      all the SIG records for a name and select the one or ones that sign
        //      the resource record set(s) that resolver is interested in.
        //
        //  2.3.4 There MUST be a zone KEY RR, signed by its superzone, for every
        //        subzone if the superzone is secure. This will normally appear in the
        //        subzone and may also be included in the superzone.  But, in the case
        //        of an unsecured subzone which can not or will not be modified to add
        //        any security RRs, a KEY declaring the subzone to be unsecured MUST
        //        appear with the superzone signature in the superzone, if the
        //        superzone is secure. For all but one other RR type the data from the
        //        subzone is more authoritative so only the subzone KEY RR should be
        //        signed in the superzone if it appears there. The NS and any glue
        //        address RRs SHOULD only be signed in the subzone. The SOA and any
        //        other RRs that have the zone name as owner should appear only in the
        //        subzone and thus are signed only there.

        // Figure out if this is a delegation point.
        // The records needed are SOA, DS and NS - look them up concurrently.
        auto delegation_point_lookup = lookup(name.to_string().to_byte_string(), Messages::Class::IN, { Vector { Messages::ResourceType::SOA }, { Messages::ResourceType::DS }, { Messages::ResourceType::NS } }, { .validate_dnssec_locally = !top_level });

        delegation_point_lookup->when_resolved([this, promise, name](NonnullRefPtr<LookupResult const> const& result) {
            // - Lookup the SOA record for the domain.
            // - If we have no SOA record-
            if (!result->has_record_of_type(Messages::ResourceType::SOA)) {
                dbgln_if(DNS_DEBUG, "DNS: No SOA record found for {}", name.to_string());
                // - If there's no DS record, check for an NS record-
                if (!result->has_record_of_type(Messages::ResourceType::DS)) {
                    dbgln_if(DNS_DEBUG, "DNS: No DS record found for {}", name.to_string());
                    // - If there's no DS record, check for an NS record-
                    if (result->has_record_of_type(Messages::ResourceType::NS)) {
                        // - but if there _is_ an NS record, this is a broken delegation, so reject.
                        dbgln_if(DNS_DEBUG, "DNS: Found NS record for {}", name.to_string());
                        promise->resolve(false);
                        return;
                    }
                    dbgln_if(DNS_DEBUG, "DNS: No NS record found for {}", name.to_string());

                    // NOTE: We have to defer here due to delegation_point_lookup being resolved from a lookup, which is whilst pending lookups are locked.
                    Core::deferred_invoke([this, promise, name] {
                        // this is just part of the parent delegation, so go up one level.
                        auto upper_level_promise = validate_dnssec_chain_step(name.parent());
                        upper_level_promise->when_resolved([promise](bool valid) {
                            promise->resolve(move(valid));
                        });

                        upper_level_promise->when_rejected([promise](Error const& error) {
                            promise->reject(Error::copy(error));
                        });

                        promise->add_child(move(upper_level_promise));
                    });
                    return;
                }
                // - If there is a DS record, this is a separate zone...but since we don't have an SOA record, this is a misconfigured zone.
                // Let's just reject.
                dbgln_if(DNS_DEBUG, "DNS: Found DS record for {}", name.to_string());
                promise->resolve(false);
                return;
            }

            // So we have an SOA record, there's much rejoicing and we can continue.
            auto& soa = result->record<Messages::Records::SOA>();
            dbgln_if(DNS_DEBUG, "DNS: Found SOA record for {}: {}", name.to_string(), soa.mname.to_string());
            if (soa.mname == name.parent()) {
                // NOTE: We have to defer here due to delegation_point_lookup being resolved from a lookup, which is whilst pending lookups are locked.
                Core::deferred_invoke([this, promise, name] {
                    // Just go up one level, all is well.
                    auto upper_level_promise = validate_dnssec_chain_step(name.parent());
                    upper_level_promise->when_resolved([promise](bool valid) {
                        promise->resolve(move(valid));
                    });

                    upper_level_promise->when_rejected([promise](Error const& error) {
                        promise->reject(Error::copy(error));
                    });

                    promise->add_child(move(upper_level_promise));
                });
                return;
            }

            // NOTE: We have to defer here due to delegation_point_lookup being resolved from a lookup, which is whilst pending lookups are locked.
            Core::deferred_invoke([this, promise, name] {
                // This is a separate zone, let's look up the DS record.
                dbgln_if(DNS_DEBUG, "DNS: In separate zone, looking up DS record for {}", name.to_string());
                auto ds_lookup_promise = lookup(name.to_string().to_byte_string(), Messages::Class::IN, { Messages::ResourceType::DS }, { .validate_dnssec_locally = false });
                ds_lookup_promise->when_resolved([promise, name](NonnullRefPtr<LookupResult const> const& ds_result) {
                    if (!ds_result->has_record_of_type(Messages::ResourceType::DS)) {
                        // If there's no DS record, this is a misconfigured zone.
                        dbgln_if(DNS_DEBUG, "DNS: In separate zone, no DS record found for {}", name.to_string());
                        promise->resolve(false);
                        return;
                    }

                    dbgln_if(DNS_DEBUG, "DNS: In separate zone, DS record found for {}", name.to_string());
                    promise->resolve(true);
                });

                ds_lookup_promise->when_rejected([promise](Error const& error) {
                    promise->reject(Error::copy(error));
                });

                promise->add_child(move(ds_lookup_promise));
            });
        });

        delegation_point_lookup->when_rejected([promise](Error const& error) {
            promise->reject(Error::copy(error));
        });

        promise->add_child(move(delegation_point_lookup));
        return promise;
    }

    ErrorOr<void> validate_dnssec(Messages::Message message, PendingLookup& lookup, NonnullRefPtr<LookupResult> result)
    {
        struct RecordAndRRSIG {
            Vector<Messages::ResourceRecord> records;
            Messages::Records::RRSIG rrsig;
        };
        HashMap<Messages::ResourceType, RecordAndRRSIG> records_with_rrsigs;
        for (auto& record : message.answers) {
            if (record.type == Messages::ResourceType::RRSIG) {
                auto& rrsig = record.record.get<Messages::Records::RRSIG>();
                auto type = rrsig.type_covered;
                if (auto found = records_with_rrsigs.get(type); found.has_value())
                    found->rrsig = move(rrsig);
                else
                    records_with_rrsigs.set(type, { {}, move(rrsig) });
            } else {
                auto type = record.type;
                if (auto found = records_with_rrsigs.get(record.type); found.has_value())
                    found->records.append(move(record));
                else
                    records_with_rrsigs.set(type, { { move(record) }, {} });
            }
        }

        if (records_with_rrsigs.is_empty()) {
            dbgln_if(DNS_DEBUG, "DNS: No RRSIG records found in DNSSEC response");
            return {};
        }

        auto name = result->name();

        Core::deferred_invoke([this, lookup, name, records_with_rrsigs = move(records_with_rrsigs), result = move(result)] mutable {
            dbgln_if(DNS_DEBUG, "DNS: Resolving DNSKEY for {}", name.to_string());
            result->set_dnssec_validated(false); // Will be set to true if we successfully validate the RRSIGs.
            result->set_being_dnssec_validated(true);

            auto is_root_zone = lookup.parsed_name.labels.size() == 0;
            auto keys_promise = Core::Promise<Vector<Messages::Records::DNSKEY>>::construct();

            keys_promise->when_resolved([this, lookup, name, is_root_zone, records_with_rrsigs = move(records_with_rrsigs), result = move(result)](Vector<Messages::Records::DNSKEY> parent_zone_keys) {
                auto resolve_using_keys = [=, this, records_with_rrsigs = move(records_with_rrsigs)](Vector<Messages::Records::DNSKEY> keys) mutable {
                    dbgln_if(DNS_DEBUG, "DNS: Validating {} RRSIGs for {}; starting with {} keys", records_with_rrsigs.size(), name.to_string(), keys.size());
                    for (auto& key : keys)
                        dbgln_if(DNS_DEBUG, "- DNSKEY: {}", key.to_string());
                    Vector<NonnullRefPtr<Core::Promise<Empty>>> promises;

                    for (auto& record_and_rrsig : records_with_rrsigs) {
                        auto& records = record_and_rrsig.value.records;
                        if (record_and_rrsig.key == Messages::ResourceType::DNSKEY) {
                            for (auto& record : records)
                                keys.append(record.record.get<Messages::Records::DNSKEY>());
                        }
                    }

                    dbgln_if(DNS_DEBUG, "DNS: Found {} keys total", keys.size());

                    // (owner | type | class) -> (RRSet, RRSIG, DNSKey*)
                    HashMap<String, CanonicalizedRRSetWithRRSIG> rrsets_with_rrsigs;

                    for (auto& [type, pair] : records_with_rrsigs) {
                        auto& records = pair.records;
                        auto& rrsig = pair.rrsig;

                        for (auto& record : records) {
                            auto canonicalized_name = record.name.to_canonical_string();
                            auto key = MUST(String::formatted("{}|{}|{}", canonicalized_name, to_underlying(record.type), to_underlying(record.class_)));

                            if (!rrsets_with_rrsigs.contains(key)) {
                                auto dnskeys = [&] -> Vector<Messages::Records::DNSKEY> {
                                    Vector<Messages::Records::DNSKEY> relevant_keys;
                                    for (auto& key : keys) {
                                        if (key.algorithm == rrsig.algorithm)
                                            relevant_keys.append(key);
                                    }
                                    return relevant_keys;
                                }();
                                dbgln_if(DNS_DEBUG, "DNS: Found {} relevant DNSKEYs for key {}", dnskeys.size(), key);
                                rrsets_with_rrsigs.set(key, CanonicalizedRRSetWithRRSIG { {}, move(rrsig), move(dnskeys) });
                            }
                            auto& rrset_with_rrsig = *rrsets_with_rrsigs.get(key);
                            rrset_with_rrsig.rrset.append(move(record));
                        }
                    }

                    for (auto& entry : rrsets_with_rrsigs) {
                        auto& rrset_with_rrsig = entry.value;

                        if (rrset_with_rrsig.dnskeys.is_empty()) {
                            dbgln_if(DNS_DEBUG, "DNS: No DNSKEY found for validation of {} RRs", rrset_with_rrsig.rrset.size());
                            continue;
                        }

                        promises.append(validate_rrset_with_rrsig(move(rrset_with_rrsig), result));
                    }

                    auto promise = Core::Promise<Empty>::after(move(promises))
                                       ->when_resolved([result, lookup, keys = move(keys)](Empty) {
                                           for (auto& key : keys)
                                               result->add_dnskey(key);
                                           result->set_dnssec_validated(true);
                                           result->set_being_dnssec_validated(false);
                                           result->finished_request();
                                           lookup.promise->resolve(result);
                                       })
                                       .when_rejected([result, lookup](Error& error) {
                                           result->finished_request();
                                           result->set_being_dnssec_validated(false);
                                           lookup.promise->reject(move(error));
                                       })
                                       .map<NonnullRefPtr<LookupResult const>>([result](Empty&) { return result; });

                    lookup.promise = move(promise);
                };

                if (is_root_zone) {
                    resolve_using_keys(s_root_zone_dnskeys);
                    return;
                }

                // NOTE: We have to defer here due to keys_promises being resolved from a lookup, which is whilst pending lookups are locked.
                Core::deferred_invoke([this, lookup, name, parent_zone_keys = move(parent_zone_keys), resolve_using_keys = move(resolve_using_keys)] {
                    dbgln_if(DNS_DEBUG, "DNS: Starting DNSKEY lookup for {}", lookup.name);
                    this->lookup(lookup.name, Messages::Class::IN, { Messages::ResourceType::DNSKEY }, { .validate_dnssec_locally = false })
                        ->when_resolved([=](NonnullRefPtr<LookupResult const>& dnskey_lookup_result) mutable {
                            dbgln_if(DNS_DEBUG, "DNSKEY for {}:", name.to_string());
                            auto key_records = dnskey_lookup_result->records(Messages::ResourceType::DNSKEY);
                            for (auto& record : key_records)
                                dbgln_if(DNS_DEBUG, "- DNSKEY: {}", record.to_string());
                            Vector<Messages::Records::DNSKEY> keys;
                            keys.ensure_capacity(parent_zone_keys.size() + dnskey_lookup_result->records().size());
                            for (auto& record : parent_zone_keys)
                                keys.append(record);
                            for (auto& record : key_records)
                                keys.append(move(record.record).get<Messages::Records::DNSKEY>());
                            resolve_using_keys(move(keys));
                        })
                        .when_rejected([=](auto& error) mutable {
                            if (parent_zone_keys.is_empty()) {
                                dbgln_if(DNS_DEBUG, "Failed to resolve DNSKEY for {}: {}", name.to_string(), error);
                                lookup.promise->reject(move(error));
                                return;
                            }
                            resolve_using_keys(move(parent_zone_keys));
                        });
                });
            });

            keys_promise->when_rejected([lookup](Error const& error) {
                lookup.promise->reject(Error::copy(error));
            });

            if (!is_root_zone) {
                auto chain_valid_promise = validate_dnssec_chain_step(name, true);
                chain_valid_promise->when_resolved([this, lookup, keys_promise](bool valid) {
                    if (!valid) {
                        keys_promise->reject(Error::from_string_literal("DNSSEC chain is invalid"));
                        return;
                    }

                    // NOTE: We have to defer here due to chain_valid_promise being potentially resolved from a lookup, which is whilst pending lookups are locked.
                    Core::deferred_invoke([this, lookup, keys_promise] {
                        auto parent_result_promise = this->lookup(lookup.parsed_name.parent().to_string().to_byte_string(), Messages::Class::IN, { Messages::ResourceType::DNSKEY }, { .validate_dnssec_locally = true });
                        parent_result_promise->when_resolved([lookup, keys_promise](NonnullRefPtr<LookupResult const> const& parent_result) {
                            if (!parent_result->is_dnssec_validated()) {
                                keys_promise->reject(Error::from_string_literal("Parent zone is not DNSSEC validated"));
                                return;
                            }

                            Vector<Messages::Records::DNSKEY> parent_zone_keys = parent_result->used_dnskeys();
                            for (auto& rr : parent_result->records(Messages::ResourceType::DNSKEY))
                                parent_zone_keys.append(rr.record.get<Messages::Records::DNSKEY>());

                            dbgln("Found {} DNSKEYs for parent zone ({})", parent_zone_keys.size(), lookup.parsed_name.parent().to_string());
                            keys_promise->resolve(move(parent_zone_keys));
                        });

                        parent_result_promise->when_rejected([keys_promise](Error const& error) {
                            keys_promise->reject(Error::copy(error));
                        });

                        keys_promise->add_child(move(parent_result_promise));
                    });
                });

                chain_valid_promise->when_rejected([keys_promise](Error const& error) {
                    keys_promise->reject(Error::copy(error));
                });
            } else {
                keys_promise->resolve({});
            }

            lookup.promise->add_child(move(keys_promise));
        });

        return {};
    }

    Messages::Records::DNSKEY const* find_dnskey(CanonicalizedRRSetWithRRSIG const& rrset_with_rrsig)
    {
        for (auto& key : rrset_with_rrsig.dnskeys) {
            if (key.calculated_key_tag == rrset_with_rrsig.rrsig.key_tag)
                return &key;
            dbgln_if(DNS_DEBUG, "DNS: DNSKEY with tag {} does not match RRSIG with tag {}", key.calculated_key_tag, rrset_with_rrsig.rrsig.key_tag);
        }
        return nullptr;
    }

    NonnullRefPtr<Core::Promise<Empty>> validate_rrset_with_rrsig(CanonicalizedRRSetWithRRSIG rrset_with_rrsig, NonnullRefPtr<LookupResult> result)
    {
        auto promise = Core::Promise<Empty>::construct();
        auto& rrsig = rrset_with_rrsig.rrsig;

        Vector<ByteBuffer> canon_encoded_rrs;
        auto total_size = 0uz;
        for (auto& rr : rrset_with_rrsig.rrset) {
            rr.ttl = rrsig.original_ttl;
            canon_encoded_rrs.empend();
            auto& canon_encoded_rr = canon_encoded_rrs.last();
            TRY_OR_REJECT_PROMISE(promise, rr.to_raw(canon_encoded_rr));
            total_size += canon_encoded_rr.size();
        }
        quick_sort(canon_encoded_rrs, [](auto const& a, auto const& b) {
            return memcmp(a.data(), b.data(), min(a.size(), b.size())) < 0;
        });

        ByteBuffer canon_encoded;
        TRY_OR_REJECT_PROMISE(promise, canon_encoded.try_ensure_capacity(total_size));
        for (auto& rr : canon_encoded_rrs)
            canon_encoded.append(rr);

        auto& dnskey = *find_dnskey(rrset_with_rrsig);

        if constexpr (DNS_DEBUG) {
            dbgln("Validating RRSet with RRSIG for {}", result->name().to_string());
            for (auto& rr : rrset_with_rrsig.rrset)
                dbgln("- RR {}", rr.to_string());
            for (auto& canon : canon_encoded_rrs) {
                FixedMemoryStream stream { canon.bytes() };
                CountingStream rr_counting_stream { MaybeOwned<Stream>(stream) };
                DNS::Messages::ParseContext rr_ctx { rr_counting_stream, make<RedBlackTree<u16, Messages::DomainName>>() };
                auto maybe_decoded = Messages::ResourceRecord::from_raw(rr_ctx);
                if (maybe_decoded.is_error())
                    dbgln("-- Failed to decode RR: {}", maybe_decoded.error());
                else
                    dbgln("-- Canon encoded (decoded): {}", maybe_decoded.value().to_string());
            }
            dbgln("- DNSKEY {}", dnskey.to_string());
            dbgln("- RRSIG {}", rrsig.to_string());
        }

        ByteBuffer to_be_signed;
        {
            //  2 bytes: type_covered
            //  1 byte : algorithm
            //  1 byte : labels
            //  4 bytes: original_ttl
            //  4 bytes: signature_expiration
            //  4 bytes: signature_inception
            //  2 bytes: key_tag
            //  (wire-format encoded signer name)
            to_be_signed = TRY_OR_REJECT_PROMISE(promise, ByteBuffer::create_uninitialized(2 + 1 + 1 + 4 + 4 + 4 + 2));

            auto write_u16_be = [&](size_t offset, u16 value) {
                to_be_signed.bytes()[offset + 0] = (value >> 8) & 0xff;
                to_be_signed.bytes()[offset + 1] = (value >> 0) & 0xff;
            };
            auto write_u32_be = [&](size_t offset, u32 value) {
                to_be_signed.bytes()[offset + 0] = (value >> 24) & 0xff;
                to_be_signed.bytes()[offset + 1] = (value >> 16) & 0xff;
                to_be_signed.bytes()[offset + 2] = (value >> 8) & 0xff;
                to_be_signed.bytes()[offset + 3] = (value >> 0) & 0xff;
            };

            size_t offset = 0;
            write_u16_be(offset, to_underlying(rrsig.type_covered));
            offset += 2;
            to_be_signed[offset++] = static_cast<u8>(rrsig.algorithm);
            to_be_signed[offset++] = rrsig.label_count;
            write_u32_be(offset, rrsig.original_ttl);
            offset += 4;
            write_u32_be(offset, rrsig.expiration.seconds_since_epoch());
            offset += 4;
            write_u32_be(offset, rrsig.inception.seconds_since_epoch());
            offset += 4;
            write_u16_be(offset, rrsig.key_tag);
        }

        TRY_OR_REJECT_PROMISE(promise, rrsig.signers_name.to_raw(to_be_signed));
        TRY_OR_REJECT_PROMISE(promise, to_be_signed.try_append(canon_encoded.data(), canon_encoded.size()));

        dbgln_if(DNS_DEBUG, "To be signed: {:hex-dump}", to_be_signed.bytes());

        switch (dnskey.algorithm) {
        case Messages::DNSSEC::Algorithm::RSAMD5: {
            auto md5 = Crypto::Hash::MD5::create();
            md5->update(to_be_signed.data(), to_be_signed.size());
            auto digest = md5->digest();

            auto public_key = TRY_OR_REJECT_PROMISE(promise, Crypto::PK::RSA::parse_rsa_key(dnskey.public_key, false, {}));

            auto const& signature_data = rrsig.signature; // ByteBuffer with raw RSA/MD5 signature
            if (signature_data.is_empty()) {
                promise->reject(Error::from_string_literal("RRSIG has an empty signature"));
                return promise;
            }

            Crypto::PK::RSA_PKCS1_EME rsa { public_key };
            if (auto const ok = TRY_OR_REJECT_PROMISE(promise, rsa.verify(digest.bytes(), signature_data)); !ok) {
                promise->reject(Error::from_string_literal("RSA/MD5 signature validation failed"));
                return promise;
            }

            break;
        }
        case Messages::DNSSEC::Algorithm::ECDSAP256SHA256: {
            auto sha256 = Crypto::Hash::SHA256::hash(to_be_signed);
            auto keys = TRY_OR_REJECT_PROMISE(promise, Crypto::PK::EC::parse_ec_key(dnskey.public_key, false, {}));
            auto signature = TRY_OR_REJECT_PROMISE(promise, Crypto::Curves::SECPxxxr1Signature::from_raw(Crypto::ASN1::secp256r1_oid, rrsig.signature));
            Crypto::Curves::SECP256r1 curve;
            if (auto ok = TRY_OR_REJECT_PROMISE(promise, curve.verify(sha256.bytes(), keys.public_key.to_secpxxxr1_point(), signature)); !ok) {
                promise->reject(Error::from_string_literal("ECDSA/SHA256 signature validation failed"));
                return promise;
            }
            break;
        }
        case Messages::DNSSEC::Algorithm::ECDSAP384SHA384: {
            auto sha384 = Crypto::Hash::SHA384::hash(to_be_signed);
            auto keys = TRY_OR_REJECT_PROMISE(promise, Crypto::PK::EC::parse_ec_key(dnskey.public_key, false, {}));
            auto signature = TRY_OR_REJECT_PROMISE(promise, Crypto::Curves::SECPxxxr1Signature::from_raw(Crypto::ASN1::secp384r1_oid, rrsig.signature));
            Crypto::Curves::SECP384r1 curve;
            if (auto ok = TRY_OR_REJECT_PROMISE(promise, curve.verify(sha384.bytes(), keys.public_key.to_secpxxxr1_point(), signature)); !ok) {
                promise->reject(Error::from_string_literal("ECDSA/SHA384 signature validation failed"));
                return promise;
            }
            break;
        }
        case Messages::DNSSEC::Algorithm::RSASHA512: {
            auto n = Crypto::UnsignedBigInteger::import_data(dnskey.public_key_rsa_modulus());
            auto e = Crypto::UnsignedBigInteger::import_data(dnskey.public_key_rsa_exponent());
            Crypto::PK::RSA_PKCS1_EMSA rsa { Crypto::Hash::HashKind::SHA512, Crypto::PK::RSAPublicKey { move(n), move(e) } };
            if (auto ok = TRY_OR_REJECT_PROMISE(promise, rsa.verify(to_be_signed, rrsig.signature)); !ok) {
                promise->reject(Error::from_string_literal("RSA/SHA512 signature validation failed"));
                return promise;
            }
            break;
        }
        case Messages::DNSSEC::Algorithm::RSASHA1: {
            auto n = Crypto::UnsignedBigInteger::import_data(dnskey.public_key_rsa_modulus());
            auto e = Crypto::UnsignedBigInteger::import_data(dnskey.public_key_rsa_exponent());
            Crypto::PK::RSA_PKCS1_EMSA rsa { Crypto::Hash::HashKind::SHA1, Crypto::PK::RSAPublicKey { move(n), move(e) } };
            if (auto ok = TRY_OR_REJECT_PROMISE(promise, rsa.verify(to_be_signed, rrsig.signature)); !ok) {
                promise->reject(Error::from_string_literal("RSA/SHA1 signature validation failed"));
                return promise;
            }
            break;
        }
        case Messages::DNSSEC::Algorithm::RSASHA256: {
            auto n = Crypto::UnsignedBigInteger::import_data(dnskey.public_key_rsa_modulus());
            auto e = Crypto::UnsignedBigInteger::import_data(dnskey.public_key_rsa_exponent());
            Crypto::PK::RSA_PKCS1_EMSA rsa { Crypto::Hash::HashKind::SHA256, Crypto::PK::RSAPublicKey { move(n), move(e) } };
            if (auto ok = TRY_OR_REJECT_PROMISE(promise, rsa.verify(to_be_signed, rrsig.signature)); !ok) {
                promise->reject(Error::from_string_literal("RSA/SHA256 signature validation failed"));
                return promise;
            }
            break;
        }
        case Messages::DNSSEC::Algorithm::ED25519: {
            Crypto::Curves::Ed25519 ed25519;
            if (!TRY_OR_REJECT_PROMISE(promise, ed25519.verify(dnskey.public_key.bytes(), rrsig.signature.bytes(), to_be_signed.bytes()))) {
                promise->reject(Error::from_string_literal("ED25519 signature validation failed"));
                return promise;
            }
            break;
        }
        case Messages::DNSSEC::Algorithm::DSA:
        case Messages::DNSSEC::Algorithm::RSASHA1NSEC3SHA1:
            // Not implemented yet.
        case Messages::DNSSEC::Algorithm::Unknown:
            dbgln("DNS: Unsupported algorithm for DNSSEC validation: {}", to_string(dnskey.algorithm));
            promise->reject(Error::from_string_literal("Unsupported algorithm for DNSSEC validation"));
            break;
        }

        // If we haven't rejected by now, we consider the RRSet valid.
        if (!promise->is_rejected()) {
            // Typically you'd store these validated RRs in the lookup result.
            for (auto& record : rrset_with_rrsig.rrset)
                result->add_record(move(record));

            // Resolve with an empty success.
            promise->resolve({});
        }

        return promise;
    }

    NonnullRefPtr<Core::Promise<bool>> has_connection(bool attempt_restart = true)
    {
        auto promise = Core::Promise<bool>::construct();

        auto result = m_socket.with_read_locked(
            [&](auto& socket) { return socket.has_value() && (*socket)->is_open(); });

        if (attempt_restart && !result && !m_attempting_restart) {
            m_attempting_restart = true;

            auto create_socket_promise = m_create_socket();
            create_socket_promise->when_resolved([this, promise](SocketResult& result) {
                m_attempting_restart = false;
                set_socket(move(result.socket), result.mode);
                promise->resolve(true);
            });

            create_socket_promise->when_rejected([this, promise](Error const& error) {
                dbgln_if(DNS_DEBUG, "DNS: Failed to create socket: {}", error);
                m_attempting_restart = false;
                promise->resolve(false);
            });

            promise->add_child(move(create_socket_promise));
        } else {
            promise->resolve(move(result));
        }

        return promise;
    }

    void set_socket(MaybeOwned<Core::Socket> socket, ConnectionMode mode = ConnectionMode::UDP)
    {
        m_mode = mode;
        m_socket.with_write_locked([&](auto& s) {
            s = move(socket);
            (*s)->on_ready_to_read = [this] {
                process_incoming_messages();
            };
            (*s)->set_notifications_enabled(true);
        });
    }

    void flush_cache()
    {
        m_cache.with_write_locked([&](auto& cache) {
            HashTable<ByteString> to_remove;
            for (auto& entry : cache) {
                entry.value->check_expiration();
                if (entry.value->can_be_removed())
                    to_remove.set(entry.key);
            }
            for (auto const& key : to_remove)
                cache.remove(key);
        });
    }

    Threading::RWLockProtected<HashMap<ByteString, NonnullRefPtr<LookupResult>>> m_cache;
    Threading::RWLockProtected<NonnullOwnPtr<RedBlackTree<u16, PendingLookup>>> m_pending_lookups;
    Threading::RWLockProtected<Optional<MaybeOwned<Core::Socket>>> m_socket;
    CreateSocketFunction m_create_socket;
    bool m_attempting_restart { false };
    ConnectionMode m_mode { ConnectionMode::UDP };
};

}

#undef TRY_OR_REJECT_PROMISE
