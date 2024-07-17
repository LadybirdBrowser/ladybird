/*
 * Copyright (c) 2021-2022, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Debug.h>
#include <AK/HashMap.h>
#include <AK/Vector.h>
#include <LibCore/ElapsedTimer.h>
#include <LibCore/EventLoop.h>
#include <LibCore/NetworkJob.h>
#include <LibCore/SOCKSProxyClient.h>
#include <LibCore/Timer.h>
#include <LibTLS/TLSv12.h>
#include <LibThreading/RWLockProtected.h>
#include <LibURL/URL.h>

namespace RequestServer {

enum class CacheLevel {
    ResolveOnly,
    CreateConnection,
};

}

namespace RequestServer::ConnectionCache {

struct Proxy {
    Core::ProxyData data;
    OwnPtr<Core::SOCKSProxyClient> proxy_client_storage {};

    template<typename SocketType, typename StorageType, typename... Args>
    ErrorOr<NonnullOwnPtr<StorageType>> tunnel(URL::URL const& url, Args&&... args)
    {
        if (data.type == Core::ProxyData::Direct) {
            return TRY(SocketType::connect(TRY(url.serialized_host()).to_byte_string(), url.port_or_default(), forward<Args>(args)...));
        }
        if (data.type == Core::ProxyData::SOCKS5) {
            if constexpr (requires { SocketType::connect(declval<ByteString>(), *proxy_client_storage, forward<Args>(args)...); }) {
                proxy_client_storage = TRY(Core::SOCKSProxyClient::connect(data.host_ipv4, data.port, Core::SOCKSProxyClient::Version::V5, TRY(url.serialized_host()).to_byte_string(), url.port_or_default()));
                return TRY(SocketType::connect(TRY(url.serialized_host()).to_byte_string(), *proxy_client_storage, forward<Args>(args)...));
            } else if constexpr (IsSame<SocketType, Core::TCPSocket>) {
                return TRY(Core::SOCKSProxyClient::connect(data.host_ipv4, data.port, Core::SOCKSProxyClient::Version::V5, TRY(url.serialized_host()).to_byte_string(), url.port_or_default()));
            } else {
                return Error::from_string_literal("SOCKS5 not supported for this socket type");
            }
        }
        VERIFY_NOT_REACHED();
    }
};

struct JobData {
    Function<void(Core::BufferedSocketBase&)> start {};
    Function<void(Core::NetworkJob::Error)> fail {};
    Function<Vector<TLS::Certificate>()> provide_client_certificates {};
    struct TimingInfo {
#if REQUESTSERVER_DEBUG
        bool valid { true };
        Core::ElapsedTimer timer {};
        URL::URL url {};
        Duration waiting_in_queue {};
        Duration starting_connection {};
        Duration performing_request {};
#endif
    } timing_info {};

    JobData(Function<void(Core::BufferedSocketBase&)> start, Function<void(Core::NetworkJob::Error)> fail, Function<Vector<TLS::Certificate>()> provide_client_certificates, TimingInfo timing_info)
        : start(move(start))
        , fail(move(fail))
        , provide_client_certificates(move(provide_client_certificates))
        , timing_info(move(timing_info))
    {
    }

    JobData(JobData&& other)
        : start(move(other.start))
        , fail(move(other.fail))
        , provide_client_certificates(move(other.provide_client_certificates))
        , timing_info(move(other.timing_info))
    {
#if REQUESTSERVER_DEBUG
        other.timing_info.valid = false;
#endif
    }

#if REQUESTSERVER_DEBUG
    ~JobData()
    {
        if (timing_info.valid) {
            dbgln("JobData for {} timings:", timing_info.url);
            dbgln("  - Waiting in queue: {}ms", timing_info.waiting_in_queue.to_milliseconds());
            dbgln("  - Starting connection: {}ms", timing_info.starting_connection.to_milliseconds());
            dbgln("  - Performing request: {}ms", timing_info.performing_request.to_milliseconds());
        }
    }
#endif

    template<typename T>
    static JobData create(NonnullRefPtr<T> job, [[maybe_unused]] URL::URL url)
    {
        return JobData {
            [job](auto& socket) { job->start(socket); },
            [job](auto error) { job->fail(error); },
            [job] {
                if constexpr (requires { job->on_certificate_requested; }) {
                    if (job->on_certificate_requested)
                        return job->on_certificate_requested();
                } else {
                    // "use" `job`, otherwise clang gets sad.
                    (void)job;
                }
                return Vector<TLS::Certificate> {};
            },
            {
#if REQUESTSERVER_DEBUG
                .timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise),
                .url = move(url),
                .waiting_in_queue = {},
                .starting_connection = {},
                .performing_request = {},
#endif
            },
        };
    }
};

template<typename Socket, typename SocketStorageType = Socket>
struct Connection {
    using QueueType = Vector<JobData>;
    using SocketType = Socket;
    using StorageType = SocketStorageType;

    OwnPtr<Core::BufferedSocket<SocketStorageType>> socket;
    Threading::RWLockProtected<QueueType> request_queue;
    NonnullRefPtr<Core::Timer> removal_timer;
    Atomic<bool> is_being_started { false };
    bool has_started { false };
    URL::URL current_url {};
    Core::ElapsedTimer timer {};
    Optional<JobData> job_data {};
    Proxy proxy {};
    size_t max_queue_length { 0 };
};

struct ConnectionKey {
    ByteString hostname;
    u16 port { 0 };
    Core::ProxyData proxy_data {};

    bool operator==(ConnectionKey const&) const = default;
};

};

template<>
struct AK::Traits<RequestServer::ConnectionCache::ConnectionKey> : public AK::DefaultTraits<RequestServer::ConnectionCache::ConnectionKey> {
    static u32 hash(RequestServer::ConnectionCache::ConnectionKey const& key)
    {
        return pair_int_hash(pair_int_hash(key.proxy_data.host_ipv4, key.proxy_data.port), pair_int_hash(key.hostname.hash(), key.port));
    }
};

namespace RequestServer::ConnectionCache {

struct InferredServerProperties {
    size_t requests_served_per_connection { NumericLimits<size_t>::max() };
};

extern Threading::RWLockProtected<HashMap<ConnectionKey, NonnullOwnPtr<Vector<NonnullOwnPtr<Connection<Core::TCPSocket, Core::Socket>>>>>> g_tcp_connection_cache;
extern Threading::RWLockProtected<HashMap<ConnectionKey, NonnullOwnPtr<Vector<NonnullOwnPtr<Connection<TLS::TLSv12>>>>>> g_tls_connection_cache;
extern Threading::RWLockProtected<HashMap<ByteString, InferredServerProperties>> g_inferred_server_properties;

void request_did_finish(URL::URL const&, Core::Socket const*);
void dump_jobs();

constexpr static size_t MaxConcurrentConnectionsPerURL = 4;
constexpr static size_t ConnectionKeepAliveTimeMilliseconds = 20'000;
constexpr static size_t ConnectionCacheQueueHighWatermark = 4;

template<typename T>
ErrorOr<void> recreate_socket_if_needed(T& connection, URL::URL const& url)
{
    using SocketType = typename T::SocketType;
    using SocketStorageType = typename T::StorageType;

    if (!connection.socket || !connection.socket->is_open() || connection.socket->is_eof()) {
        connection.socket = nullptr;
        // Create another socket for the connection.
        auto set_socket = [&](NonnullOwnPtr<SocketStorageType>&& socket) -> ErrorOr<void> {
            connection.socket = TRY(Core::BufferedSocket<SocketStorageType>::create(move(socket)));
            return {};
        };

        if constexpr (IsSame<TLS::TLSv12, SocketType>) {
            TLS::Options options;
            options.set_alert_handler([&connection](TLS::AlertDescription alert) {
                Core::NetworkJob::Error reason;
                if (alert == TLS::AlertDescription::HANDSHAKE_FAILURE)
                    reason = Core::NetworkJob::Error::ProtocolFailed;
                else if (alert == TLS::AlertDescription::DECRYPT_ERROR)
                    reason = Core::NetworkJob::Error::ConnectionFailed;
                else
                    reason = Core::NetworkJob::Error::TransmissionFailed;

                if (connection.job_data->fail)
                    connection.job_data->fail(reason);
            });
            options.set_certificate_provider([&connection]() -> Vector<TLS::Certificate> {
                if (connection.job_data->provide_client_certificates)
                    return connection.job_data->provide_client_certificates();
                return {};
            });
            TRY(set_socket(TRY((connection.proxy.template tunnel<SocketType, SocketStorageType>(url, move(options))))));
        } else {
            TRY(set_socket(TRY((connection.proxy.template tunnel<SocketType, SocketStorageType>(url)))));
        }
        dbgln_if(REQUESTSERVER_DEBUG, "Creating a new socket for {} -> {}", url, connection.socket.ptr());
    }
    return {};
}

extern size_t hits;
extern size_t misses;

template<typename Cache>
void start_connection(const URL::URL& url, auto job, auto& sockets_for_url, size_t index, AK::Duration, Cache&);

void ensure_connection(auto& cache, const URL::URL& url, auto job, Core::ProxyData proxy_data = {})
{
    using CacheEntryType = RemoveCVReference<decltype(*declval<typename RemoveCVReference<decltype(cache)>::ProtectedType>().begin()->value)>;

    auto hostname = url.serialized_host().release_value_but_fixme_should_propagate_errors().to_byte_string();
    auto& properties = g_inferred_server_properties.with_write_locked([&](auto& map) -> InferredServerProperties& { return map.ensure(hostname); });

    auto& sockets_for_url = *cache.with_write_locked([&](auto& map) -> NonnullOwnPtr<CacheEntryType>& {
        return map.ensure({ move(hostname), url.port_or_default(), proxy_data }, [] { return make<CacheEntryType>(); });
    });

    // Find the connection with an empty queue; if none exist, we'll find the least backed-up connection later.
    // Note that servers that are known to serve a single request per connection (e.g. HTTP/1.0) usually have
    // issues with concurrent connections, so we'll only allow one connection per URL in that case to avoid issues.
    // This is a bit too aggressive, but there's no way to know if the server can handle concurrent connections
    // without trying it out first, and that's not worth the effort as HTTP/1.0 is a legacy protocol anyway.
    auto it = sockets_for_url.find_if([&](auto const& connection) {
        return properties.requests_served_per_connection < 2
            || connection->request_queue.with_read_locked([](auto const& queue) { return queue.size(); }) <= ConnectionCacheQueueHighWatermark;
    });
    auto did_add_new_connection = false;
    auto failed_to_find_a_socket = it.is_end();

    Proxy proxy { proxy_data };
    size_t index;

    auto timer = Core::ElapsedTimer::start_new();
    if (failed_to_find_a_socket && sockets_for_url.size() < MaxConcurrentConnectionsPerURL) {
        using ConnectionType = RemoveCVReference<decltype(*AK::Detail::declval<CacheEntryType>().at(0))>;
        auto& connection = cache.with_write_locked([&](auto&) -> ConnectionType& {
            index = sockets_for_url.size();
            sockets_for_url.append(AK::make<ConnectionType>(
                nullptr,
                typename ConnectionType::QueueType {},
                Core::Timer::create_single_shot(ConnectionKeepAliveTimeMilliseconds, nullptr),
                true));
            auto& connection = sockets_for_url.last();
            connection->proxy = move(proxy);
            return *connection;
        });
        ScopeGuard start_guard = [&] {
            connection.is_being_started = false;
        };
        dbgln_if(REQUESTSERVER_DEBUG, "I will start a connection ({}) for URL {}", &connection, url);

        auto connection_result = proxy.tunnel<typename ConnectionType::SocketType, typename ConnectionType::StorageType>(url);
        misses++;
        if (connection_result.is_error()) {
            dbgln("ConnectionCache: Connection to {} failed: {}", url, connection_result.error());
            Core::deferred_invoke([job] {
                job->fail(Core::NetworkJob::Error::ConnectionFailed);
            });
            return;
        }
        auto socket_result = Core::BufferedSocket<typename ConnectionType::StorageType>::create(connection_result.release_value());
        if (socket_result.is_error()) {
            dbgln("ConnectionCache: Failed to make a buffered socket for {}: {}", url, socket_result.error());
            Core::deferred_invoke([job] {
                job->fail(Core::NetworkJob::Error::ConnectionFailed);
            });
            return;
        }
        did_add_new_connection = true;
        connection.socket = socket_result.release_value();
    }

    auto elapsed = AK::Duration::from_milliseconds(timer.elapsed_milliseconds());

    if (failed_to_find_a_socket) {
        if (!did_add_new_connection) {
            // Find the least backed-up connection (based on how many entries are in their request queue).
            index = 0;
            auto min_queue_size = (size_t)-1;
            for (auto it = sockets_for_url.begin(); it != sockets_for_url.end(); ++it) {
                if (auto queue_size = (*it)->request_queue.with_read_locked([](auto const& queue) { return queue.size(); }); min_queue_size > queue_size) {
                    index = it.index();
                    min_queue_size = queue_size;
                }
            }
        }
    } else {
        index = it.index();
        hits++;
    }

    dbgln_if(REQUESTSERVER_DEBUG, "ConnectionCache: Hits: {}, Misses: {}", RequestServer::ConnectionCache::hits, RequestServer::ConnectionCache::misses);
    start_connection(url, job, sockets_for_url, index, elapsed, cache);
}

template<typename Cache>
void start_connection(URL::URL const& url, auto job, auto& sockets_for_url, size_t index, AK::Duration setup_time, Cache& cache)
{
    if (sockets_for_url.is_empty()) {
        Core::deferred_invoke([job] {
            job->fail(Core::NetworkJob::Error::ConnectionFailed);
        });
        return;
    }

    auto& connection = *sockets_for_url[index];
    if (connection.is_being_started) {
        // Someone else is creating the connection, queue the job and let them handle it.
        dbgln_if(REQUESTSERVER_DEBUG, "Enqueue request for URL {} in {} - {}", url, &connection, connection.socket.ptr());
        auto size = connection.request_queue.with_write_locked([&](auto& queue) {
            queue.append(JobData::create(job, url));
            return queue.size();
        });
        connection.max_queue_length = max(connection.max_queue_length, size);
        return;
    }

    if (!connection.has_started) {
        connection.has_started = true;
        Core::deferred_invoke([&connection, &cache, url, job, setup_time] {
            (void)setup_time;
            auto job_data = JobData::create(job, url);
            if constexpr (REQUESTSERVER_DEBUG) {
                job_data.timing_info.waiting_in_queue = AK::Duration::from_milliseconds(job_data.timing_info.timer.elapsed_milliseconds());
                job_data.timing_info.timer.start();
            }
            if (auto result = recreate_socket_if_needed(connection, url); result.is_error()) {
                dbgln_if(REQUESTSERVER_DEBUG, "ConnectionCache: request failed to start, failed to make a socket: {}", result.error());
                if constexpr (REQUESTSERVER_DEBUG) {
                    job_data.timing_info.starting_connection += AK::Duration::from_milliseconds(job_data.timing_info.timer.elapsed_milliseconds()) + setup_time;
                    job_data.timing_info.timer.start();
                }
                Core::deferred_invoke([job] {
                    job->fail(Core::NetworkJob::Error::ConnectionFailed);
                });
            } else {
                cache.with_write_locked([&](auto&) {
                    dbgln_if(REQUESTSERVER_DEBUG, "Immediately start request for url {} in {} - {}", url, &connection, connection.socket.ptr());
                    connection.job_data = move(job_data);
                    if constexpr (REQUESTSERVER_DEBUG) {
                        connection.job_data->timing_info.starting_connection += AK::Duration::from_milliseconds(connection.job_data->timing_info.timer.elapsed_milliseconds()) + setup_time;
                        connection.job_data->timing_info.timer.start();
                    }
                    connection.removal_timer->stop();
                    connection.timer.start();
                    connection.current_url = url;
                    connection.socket->set_notifications_enabled(true);
                    connection.job_data->start(*connection.socket);
                });
            }
        });
    } else {
        dbgln_if(REQUESTSERVER_DEBUG, "Enqueue request for URL {} in {} - {}", url, &connection, connection.socket.ptr());
        auto size = connection.request_queue.with_write_locked([&](auto& queue) {
            queue.append(JobData::create(job, url));
            return queue.size();
        });
        connection.max_queue_length = max(connection.max_queue_length, size);
    }
}
}
