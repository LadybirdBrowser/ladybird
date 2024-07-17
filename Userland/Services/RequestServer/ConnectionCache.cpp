/*
 * Copyright (c) 2021-2022, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ConnectionCache.h"
#include <AK/Debug.h>
#include <AK/Find.h>
#include <LibCore/EventLoop.h>

namespace RequestServer::ConnectionCache {

Threading::RWLockProtected<HashMap<ConnectionKey, NonnullOwnPtr<Vector<NonnullOwnPtr<Connection<Core::TCPSocket, Core::Socket>>>>>> g_tcp_connection_cache {};
Threading::RWLockProtected<HashMap<ConnectionKey, NonnullOwnPtr<Vector<NonnullOwnPtr<Connection<TLS::TLSv12>>>>>> g_tls_connection_cache {};
Threading::RWLockProtected<HashMap<ByteString, InferredServerProperties>> g_inferred_server_properties;

void request_did_finish(URL::URL const& url, Core::Socket const* socket)
{
    if (!socket) {
        dbgln("Request with a null socket finished for URL {}", url);
        return;
    }

    dbgln_if(REQUESTSERVER_DEBUG, "Request for {} finished", url);

    ConnectionKey partial_key { url.serialized_host().release_value_but_fixme_should_propagate_errors().to_byte_string(), url.port_or_default() };
    auto fire_off_next_job = [&](auto& cache) {
        using CacheType = typename RemoveCVReference<decltype(cache)>::ProtectedType;
        auto [it, end] = cache.with_read_locked([&](auto const& cache) {
            struct Result {
                decltype(cache.begin()) it;
                decltype(cache.end()) end;
            };
            return Result {
                find_if(cache.begin(), cache.end(), [&](auto& connection) {
                    return connection.key.hostname == partial_key.hostname && connection.key.port == partial_key.port;
                }),
                cache.end(),
            };
        });
        if (it == end) {
            dbgln("Request for URL {} finished, but we don't own that!", url);
            return;
        }
        auto connection_it = it->value->find_if([&](auto& connection) { return connection->socket == socket; });
        if (connection_it.is_end()) {
            dbgln("Request for URL {} finished, but we don't have a socket for that!", url);
            return;
        }

        auto& connection = *connection_it;
        if constexpr (REQUESTSERVER_DEBUG) {
            connection->job_data->timing_info.performing_request = AK::Duration::from_milliseconds(connection->job_data->timing_info.timer.elapsed_milliseconds());
            connection->job_data->timing_info.timer.start();
        }

        auto& properties = g_inferred_server_properties.with_write_locked([&](auto& map) -> InferredServerProperties& { return map.ensure(partial_key.hostname); });
        if (!connection->socket->is_open())
            properties.requests_served_per_connection = min(properties.requests_served_per_connection, connection->max_queue_length + 1);

        if (connection->request_queue.with_read_locked([](auto const& queue) { return queue.is_empty(); })) {
            // Immediately mark the connection as finished, as new jobs will never be run if they are queued
            // before the deferred_invoke() below runs otherwise.
            connection->has_started = false;
            connection->socket->set_notifications_enabled(false);

            Core::deferred_invoke([&connection, &cache_entry = *it->value, key = it->key, &cache] {
                if (connection->has_started)
                    return;

                connection->current_url = {};
                connection->job_data = {};
                connection->removal_timer->on_timeout = [ptr = connection.ptr(), &cache_entry, key = move(key), &cache]() mutable {
                    Core::deferred_invoke([&, key = move(key), ptr] {
                        if (ptr->has_started)
                            return;

                        dbgln_if(REQUESTSERVER_DEBUG, "Removing no-longer-used connection {} (socket {})", ptr, ptr->socket.ptr());
                        cache.with_write_locked([&](CacheType& cache) {
                            auto did_remove = cache_entry.remove_first_matching([&](auto& entry) { return entry == ptr; });
                            VERIFY(did_remove);
                            if (cache_entry.is_empty())
                                cache.remove(key);
                        });
                    });
                };
                connection->removal_timer->start();
            });
        } else {
            auto timer = Core::ElapsedTimer::start_new();
            if (auto result = recreate_socket_if_needed(*connection, url); result.is_error()) {
                if constexpr (REQUESTSERVER_DEBUG) {
                    connection->job_data->timing_info.starting_connection += AK::Duration::from_milliseconds(timer.elapsed_milliseconds());
                }
                cache.with_read_locked([&](auto&) {
                    dbgln("ConnectionCache request finish handler, reconnection failed with {}", result.error());
                    connection->job_data->fail(Core::NetworkJob::Error::ConnectionFailed);
                });
                return;
            }
            if constexpr (REQUESTSERVER_DEBUG) {
                connection->job_data->timing_info.starting_connection += AK::Duration::from_milliseconds(timer.elapsed_milliseconds());
            }

            connection->has_started = true;
            Core::deferred_invoke([&connection = *connection, url, &cache] {
                cache.with_read_locked([&](auto&) {
                    dbgln_if(REQUESTSERVER_DEBUG, "Running next job in queue for connection {}", &connection);
                    connection.timer.start();
                    connection.current_url = url;
                    connection.job_data = connection.request_queue.with_write_locked([](auto& queue) { return queue.take_first(); });
                    if constexpr (REQUESTSERVER_DEBUG) {
                        connection.job_data->timing_info.waiting_in_queue = AK::Duration::from_milliseconds(connection.job_data->timing_info.timer.elapsed_milliseconds() - connection.job_data->timing_info.performing_request.to_milliseconds());
                        connection.job_data->timing_info.timer.start();
                    }
                    connection.socket->set_notifications_enabled(true);
                    connection.job_data->start(*connection.socket);
                });
            });
        }
    };

    if (is<Core::BufferedSocket<TLS::TLSv12>>(socket))
        fire_off_next_job(g_tls_connection_cache);
    else if (is<Core::BufferedSocket<Core::Socket>>(socket))
        fire_off_next_job(g_tcp_connection_cache);
    else
        dbgln("Unknown socket {} finished for URL {}", socket, url);
}

void dump_jobs()
{
    g_tls_connection_cache.with_read_locked([](auto& cache) {
        dbgln("=========== TLS Connection Cache ==========");
        for (auto& connection : cache) {
            dbgln(" - {}:{}", connection.key.hostname, connection.key.port);
            for (auto& entry : *connection.value) {
                dbgln("  - Connection {} (started={}) (socket={})", &entry, entry->has_started, entry->socket.ptr());
                dbgln("    Currently loading {} ({} elapsed)", entry->current_url, entry->timer.is_valid() ? entry->timer.elapsed() : 0);
                dbgln("    Request Queue:");
                entry->request_queue.for_each_locked([](auto const& job) {
                    dbgln("    - {}", &job);
                });
            }
        }
    });

    g_tcp_connection_cache.with_read_locked([](auto& cache) {
        dbgln("=========== TCP Connection Cache ==========");
        for (auto& connection : cache) {
            dbgln(" - {}:{}", connection.key.hostname, connection.key.port);
            for (auto& entry : *connection.value) {
                dbgln("  - Connection {} (started={}) (socket={})", &entry, entry->has_started, entry->socket.ptr());
                dbgln("    Currently loading {} ({} elapsed)", entry->current_url, entry->timer.is_valid() ? entry->timer.elapsed() : 0);
                dbgln("    Request Queue:");
                entry->request_queue.for_each_locked([](auto const& job) {
                    dbgln("    - {}", &job);
                });
            }
        }
    });
}

size_t hits;
size_t misses;
}
