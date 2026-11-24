/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/NeverDestroyed.h>
#include <AK/ScopeGuard.h>
#include <LibCore/Notifier.h>
#include <LibCore/Timer.h>
#include <RequestServer/DNSStreamCurl.h>

#if defined(AK_OS_WINDOWS)
#    include <AK/Windows.h> // Needed because curl.h includes winsock2.h
#endif

#include <curl/curl.h>

namespace RequestServer {

// The connect and handshake take as long as this at most; the resolver gives up on its own a little later.
static constexpr long s_connect_timeout_seconds = 10L;

class DNSStreamCurl;

// DNS streams share a separate multi handle because they belong to no client.
class DNSStreamCurlMulti {
public:
    static DNSStreamCurlMulti& the()
    {
        static NeverDestroyed<DNSStreamCurlMulti> multi;
        return *multi;
    }

    DNSStreamCurlMulti()
    {
        m_handle = curl_multi_init();
        VERIFY(m_handle);
        curl_multi_setopt(m_handle, CURLMOPT_SOCKETFUNCTION, &on_socket_callback);
        curl_multi_setopt(m_handle, CURLMOPT_SOCKETDATA, this);
        curl_multi_setopt(m_handle, CURLMOPT_TIMERFUNCTION, &on_timeout_callback);
        curl_multi_setopt(m_handle, CURLMOPT_TIMERDATA, this);

        // https://curl.se/libcurl/c/CURLMOPT_TIMERFUNCTION.html
        // When that timer fires, call either curl_multi_socket_action or curl_multi_perform, depending on which
        // interface you use.
        m_timer = Core::Timer::create_single_shot(AK::Duration::zero(), [this] {
            int running = 0;
            curl_multi_socket_action(m_handle, CURL_SOCKET_TIMEOUT, 0, &running);
            check_finished_connections();
        });
    }

    CURLM* handle() const { return m_handle; }

private:
    static int on_socket_callback(CURL*, curl_socket_t socket, int what, void* user_data, void*)
    {
        auto& multi = *static_cast<DNSStreamCurlMulti*>(user_data);
        if (what == CURL_POLL_REMOVE) {
            multi.m_read_notifiers.remove(socket);
            multi.m_write_notifiers.remove(socket);
            return 0;
        }

        auto update = [&](auto& notifiers, Core::NotificationType type, int poll_flag, int select_flag) {
            if (!(what & poll_flag)) {
                if (auto notifier = notifiers.get(socket); notifier.has_value())
                    notifier.value()->set_enabled(false);
                return;
            }
            auto& notifier = notifiers.ensure(socket, [&] {
                auto notifier = Core::Notifier::construct(socket, type);
                notifier->on_activation = [&multi, socket, select_flag] {
                    int running = 0;
                    curl_multi_socket_action(multi.m_handle, socket, select_flag, &running);
                    multi.check_finished_connections();
                };
                return notifier;
            });
            notifier->set_enabled(true);
        };
        update(multi.m_read_notifiers, Core::NotificationType::Read, CURL_POLL_IN, CURL_CSELECT_IN);
        update(multi.m_write_notifiers, Core::NotificationType::Write, CURL_POLL_OUT, CURL_CSELECT_OUT);
        return 0;
    }

    static int on_timeout_callback(CURLM*, long timeout_ms, void* user_data)
    {
        auto& multi = *static_cast<DNSStreamCurlMulti*>(user_data);
        // https://curl.se/libcurl/c/CURLMOPT_TIMERFUNCTION.html
        // A timeout_ms value of -1 passed to this callback means you should delete the timer.
        if (timeout_ms == -1)
            multi.m_timer->stop();
        else
            multi.m_timer->restart(AK::Duration::from_milliseconds(timeout_ms));
        return 0;
    }

    void check_finished_connections();

    CURLM* m_handle { nullptr };
    HashMap<curl_socket_t, NonnullRefPtr<Core::Notifier>> m_read_notifiers;
    HashMap<curl_socket_t, NonnullRefPtr<Core::Notifier>> m_write_notifiers;
    RefPtr<Core::Timer> m_timer;
};

class DNSStreamCurl final : public DNS::StreamConnection {
    AK_ALLOC_WITH_KMALLOC;

public:
    explicit DNSStreamCurl(CURL* easy_handle, curl_slist* resolve_list)
        : m_easy_handle(easy_handle)
        , m_resolve_list(resolve_list)
    {
    }

