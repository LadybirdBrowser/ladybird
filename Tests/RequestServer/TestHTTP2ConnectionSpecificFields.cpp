/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Vector.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>
#include <RequestServer/CURL.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

constexpr auto client_preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"sv;
constexpr u8 frame_type_headers = 0x1;
constexpr u8 frame_type_settings = 0x4;
constexpr u8 flag_end_stream = 0x1;
constexpr u8 flag_ack = 0x1;
constexpr u8 flag_end_headers = 0x4;

void append_frame(ByteBuffer& buffer, u8 type, u8 flags, u32 stream_id, ReadonlyBytes payload)
{
    u8 header[9] = {
        static_cast<u8>(payload.size() >> 16), static_cast<u8>(payload.size() >> 8), static_cast<u8>(payload.size()),
        type, flags,
        static_cast<u8>(stream_id >> 24), static_cast<u8>(stream_id >> 16), static_cast<u8>(stream_id >> 8), static_cast<u8>(stream_id)
    };
    buffer.append(header, sizeof(header));
    buffer.append(payload);
}

// HPACK: a literal field without indexing, with a literal name (RFC 7541, section 6.2.2).
void append_literal_field(ByteBuffer& block, StringView name, StringView value)
{
    block.append(0x00);
    block.append(static_cast<u8>(name.length()));
    block.append(name.bytes());
    block.append(static_cast<u8>(value.length()));
    block.append(value.bytes());
}

// A cleartext HTTP/2 server for a single connection, which answers the client's first request with the given fields.
class HTTP2Server {
public:
    explicit HTTP2Server(Vector<std::pair<StringView, StringView>> response_fields)
        : m_response_fields(move(response_fields))
    {
        m_listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        VERIFY(m_listen_fd >= 0);
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        VERIFY(::bind(m_listen_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        VERIFY(::listen(m_listen_fd, 1) == 0);
        socklen_t length = sizeof(address);
        VERIFY(::getsockname(m_listen_fd, reinterpret_cast<sockaddr*>(&address), &length) == 0);
        m_port = ntohs(address.sin_port);

        m_thread = Threading::Thread::construct("HTTP2Server"sv, [this] {
            serve();
            return static_cast<intptr_t>(0);
        });
        m_thread->start();
    }

    ~HTTP2Server()
    {
        (void)m_thread->join();
        ::close(m_listen_fd);
    }

    u16 port() const { return m_port; }

private:
    void serve()
    {
        // Give up if no client connects in time (a proxy that takes the request elsewhere, e.g.), so that the test fails
        // rather than hangs when it joins this thread.
        pollfd listen_poll { m_listen_fd, POLLIN, 0 };
        if (::poll(&listen_poll, 1, 10'000) <= 0)
            return;
        auto fd = ::accept(m_listen_fd, nullptr, nullptr);
        if (fd < 0)
            return;

        // Read the client's preface and frames until its request (a HEADERS frame on stream 1) has arrived.
        ByteBuffer received;
        size_t offset = 0;
        bool has_request = false;
        while (!has_request) {
            u8 chunk[4096];
            auto count = ::read(fd, chunk, sizeof(chunk));
            if (count <= 0)
                break;
            received.append(chunk, count);
            if (offset == 0) {
                if (received.size() < client_preface.length())
                    continue;
                offset = client_preface.length();
            }
            while (received.size() >= offset + 9) {
                auto length = (received[offset] << 16) | (received[offset + 1] << 8) | received[offset + 2];
                if (received.size() < offset + 9 + length)
                    break;
                if (received[offset + 3] == frame_type_headers)
                    has_request = true;
                offset += 9 + length;
            }
        }

        ByteBuffer response;
        append_frame(response, frame_type_settings, 0, 0, {});
        append_frame(response, frame_type_settings, flag_ack, 0, {});
        ByteBuffer block;
        block.append(0x88); // ":status: 200", from the static table.
        for (auto const& [name, value] : m_response_fields)
            append_literal_field(block, name, value);
        append_frame(response, frame_type_headers, flag_end_headers | flag_end_stream, 1, block);
        VERIFY(::write(fd, response.data(), response.size()) == static_cast<ssize_t>(response.size()));

        // Hold the connection open until the client is done with it.
        u8 chunk[4096];
        while (::read(fd, chunk, sizeof(chunk)) > 0) { }
        ::close(fd);
    }

    Vector<std::pair<StringView, StringView>> m_response_fields;
    int m_listen_fd { -1 };
    u16 m_port { 0 };
    RefPtr<Threading::Thread> m_thread;
};

size_t collect_header(char* data, size_t size, size_t count, void* user_data)
{
    auto& headers = *static_cast<Vector<ByteString>*>(user_data);
    headers.append(ByteString { data, size * count }.trim_whitespace());
    return size * count;
}

}

// A connection-specific field isn't allowed in an HTTP/2 response, but some servers send one. The response is still
// delivered, without that field, rather than failing.
TEST_CASE(http2_response_with_connection_specific_fields_is_delivered)
{
    MUST(RequestServer::initialize_libcurl());

    HTTP2Server server({ { "connection"sv, "close"sv }, { "keep-alive"sv, "timeout=5"sv }, { "x-test"sv, "delivered"sv } });

    auto* easy = curl_easy_init();
    VERIFY(easy);
    auto url = ByteString::formatted("http://127.0.0.1:{}/", server.port());
    Vector<ByteString> headers;
    curl_easy_setopt(easy, CURLOPT_URL, url.characters());
    curl_easy_setopt(easy, CURLOPT_NOPROXY, "*");
    curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE);
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, collect_header);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA, &headers);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT, 10L);

    auto result = curl_easy_perform(easy);
    long status = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(easy);

    EXPECT_EQ(result, CURLE_OK);
    EXPECT_EQ(status, 200);
    EXPECT(headers.contains_slow("x-test: delivered"sv));
    EXPECT(!headers.contains_slow("connection: close"sv));
}
