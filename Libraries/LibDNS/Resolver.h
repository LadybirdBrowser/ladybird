/*
 * Copyright (c) 2024, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/HashTable.h>
#include <AK/MaybeOwned.h>
#include <AK/MemoryStream.h>
#include <AK/RWLockProtected.h>
#include <AK/Random.h>
#include <AK/StringView.h>
#include <AK/TemporaryChange.h>
#include <AK/Time.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Promise.h>
#include <LibCore/Socket.h>
#include <LibCore/Timer.h>
#include <LibDNS/Message.h>
#include <LibDNS/Validator.h>
#include <LibThreading/ThreadPool.h>

namespace DNS {

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
        for (auto const& re : m_cached_records) {
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
            if (record.expiration < now) {
                dbgln_if(DNS_DEBUG, "DNS: Removing expired record for {}", m_name.to_string());
                m_cached_records.remove(i);
            } else {
                ++i;
            }
        }

        if (m_cached_records.is_empty() && m_request_done)
            m_valid = false;
    }

    void add_record(Messages::ResourceRecord record, AK::UnixDateTime received_at = AK::UnixDateTime::now())
    {
        m_valid = true;
        auto expiration = received_at + AK::Duration::from_seconds(record.ttl);
        m_cached_records.append({ move(record), expiration });
    }

    Vector<Messages::ResourceRecord> records() const
    {
        Vector<Messages::ResourceRecord> result;
        result.ensure_capacity(m_cached_records.size());
        for (auto const& re : m_cached_records)
            result.unchecked_append(re.record);
        return result;
    }

    Vector<Messages::ResourceRecord> records(Messages::ResourceType type) const
    {
        Vector<Messages::ResourceRecord> result;
        for (auto const& re : m_cached_records) {
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

    bool has_record_of_type(Messages::ResourceType type) const
    {
        for (auto const& re : m_cached_records) {
            if (re.record.type == type)
                return true;
        }
        return false;
    }

    // Whether a completed lookup asked for this type, so having no records of it is a (negative) answer too.
    bool answers_type(Messages::ResourceType type) const
    {
        return has_record_of_type(type) || (m_request_done && m_queried_types.contains(type));
    }

    void will_add_record_of_type(Messages::ResourceType type) { m_queried_types.set(type); }
    void finished_request() { m_request_done = true; }

    bool can_be_removed() const { return !m_valid && m_request_done; }
    bool is_done() const { return m_request_done; }
    bool is_empty() const { return m_cached_records.is_empty(); }
    Messages::DomainName const& name() const { return m_name; }

    DNSSEC::SecurityStatus security_status() const { return m_security_status; }
    void set_security_status(DNSSEC::SecurityStatus status)
    {
        m_validation_attempted = true;
        m_security_status = status;
    }
    bool is_dnssec_validated() const { return m_security_status == DNSSEC::SecurityStatus::Secure; }
    // Whether DNSSEC validation was attempted, with the result being Secure or Insecure.
    bool was_dnssec_validated() const { return m_validation_attempted; }

private:
    bool m_valid { false };
    bool m_request_done { false };
    bool m_validation_attempted { false };
    DNSSEC::SecurityStatus m_security_status { DNSSEC::SecurityStatus::Insecure };
    Messages::DomainName m_name;

    struct RecordWithExpiration {
        Messages::ResourceRecord record;
        AK::UnixDateTime expiration;
    };

    Vector<RecordWithExpiration> m_cached_records;
    HashTable<Messages::ResourceType> m_queried_types;
};

class Resolver {
    AK_MAKE_NONCOPYABLE(Resolver);
    AK_MAKE_NONMOVABLE(Resolver);

public:
    enum class ConnectionMode {
        TCP,
        UDP,
    };

    struct LookupOptions {
        bool validate_dnssec_locally { false };

        static LookupOptions default_() { return {}; }
    };

    struct SocketResult {
        MaybeOwned<Core::Socket> socket;
        ConnectionMode mode;
    };

    using ResultPromise = Core::Promise<NonnullRefPtr<LookupResult const>>;
    using MessagePromise = Core::Promise<Messages::Message>;

    // An empty result means no server is configured and the system resolver should be used instead.
    Resolver(Function<ErrorOr<Optional<SocketResult>>()> create_socket)
        : m_receive_buffer(MUST(ByteBuffer::create_uninitialized(NumericLimits<u16>::max())))
        , m_create_socket(move(create_socket))
        , m_validator([this](Messages::DomainName const& name, Messages::ResourceType type) {
            return query(name, type, Messages::Class::IN, true);
        })
    {
    }

    DNSSEC::Validator& validator() { return m_validator; }

    NonnullRefPtr<Core::Promise<Empty>> when_socket_ready()
    {
        auto promise = Core::Promise<Empty>::construct();
        m_socket_ready_promises.append(promise);
        if (has_connection(false)) {
            promise->resolve({});
            return promise;
        }

        if (!has_connection())
            promise->reject(Error::from_string_literal("Failed to create socket"));

        return promise;
    }

    void reset_connection()
    {
        m_socket.with_write_locked([&](auto& socket) { socket = {}; });
        m_incoming.clear();
        auto in_flight = move(m_in_flight);
        m_in_flight_by_id.clear();
        for (auto& entry : in_flight)
            entry.value->reject(Error::from_string_literal("DNS connection was reset"));
        m_cache.with_write_locked([&](auto& cache) { cache.clear(); });
        m_validator.clear_caches();
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
            result.check_expiration();
            if (result.can_be_removed())
                return {};

            for (auto const& type : desired_types) {
                if (!result.answers_type(type))
                    return {};
            }

            return result;
        });
    }

    NonnullRefPtr<ResultPromise> lookup(ByteString name, Messages::Class class_, Vector<Vector<Messages::ResourceType>> desired_types, LookupOptions options = LookupOptions::default_())
    {
        Vector<NonnullRefPtr<ResultPromise>> promises;
        promises.ensure_capacity(desired_types.size());

        for (auto& types : desired_types)
            promises.unchecked_append(lookup(name, class_, types, options));

        auto result_promise = ResultPromise::construct();
        result_promise->add_child(Core::Promise<Empty>::after(promises)
                ->when_resolved([promises, result_promise = result_promise->make_weak_ptr<ResultPromise>()](auto&&) {
                    if (!result_promise.ptr())
                        return;
                    VERIFY(promises.first()->is_resolved());
                    result_promise->resolve(MUST(promises.first()->await()));
                })
                .when_rejected([promises, result_promise = result_promise->make_weak_ptr<ResultPromise>()](auto&& error) {
                    if (!result_promise.ptr())
                        return;
                    for (auto& promise : promises) {
                        if (promise->is_resolved()) {
                            result_promise->resolve(MUST(promise->await()));
                            return;
                        }
                    }
                    result_promise->reject(move(error));
                }));
        return result_promise;
    }

    NonnullRefPtr<ResultPromise> lookup(ByteString name, Messages::Class class_ = Messages::Class::IN, LookupOptions options = LookupOptions::default_())
    {
        return lookup(move(name), class_, { Messages::ResourceType::A, Messages::ResourceType::AAAA }, options);
    }

    NonnullRefPtr<ResultPromise> lookup(ByteString name, Messages::Class class_, Vector<Messages::ResourceType> desired_types, LookupOptions options = LookupOptions::default_())
    {
        // Classifies how the lookup was satisfied (cache hit, system-resolver fallback, async query) so the
        // `wire-dns:` log line below can show where event-loop time went when the synchronous portion is slow.
        StringView lookup_path = "unknown"sv;
        Optional<MonotonicTime> lookup_entered_at;
        if constexpr (REQUESTSERVER_WIRE_DEBUG)
            lookup_entered_at = MonotonicTime::now();
        ScopeGuard log_guard = [&] {
            if constexpr (!REQUESTSERVER_WIRE_DEBUG)
                return;
            auto sync_ms = (MonotonicTime::now() - *lookup_entered_at).to_milliseconds();
            if (sync_ms > 5)
                dbgln("LibDNS wire-dns: lookup({}) path={} sync={} ms", name, lookup_path, sync_ms);
        };

        auto promise = ResultPromise::construct();

        if (auto maybe_ipv4 = IPv4Address::from_string(name); maybe_ipv4.has_value()) {
            dbgln_if(DNS_DEBUG, "DNS: Resolving {} as IPv4", name);
            if (desired_types.contains_slow(Messages::ResourceType::A)) {
                auto result = make_ref_counted<LookupResult>(Messages::DomainName {});
                result->add_record({ .name = {}, .type = Messages::ResourceType::A, .class_ = Messages::Class::IN, .ttl = 0, .record = Messages::Records::A { maybe_ipv4.release_value() }, .raw = {} });
                result->finished_request();
                promise->resolve(move(result));
                lookup_path = "literal-ipv4"sv;
                return promise;
            }
        }

        if (auto maybe_ipv6 = IPv6Address::from_string(name); maybe_ipv6.has_value()) {
            dbgln_if(DNS_DEBUG, "DNS: Resolving {} as IPv6", name);
            if (desired_types.contains_slow(Messages::ResourceType::AAAA)) {
                auto result = make_ref_counted<LookupResult>(Messages::DomainName {});
                result->add_record({ .name = {}, .type = Messages::ResourceType::AAAA, .class_ = Messages::Class::IN, .ttl = 0, .record = Messages::Records::AAAA { maybe_ipv6.release_value() }, .raw = {} });
                result->finished_request();
                promise->resolve(move(result));
                lookup_path = "literal-ipv6"sv;
                return promise;
            }
        }

        // RFC 1034, 3.1. Name space specifications and terminology.
        // Since a complete domain name ends with the root label, this leads to a printed form which ends in a dot.
        if (name.ends_with('.'))
            name = name.substring(0, name.length() - 1);

        // Anything still ending in a dot has an empty label, and must not slip past the suffix checks below.
        if (name.ends_with('.')) {
            promise->reject(Error::from_string_literal("Invalid domain name"));
            lookup_path = "invalid-name"sv;
            return promise;
        }

        auto domain_name = Messages::DomainName::from_string(name);

        // https://www.rfc-editor.org/rfc/rfc6761#section-6.3
        // "localhost" and names within ".localhost" resolve to loopback for address queries and are never sent
        // upstream; we answer in-process since the host resolver and upstream server are not guaranteed to.
        if (is_within_domain(name, "localhost"sv)) {
            dbgln_if(DNS_DEBUG, "DNS: Resolving {} as loopback", name);
            auto result = make_ref_counted<LookupResult>(domain_name);
            if (desired_types.contains_slow(Messages::ResourceType::A))
                result->add_record({ .name = {}, .type = Messages::ResourceType::A, .class_ = Messages::Class::IN, .ttl = 0, .record = Messages::Records::A { IPv4Address { 127, 0, 0, 1 } }, .raw = {} });
            if (desired_types.contains_slow(Messages::ResourceType::AAAA))
                result->add_record({ .name = {}, .type = Messages::ResourceType::AAAA, .class_ = Messages::Class::IN, .ttl = 0, .record = Messages::Records::AAAA { IPv6Address::loopback() }, .raw = {} });
            result->finished_request();
            promise->resolve(move(result));
            lookup_path = "localhost-loopback"sv;
            return promise;
        }

        // Checked after the localhost names above, since those never go on the wire and URL hosts may have longer labels.
        if (!domain_name.is_valid()) {
            promise->reject(Error::from_string_literal("Invalid domain name"));
            lookup_path = "invalid-name"sv;
            return promise;
        }

        // RFC 7686, 2. The ".onion" Special-Use Domain Name.
        // Applications that do not implement the Tor protocol SHOULD generate an error upon the use of .onion and
        // SHOULD NOT perform a DNS lookup.
        if (is_within_domain(name, "onion"sv)) {
            promise->reject(Error::from_string_literal("Refusing to resolve a .onion name"));
            lookup_path = "onion-rejected"sv;
            return promise;
        }

        // RFC 6762, 3. Multicast DNS Names.
        // Any DNS query for a name ending with ".local." MUST be sent to the mDNS IPv4 link-local multicast address
        // 224.0.0.251 (or its IPv6 equivalent FF02::FB).
        // The system resolver takes care of that, so these never go to the configured server.
        auto is_link_local_name = is_within_domain(name, "local"sv);

        if (auto result = lookup_in_cache(name, class_, desired_types)) {
            dbgln_if(DNS_DEBUG, "DNS: Resolving {} from cache...", name);
            if (!options.validate_dnssec_locally || result->was_dnssec_validated()) {
                dbgln_if(DNS_DEBUG, "DNS: Resolved {} from cache", name);
                promise->resolve(result.release_nonnull());
                lookup_path = "cache-hit"sv;
                return promise;
            }
            dbgln_if(DNS_DEBUG, "DNS: Cache entry for {} is not DNSSEC validated (and we expect that), re-resolving", name);
        }

        if (is_link_local_name || !has_connection()) {
            if (!is_link_local_name && !m_use_system_resolver) {
                promise->reject(Error::from_string_literal("No connection to the configured DNS server"));
                lookup_path = "no-conn-rejected"sv;
                return promise;
            }

            if (!is_link_local_name && options.validate_dnssec_locally) {
                promise->reject(Error::from_string_literal("No connection available to validate DNSSEC"));
                lookup_path = "no-conn-dnssec-rejected"sv;
                return promise;
            }

            // FIXME: Use an underlying async resolver instead of getaddrinfo entirely. Until then, see
            //        PendingSystemResolution for why we split into two parallel workers.
            dbgln_if(DNS_DEBUG, "Not ready to resolve, dispatching system resolver to ThreadPool for {}", name);

            RefPtr<PendingSystemResolution> our_state;
            RefPtr<LookupResult> already_finalized_result;
            m_pending_system_resolutions.with_write_locked(
                [&](auto& pending) {
                    if (auto it = pending.find(name); it != pending.end()) {
                        auto& existing = *it->value;
                        // Grace timer may have already finalized the resolution while AAAA is still in
                        // flight; serve the join-pending caller from the existing result. We resolve the
                        // promise outside this critical section so a synchronous handler can't reenter
                        // the resolver and deadlock on m_pending_system_resolutions.
                        if (existing.promise_resolved) {
                            already_finalized_result = existing.result;
                            return;
                        }
                        existing.waiting_promises.append(promise);
                        return;
                    }
                    auto result = make_ref_counted<LookupResult>(domain_name);
                    for (auto const& type : desired_types)
                        result->will_add_record_of_type(type);
                    auto state = adopt_ref(*new PendingSystemResolution(result));
                    state->waiting_promises.append(promise);
                    pending.set(name, state);
                    our_state = state;
                });

            if (already_finalized_result) {
                lookup_path = "system-resolver-join-finalized"sv;
                if (already_finalized_result->records().is_empty())
                    promise->reject(Error::from_string_literal("Could not resolve to IPv4 or IPv6 address"));
                else
                    promise->resolve(already_finalized_result.release_nonnull());
                return promise;
            }

            if (!our_state) {
                lookup_path = "system-resolver-join-pending"sv;
                return promise;
            }

            lookup_path = "system-resolver-bg"sv;

            auto& main_thread_event_loop = Core::EventLoop::current();

            auto submit_worker = [&, this](Core::Socket::AddressFamily family) {
                Threading::ThreadPool::the().submit(
                    [this, name, state = our_state, family,
                        &main_thread_event_loop]() mutable {
                        auto worker_started_at = MonotonicTime::now();
                        auto record_or_error = Core::Socket::resolve_host(name, Core::Socket::SocketType::Stream, family);
                        auto worker_finished_at = MonotonicTime::now();
                        PendingSystemResolution::SideTiming timing {
                            .queue_ms = (worker_started_at - state->dispatched_at).to_milliseconds(),
                            .work_ms = (worker_finished_at - worker_started_at).to_milliseconds(),
                        };

                        main_thread_event_loop.deferred_invoke(
                            [this, name, state, family,
                                record_or_error = move(record_or_error),
                                timing]() mutable {
                                handle_system_resolver_completion(name, *state, family, move(record_or_error), timing);
                            });
                    });
            };

            submit_worker(Core::Socket::AddressFamily::IPv4Only);
            submit_worker(Core::Socket::AddressFamily::IPv6Only);

            return promise;
        }

        lookup_path = "async-query"sv;

        // Every caller for the same name, types and validation policy shares one lookup; the first one runs it.
        StringBuilder key_builder;
        key_builder.appendff("{}|{}|{}", name, to_underlying(class_), options.validate_dnssec_locally);
        for (auto type : desired_types)
            key_builder.appendff("|{}", to_underlying(type));
        auto key = key_builder.to_byte_string();

        if (auto pending = m_pending_lookups.get(key); pending.has_value()) {
            pending->waiters.append(promise);
            lookup_path = "join-pending"sv;
            return promise;
        }

        auto validate = options.validate_dnssec_locally;
        Vector<NonnullRefPtr<MessagePromise>> query_promises;
        for (auto type : desired_types)
            query_promises.append(query(domain_name, type, class_, validate));

        auto settle = [this, key](ErrorOr<NonnullRefPtr<LookupResult const>> result) {
            auto pending = m_pending_lookups.take(key);
            if (!pending.has_value())
                return;
            for (auto& waiter : pending->waiters) {
                if (result.is_error())
                    waiter->reject(Error::copy(result.error()));
                else
                    waiter->resolve(result.value());
            }
        };

        auto all_answered = Core::Promise<Empty>::after(query_promises);
        all_answered->when_resolved([this, settle, name, domain_name, class_, desired_types, validate, query_promises](Empty&) {
                        Vector<Messages::Message> responses;
                        for (auto& query_promise : query_promises)
                            responses.append(MUST(query_promise->await()));
                        settle(finish_lookup(name, domain_name, class_, desired_types, validate, move(responses)));
                    })
            .when_rejected([settle](Error& error) {
                settle(Error::copy(error));
            });

        m_pending_lookups.set(key, { { promise }, move(all_answered) });
        return promise;
    }

    // Sends one question and hands back the whole response. Identical questions in flight share one transaction.
    NonnullRefPtr<MessagePromise> query(Messages::DomainName const& name, Messages::ResourceType type, Messages::Class class_, bool dnssec_ok)
    {
        auto promise = MessagePromise::construct();
        auto key = ByteString::formatted("{}|{}|{}|{}", name.to_canonical_string(), to_underlying(type), to_underlying(class_), dnssec_ok);

        if (auto existing = m_in_flight.get(key); existing.has_value()) {
            (*existing)->waiters.append(promise);
            return promise;
        }

        u16 id = 0;
        do
            fill_with_random({ &id, sizeof(id) });
        while (m_in_flight_by_id.contains(id));

        auto in_flight = make<InFlightQuery>();
        in_flight->id = id;
        in_flight->key = key;
        in_flight->question = { name, type, class_ };
        in_flight->dnssec_ok = dnssec_ok;
        in_flight->waiters.append(promise);
        in_flight->retry_timer = Core::Timer::create_single_shot(query_retry_interval_ms, [this, id] { retry_query(id); });
        auto* raw = in_flight.ptr();
        m_in_flight.set(key, move(in_flight));
        m_in_flight_by_id.set(id, key);

        if (auto sent = send_query(*raw); sent.is_error())
            settle_query(*raw, sent.release_error());

        return promise;
    }

private:
    // RFC 4343, Abstract.
    // Domain Name System (DNS) names are "case insensitive".
    static bool is_within_domain(StringView name, StringView domain)
    {
        if (name.equals_ignoring_ascii_case(domain))
            return true;
        return name.length() > domain.length()
            && name[name.length() - domain.length() - 1] == '.'
            && name.ends_with(domain, CaseSensitivity::CaseInsensitive);
    }

    static constexpr int query_retry_interval_ms = 1000;
    static constexpr size_t query_max_attempts = 5;
    static constexpr size_t MaxCacheEntries = 4096;

    struct InFlightQuery {
        AK_ALLOC_WITH_KMALLOC;

        u16 id { 0 };
        ByteString key;
        Messages::Question question;
        bool dnssec_ok { false };
        size_t attempts { 0 };
        Vector<NonnullRefPtr<MessagePromise>> waiters;
        RefPtr<Core::Timer> retry_timer;

        void reject(Error error)
        {
            retry_timer->stop();
            for (auto& waiter : waiters)
                waiter->reject(Error::copy(error));
        }
    };

    ErrorOr<void> send_query(InFlightQuery& in_flight)
    {
        if (in_flight.attempts >= query_max_attempts)
            return Error::from_string_literal("DNS lookup timed out");
        ++in_flight.attempts;

        if (!has_connection())
            return Error::from_string_literal("No connection to the configured DNS server");

        Messages::Message query;
        query.header.id = in_flight.id;
        query.header.question_count = 1;
        query.header.options.set_response_code(Messages::Options::ResponseCode::NoError);
        query.header.options.set_recursion_desired(true);
        query.header.options.set_op_code(Messages::OpCode::Query);
        query.questions.append(in_flight.question);

        if (in_flight.dnssec_ok) {
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
                .name = Messages::DomainName {},
                .type = Messages::ResourceType::OPT,
                .class_ = in_flight.question.class_,
                .ttl = 0,
                .record = move(opt),
                .raw = {},
            });
        }

        ByteBuffer query_bytes;
        TRY(query.to_raw(query_bytes));

        if (m_mode == ConnectionMode::TCP) {
            // RFC 1035, 4.2.2. TCP usage.
            // The message is prefixed with a two byte length field which gives the message length, excluding the two
            // byte length field.
            if (query_bytes.size() > NumericLimits<u16>::max())
                return Error::from_string_literal("DNS query is too large for a TCP frame");
            auto original_query_bytes = query_bytes;
            query_bytes = TRY(ByteBuffer::create_uninitialized(query_bytes.size() + sizeof(u16)));
            NetworkOrdered<u16> size = original_query_bytes.size();
            query_bytes.overwrite(0, &size, sizeof(size));
            query_bytes.overwrite(sizeof(size), original_query_bytes.data(), original_query_bytes.size());
        }

        TRY(m_socket.with_write_locked([&](auto& socket) {
            return (*socket)->write_until_depleted(query_bytes.bytes());
        }));

        in_flight.retry_timer->start();
        return {};
    }

    void retry_query(u16 id)
    {
        auto key = m_in_flight_by_id.get(id);
        if (!key.has_value())
            return;
        auto in_flight = m_in_flight.get(*key);
        if (!in_flight.has_value())
            return;
        if (auto sent = send_query(**in_flight); sent.is_error())
            settle_query(**in_flight, sent.release_error());
    }

    void settle_query(InFlightQuery& in_flight, ErrorOr<Messages::Message> result)
    {
        in_flight.retry_timer->stop();
        m_in_flight_by_id.remove(in_flight.id);
        auto owned = m_in_flight.take(in_flight.key);
        if (!owned.has_value())
            return;
        // Waiters run from the top of the event loop, not from inside the socket callback that delivered the message.
        Core::deferred_invoke([waiters = move((*owned)->waiters), result = move(result)] mutable {
            for (auto& waiter : waiters) {
                if (result.is_error())
                    waiter->reject(Error::copy(result.error()));
                else
                    waiter->resolve(Messages::Message(result.value()));
            }
        });
    }

    ErrorOr<NonnullRefPtr<LookupResult const>> finish_lookup(ByteString const& name, Messages::DomainName const& domain_name, Messages::Class class_, Vector<Messages::ResourceType> const& desired_types, bool validate, Vector<Messages::Message> responses)
    {
        auto received_at = AK::UnixDateTime::now();
        auto result = make_ref_counted<LookupResult>(domain_name);
        auto status = DNSSEC::SecurityStatus::Secure;

        for (size_t i = 0; i < desired_types.size(); ++i) {
            auto& response = responses[i];
            Messages::Question question { domain_name, desired_types[i], class_ };
            result->will_add_record_of_type(desired_types[i]);

            if (validate) {
                auto validation = TRY(m_validator.validate(response, question));
                if (validation.status == DNSSEC::SecurityStatus::Bogus) {
                    dbgln("DNS: DNSSEC validation of {} {} failed: {}", name, Messages::to_string(desired_types[i]), validation.reason);
                    return Error::from_string_literal("DNSSEC validation failed");
                }
                if (validation.status == DNSSEC::SecurityStatus::Insecure)
                    status = DNSSEC::SecurityStatus::Insecure;
                for (auto& record : validation.records)
                    result->add_record(move(record), received_at);
                continue;
            }

            auto rcode = response.header.options.response_code();
            if (rcode != Messages::Options::ResponseCode::NoError && rcode != Messages::Options::ResponseCode::NameError) {
                dbgln_if(DNS_DEBUG, "DNS: {} {} failed with {}", name, Messages::to_string(desired_types[i]), Messages::to_string(rcode));
                return Error::from_string_literal("DNS server returned an error");
            }
            for (auto& record : response.answers_to(question))
                result->add_record(move(record), received_at);
        }

        result->finished_request();
        if (validate)
            result->set_security_status(status);

        m_cache.with_write_locked([&](auto& cache) {
            make_room_in_cache(cache);
            cache.set(name, result);
        });

        return result;
    }

    // Per-name state for an in-flight system-resolver lookup. We split the
    // single AF_UNSPEC `getaddrinfo` call into two parallel calls (AF_INET +
    // AF_INET6) so that buggy stub resolvers (notably systemd-resolved under
    // load) can't drop the AAAA half of a coupled query and stall us on it.
    // The promise resolves as soon as one side returns records, with a small
    // 50 ms grace window for the other side (Happy Eyeballs v2's Resolution
    // Delay, RFC 8305) so curl can prefer IPv6 when both are available. The
    // slower side keeps running and merges its records into the cached
    // LookupResult so subsequent lookups see the full set.
    //
    // `waiting_promises` holds one Promise per `lookup()` caller that joined
    // this resolution. Each caller MUST get their own Promise — Core::Promise
    // has a single on_resolution slot, so sharing one promise across callers
    // means each new `when_resolved` clobbers the previous handler and only
    // the last caller ever fires.
    struct PendingSystemResolution
        : public AtomicRefCounted<PendingSystemResolution>
        , public Weakable<PendingSystemResolution> {
        struct SideTiming {
            i64 queue_ms { 0 };
            i64 work_ms { 0 };
        };

        Vector<NonnullRefPtr<ResultPromise>> waiting_promises;
        NonnullRefPtr<LookupResult> result;
        MonotonicTime dispatched_at;
        Optional<SideTiming> a;
        Optional<SideTiming> aaaa;
        bool promise_resolved { false };
        RefPtr<Core::Timer> grace_timer;

        explicit PendingSystemResolution(NonnullRefPtr<LookupResult> r)
            : result(move(r))
            , dispatched_at(MonotonicTime::now())
        {
        }
    };

    // Resolve (or reject) every joined caller's promise from `state` and tear down any pending grace timer.
    // Idempotent — safe to call from both the main completion path and from the grace timer callback.
    static void try_finalize_pending_system_resolution(PendingSystemResolution& state)
    {
        if (state.promise_resolved)
            return;
        state.promise_resolved = true;
        if (state.grace_timer) {
            state.grace_timer->stop();
            state.grace_timer = nullptr;
        }
        auto promises = move(state.waiting_promises);
        if (state.result->records().is_empty()) {
            for (auto& promise : promises)
                promise->reject(Error::from_string_literal("Could not resolve to IPv4 or IPv6 address"));
        } else {
            for (auto& promise : promises)
                promise->resolve(state.result);
        }
    }

    // Runs on the main thread (deferred-invoked from a ThreadPool worker).
    void handle_system_resolver_completion(
        ByteString const& name,
        PendingSystemResolution& state,
        Core::Socket::AddressFamily family,
        ErrorOr<Vector<Variant<IPv4Address, IPv6Address>>> record_or_error,
        PendingSystemResolution::SideTiming timing)
    {
        (family == Core::Socket::AddressFamily::IPv4Only ? state.a : state.aaaa) = timing;

        // Merge this side's records into the (shared, main-thread-only) LookupResult. Late-arriving records still
        // populate the cache for subsequent lookups.
        bool got_records_this_side = false;
        if (!record_or_error.is_error()) {
            constexpr u32 SYSTEM_RESOLVER_SYNTHETIC_TTL_SECONDS = 60;
            for (auto const& record : record_or_error.value()) {
                record.visit(
                    [&](IPv4Address const& address) {
                        state.result->add_record({ .name = {}, .type = Messages::ResourceType::A, .class_ = Messages::Class::IN, .ttl = SYSTEM_RESOLVER_SYNTHETIC_TTL_SECONDS, .record = Messages::Records::A { address }, .raw = {} });
                        got_records_this_side = true;
                    },
                    [&](IPv6Address const& address) {
                        state.result->add_record({ .name = {}, .type = Messages::ResourceType::AAAA, .class_ = Messages::Class::IN, .ttl = SYSTEM_RESOLVER_SYNTHETIC_TTL_SECONDS, .record = Messages::Records::AAAA { address }, .raw = {} });
                        got_records_this_side = true;
                    });
            }
        }

        bool both_completed = state.a.has_value() && state.aaaa.has_value();

        if (both_completed) {
            state.result->finished_request();
            m_cache.with_write_locked([&](auto& cache) {
                make_room_in_cache(cache);
                cache.set(name, state.result);
            });
            m_pending_system_resolutions.with_write_locked([&](auto& pending) {
                pending.remove(name);
            });

            auto total_ms = (MonotonicTime::now() - state.dispatched_at).to_milliseconds();
            if (total_ms > 5) {
                dbgln_if(REQUESTSERVER_WIRE_DEBUG, "LibDNS wire-dns: lookup({}) path=system-resolver-bg total={} ms = A(queue {} + work {}) | AAAA(queue {} + work {}) (off event loop)",
                    name, total_ms,
                    state.a->queue_ms, state.a->work_ms,
                    state.aaaa->queue_ms, state.aaaa->work_ms);
            }

            try_finalize_pending_system_resolution(state);
            return;
        }

        if (state.promise_resolved)
            return;

        if (!got_records_this_side)
            return;

        // First side has records. Wait briefly so the other side gets a chance to add its records too
        // (RFC 8305 Resolution Delay, 50 ms).
        if (state.grace_timer)
            return;
        constexpr int RESOLUTION_DELAY_MS = 50;
        auto weak_state = state.make_weak_ptr();
        state.grace_timer = Core::Timer::create_single_shot(RESOLUTION_DELAY_MS, [weak_state] {
            if (auto state = weak_state.strong_ref())
                try_finalize_pending_system_resolution(*state);
        });
        state.grace_timer->start();
    }

    // Reads whatever the socket has right now and returns the first complete message, if any. Nothing here waits for
    // more bytes: a datagram is parsed as a whole, and a TCP frame is only parsed once it has fully arrived.
    // An error means the connection is unusable and gets dropped.
    ErrorOr<Optional<Messages::Message>> read_one_message()
    {
        return m_socket.with_write_locked([&](auto& socket) -> ErrorOr<Optional<Messages::Message>> {
            if (m_mode == ConnectionMode::UDP) {
                if (!TRY((*socket)->can_read_without_blocking()))
                    return OptionalNone {};
                auto datagram = TRY((*socket)->read_some(m_receive_buffer));
                // Core treats an empty read as EOF and stops notifying us, so this socket is done even on UDP.
                if (datagram.is_empty())
                    return Error::from_string_literal("DNS server socket reached EOF");
                FixedMemoryStream stream { static_cast<ReadonlyBytes>(datagram) };
                auto message = Messages::Message::from_raw(stream);
                if (message.is_error()) {
                    dbgln("DNS: Dropping malformed datagram: {}", message.error());
                    return OptionalNone {};
                }
                return message.release_value();
            }

            // RFC 1035, 4.2.2. TCP usage.
            // The message is prefixed with a two byte length field which gives the message length, excluding the two
            // byte length field.
            while (true) {
                if (m_incoming.size() >= sizeof(u16)) {
                    size_t size = static_cast<size_t>(m_incoming[0]) << 8 | m_incoming[1];
                    if (m_incoming.size() >= sizeof(u16) + size) {
                        FixedMemoryStream stream { m_incoming.bytes().slice(sizeof(u16), size) };
                        auto message = Messages::Message::from_raw(stream);
                        auto rest = m_incoming.bytes().slice(sizeof(u16) + size);
                        m_incoming = TRY(ByteBuffer::copy(rest));
                        // A frame that does not parse leaves us with no idea where the next one starts.
                        return message;
                    }
                }

                if (!TRY((*socket)->can_read_without_blocking()))
                    return OptionalNone {};
                auto chunk_or_error = (*socket)->read_some(m_receive_buffer);
                if (chunk_or_error.is_error()) {
                    if (chunk_or_error.error().is_errno() && chunk_or_error.error().code() == EAGAIN)
                        return OptionalNone {};
                    return chunk_or_error.release_error();
                }
                if (chunk_or_error.value().is_empty())
                    return Error::from_string_literal("DNS server closed the connection");
                TRY(m_incoming.try_append(chunk_or_error.value()));
            }
        });
    }

    // In-flight queries keep their retry timers, so they get sent again on the next socket.
    void drop_connection()
    {
        m_incoming.clear();
        m_socket.with_write_locked([&](auto& socket) {
            if (socket.has_value()) {
                (*socket)->set_notifications_enabled(false);
                (*socket)->close();
            }
        });
    }

    void process_incoming_messages()
    {
        while (true) {
            auto message_or_error = read_one_message();
            if (message_or_error.is_error()) {
                dbgln("DNS: Failed to receive message: {}", message_or_error.error());
                drop_connection();
                break;
            }
            if (!message_or_error.value().has_value())
                break;

            auto message = message_or_error.release_value().release_value();
            if (message.header.options.is_question())
                continue;

            auto key = m_in_flight_by_id.get(message.header.id);
            if (!key.has_value()) {
                dbgln_if(DNS_DEBUG, "DNS: Received a message with no pending query (id={})", message.header.id);
                continue;
            }
            auto& in_flight = **m_in_flight.get(*key);

            // The response has to echo our question; anything else is not an answer to it.
            if (message.questions.is_empty()) {
                dbgln_if(DNS_DEBUG, "DNS: Received a response without a question (id={})", message.header.id);
                continue;
            }
            auto const& question = message.questions.first();
            if (question.type != in_flight.question.type || question.class_ != in_flight.question.class_ || !question.name.equals_ignoring_case(in_flight.question.name)) {
                dbgln_if(DNS_DEBUG, "DNS: Received a response to a different question (id={})", message.header.id);
                continue;
            }

            if (message.header.options.is_truncated()) {
                settle_query(in_flight, Error::from_string_literal("DNS response was truncated"));
                continue;
            }

            settle_query(in_flight, move(message));
        }
    }

    bool has_connection(bool attempt_restart = true)
    {
        auto result = m_socket.with_read_locked(
            [&](auto& socket) { return socket.has_value() && (*socket)->is_open(); });

        if (attempt_restart && !result && !m_attempting_restart) {
            TemporaryChange change(m_attempting_restart, true);
            auto create_result = m_create_socket();
            if (create_result.is_error()) {
                dbgln_if(DNS_DEBUG, "DNS: Failed to create socket: {}", create_result.error());
                m_use_system_resolver = false;
                return false;
            }

            if (!create_result.value().has_value()) {
                m_use_system_resolver = true;
                return false;
            }

            m_use_system_resolver = false;
            auto [socket, mode] = create_result.release_value().release_value();
            set_socket(move(socket), mode);
            result = true;
        }

        return result;
    }

    void set_socket(MaybeOwned<Core::Socket> socket, ConnectionMode mode = ConnectionMode::UDP)
    {
        m_mode = mode;
        m_incoming.clear();
        m_socket.with_write_locked([&](auto& s) {
            s = move(socket);
            (*s)->on_ready_to_read = [this] {
                process_incoming_messages();
            };
            (*s)->set_notifications_enabled(true);
        });

        for (auto& promise : m_socket_ready_promises)
            promise->resolve({});

        m_socket_ready_promises.clear();
    }

    // Called with the cache write-locked. Drops expired entries, then completed ones, until there is room for one more.
    static void make_room_in_cache(HashMap<ByteString, NonnullRefPtr<LookupResult>>& cache)
    {
        if (cache.size() < MaxCacheEntries)
            return;

        Vector<ByteString> to_remove;
        for (auto& entry : cache) {
            entry.value->check_expiration();
            if (entry.value->can_be_removed())
                to_remove.append(entry.key);
        }
        for (auto const& key : to_remove)
            cache.remove(key);

        if (cache.size() < MaxCacheEntries)
            return;

        to_remove.clear();
        for (auto& entry : cache) {
            if (cache.size() - to_remove.size() < MaxCacheEntries)
                break;
            if (entry.value->is_done())
                to_remove.append(entry.key);
        }
        for (auto const& key : to_remove)
            cache.remove(key);
    }

    RWLockProtected<HashMap<ByteString, NonnullRefPtr<LookupResult>>> m_cache;
    RWLockProtected<HashMap<ByteString, NonnullRefPtr<PendingSystemResolution>>> m_pending_system_resolutions;
    struct PendingLookup {
        Vector<NonnullRefPtr<ResultPromise>> waiters;
        NonnullRefPtr<Core::Promise<Empty>> all_answered;
    };
    HashMap<ByteString, PendingLookup> m_pending_lookups;
    HashMap<ByteString, NonnullOwnPtr<InFlightQuery>> m_in_flight;
    HashMap<u16, ByteString> m_in_flight_by_id;
    RWLockProtected<Optional<MaybeOwned<Core::Socket>>> m_socket;
    ByteBuffer m_receive_buffer;
    ByteBuffer m_incoming;
    Function<ErrorOr<Optional<SocketResult>>()> m_create_socket;
    bool m_attempting_restart { false };
    bool m_use_system_resolver { false };
    ConnectionMode m_mode { ConnectionMode::UDP };
    Vector<NonnullRefPtr<Core::Promise<Empty>>> m_socket_ready_promises;
    DNSSEC::Validator m_validator;
};

}