    virtual ~DNSStreamCurl() override
    {
        if (m_read_notifier)
            m_read_notifier->close();
        if (m_write_notifier)
            m_write_notifier->close();
        curl_multi_remove_handle(DNSStreamCurlMulti::the().handle(), m_easy_handle);
        curl_easy_cleanup(m_easy_handle);
        curl_slist_free_all(m_resolve_list);
    }

    virtual void write(ReadonlyBytes bytes) override
    {
        m_pending_write.append(bytes);
        if (m_connected && !flush())
            fail("Failed to send to the DNS server"sv);
    }

    void did_finish_connecting(CURLcode result)
    {
        if (result != CURLE_OK) {
            dbgln("DNS: Connecting to the DNS server failed: {}", curl_easy_strerror(result));
            return fail("Connecting to the DNS server failed"sv);
        }

        curl_socket_t socket = CURL_SOCKET_BAD;
        if (curl_easy_getinfo(m_easy_handle, CURLINFO_ACTIVESOCKET, &socket) != CURLE_OK || socket == CURL_SOCKET_BAD)
            return fail("Connecting to the DNS server failed"sv);

        m_read_notifier = Core::Notifier::construct(socket, Core::Notifier::Type::Read);
        m_read_notifier->on_activation = [this] { read_from_socket(); };
        m_write_notifier = Core::Notifier::construct(socket, Core::Notifier::Type::Write);
        m_write_notifier->on_activation = [this] {
            if (!flush())
                fail("Failed to send to the DNS server"sv);
        };
        m_write_notifier->set_enabled(false);

        m_connected = true;
        on_connected();
        if (!flush())
            return fail("Failed to send to the DNS server"sv);
        read_from_socket();
    }

private:
    void fail(StringView message)
    {
        if (m_failed)
            return;
        m_failed = true;
        if (m_read_notifier)
            m_read_notifier->set_enabled(false);
        if (m_write_notifier)
            m_write_notifier->set_enabled(false);
        on_error(Error::from_string_view(message));
    }

    void read_from_socket()
    {
        // "Wait on the socket only if curl_easy_recv returns CURLE_AGAIN. The reason for this is libcurl or the SSL
        // library may internally cache some data, therefore you should call curl_easy_recv until all data is read which
        // would include any cached data."
        for (;;) {
            u8 buffer[16384];
            size_t read = 0;
            auto result = curl_easy_recv(m_easy_handle, buffer, sizeof(buffer), &read);
            if (result == CURLE_AGAIN)
                return;
            if (result != CURLE_OK) {
                dbgln("DNS: Reading from the DNS server failed: {}", curl_easy_strerror(result));
                return fail("Failed to read from the DNS server"sv);
            }
            // "Reading exactly 0 bytes indicates a closed connection."
            if (read == 0) {
                m_read_notifier->set_enabled(false);
                return on_eof();
            }
            on_data({ buffer, read });
            // The resolver may have let go of this stream, but it only goes away from the top of the event loop.
            if (m_failed)
                return;
        }
    }

    bool flush()
    {
        while (m_pending_write_offset < m_pending_write.size()) {
            auto pending = m_pending_write.bytes().slice(m_pending_write_offset);
            size_t sent = 0;
            auto result = curl_easy_send(m_easy_handle, pending.data(), pending.size(), &sent);
            if (result != CURLE_OK && result != CURLE_AGAIN) {
                dbgln("DNS: Sending to the DNS server failed: {}", curl_easy_strerror(result));
                return false;
            }
            m_pending_write_offset += sent;
            if (sent > 0)
                continue;
            if (result == CURLE_AGAIN) {
                m_write_notifier->set_enabled(true);
                return true;
            }
            return false;
        }
        m_pending_write.clear();
        m_pending_write_offset = 0;
        if (m_write_notifier)
            m_write_notifier->set_enabled(false);
        return true;
    }

    CURL* m_easy_handle { nullptr };
    curl_slist* m_resolve_list { nullptr };
    RefPtr<Core::Notifier> m_read_notifier;
    RefPtr<Core::Notifier> m_write_notifier;
    ByteBuffer m_pending_write;
    size_t m_pending_write_offset { 0 };
    bool m_connected { false };
    bool m_failed { false };
};

void DNSStreamCurlMulti::check_finished_connections()
{
    int messages_in_queue = 0;
    while (auto* message = curl_multi_info_read(m_handle, &messages_in_queue)) {
        if (message->msg != CURLMSG_DONE)
            continue;
        void* stream = nullptr;
        if (curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &stream) != CURLE_OK || !stream)
            continue;
        static_cast<DNSStreamCurl*>(stream)->did_finish_connecting(message->data.result);
    }
}

static ByteString host_for_url(StringView host)
{
    // An IPv6 address goes in brackets, in a URL and in a CURLOPT_RESOLVE entry alike, whether or not it came in them.
    if (host.starts_with('[') && host.ends_with(']'))
        host = host.substring_view(1, host.length() - 2);
    if (host.contains(':'))
        return ByteString::formatted("[{}]", host);
    return host;
}

ErrorOr<NonnullOwnPtr<DNS::StreamConnection>> connect_dns_stream(Core::SocketAddress const& address, ByteString const& server_name, DNS::StreamSecurity security, DNSStreamTLSOptions const& tls_options)
{
    auto* easy_handle = curl_easy_init();
    if (!easy_handle)
        return Error::from_errno(ENOMEM);

    curl_slist* resolve_list = nullptr;
    ArmedScopeGuard clean_up = [&] {
        curl_easy_cleanup(easy_handle);
        curl_slist_free_all(resolve_list);
    };

    // NB: Every option matters, such as the one that keeps a proxy out of the way, so failing to set one is fatal.
    bool options_set = true;
    auto set_option = [&](auto option, auto value) {
        if (auto result = curl_easy_setopt(easy_handle, option, value); result != CURLE_OK) {
            dbgln("DNS: Failed to set curl option {}: {}", to_underlying(option), curl_easy_strerror(result));
            options_set = false;
        }
    };

    auto ip_address = address.type() == Core::SocketAddress::Type::IPv6
        ? ByteString::formatted("[{}]", address.ipv6_address())
        : ByteString::formatted("{}", address.ipv4_address());

    if (security == DNS::StreamSecurity::TLS) {
        // RFC 7858, 3.1. Session Initiation.
        // By default, a DNS server that supports DNS over TLS MUST listen for and accept TCP connections on port 853.
        // The server name is what the certificate is checked against; the address, which we looked up already, is
        // where curl connects to.
        // CURLOPT_RESOLVE entries look like "[+]HOST:PORT:ADDRESS", and "-HOST:PORT" removes an entry.
        if (server_name.is_empty() || server_name.starts_with('+') || server_name.starts_with('-'))
            return Error::from_string_literal("The DNS server's name cannot be pinned to its address");
        auto host = host_for_url(server_name);
        set_option(CURLOPT_URL, ByteString::formatted("https://{}:{}/", host, address.port()).characters());
        resolve_list = curl_slist_append(nullptr, ByteString::formatted("{}:{}:{}", host, address.port(), ip_address).characters());
        if (!resolve_list)
            return Error::from_errno(ENOMEM);
        set_option(CURLOPT_RESOLVE, resolve_list);
        set_option(CURLOPT_PROTOCOLS_STR, "https");

        // The server speaks DNS, not HTTP, so offer no HTTP protocols through ALPN.
        set_option(CURLOPT_SSL_ENABLE_ALPN, 0L);
        if (!tls_options.root_certificates_path.is_empty())
            set_option(CURLOPT_CAINFO, tls_options.root_certificates_path.characters());
        set_option(CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
        if (tls_options.ssl_context_function) {
            // NB: curl reads this argument as a curl_ssl_ctx_callback, which is what it has to be passed as.
            set_option(CURLOPT_SSL_CTX_FUNCTION, reinterpret_cast<curl_ssl_ctx_callback>(tls_options.ssl_context_function));
            set_option(CURLOPT_SSL_CTX_DATA, nullptr);
        }
    } else {
        set_option(CURLOPT_URL, ByteString::formatted("http://{}:{}/", ip_address, address.port()).characters());
        set_option(CURLOPT_PROTOCOLS_STR, "http");
    }

    // Only the connection, which we then speak DNS over ourselves.
    set_option(CURLOPT_CONNECT_ONLY, 1L);
    set_option(CURLOPT_CONNECTTIMEOUT, s_connect_timeout_seconds);
    set_option(CURLOPT_NOSIGNAL, 1L);
    // The DNS server is what the user chose to talk to, never something a proxy should see.
    set_option(CURLOPT_PROXY, "");
    if (!options_set)
        return Error::from_string_literal("Failed to set up connecting to the DNS server");

    clean_up.disarm();
    auto stream = make<DNSStreamCurl>(easy_handle, resolve_list);
    set_option(CURLOPT_PRIVATE, stream.ptr());
    if (!options_set)
        return Error::from_string_literal("Failed to set up connecting to the DNS server");
    if (auto result = curl_multi_add_handle(DNSStreamCurlMulti::the().handle(), easy_handle); result != CURLM_OK)
        return Error::from_string_literal("Failed to start connecting to the DNS server");
    return stream;
}

}
