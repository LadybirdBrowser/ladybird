/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/Base64.h>
#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/ScopeGuard.h>
#include <AK/StringBuilder.h>
#include <AK/Time.h>
#include <LibCrypto/Hash/Hash.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>
#include <LibURL/Parser.h>
#include <RequestServer/AIA.h>
#include <RequestServer/CURL.h>
#include <RequestServer/CertificateExceptions.h>
#include <RequestServer/Resolver.h>
#include <RequestServer/WebSocketImplCurl.h>

#include <arpa/inet.h>
#include <curl/curl.h>
#include <netinet/in.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

// Generated certificate fixtures (EC/prime256v1, fixed validity 2020-2050):
// - SELF_SIGNED_A, SELF_SIGNED_B: certificates for localhost and 127.0.0.1, as often found on local network devices.
// - CA: "Certificate Exception Test CA", the only trusted root.
// - OTHER_NAME: issued by CA to other.test.
// - DEVICE_NAME: issued by CA to device.test, the host every request goes to.
// - EXPIRED_SELF_SIGNED: like SELF_SIGNED_A, but valid only during 2000.
// - NAME_TEST_CA: "Certificate Exception Name Test CA", trusted only by the name matching tests.
// - PARTIAL_WILDCARD: issued by NAME_TEST_CA to DNS:w*.device.test.
// - IP_AS_DNS_NAME: issued by NAME_TEST_CA to DNS:127.0.0.1, without an iPAddress name.
static constexpr auto self_signed_a_certificate = R"PEM(-----BEGIN CERTIFICATE-----
MIIBZDCCAQqgAwIBAgIBATAKBggqhkjOPQQDAjAUMRIwEAYDVQQDDAlsb2NhbGhv
c3QwIBcNMjAwMTAxMDAwMDAwWhgPMjA1MDAxMDEwMDAwMDBaMBQxEjAQBgNVBAMM
CWxvY2FsaG9zdDBZMBMGByqGSM49AgEGCCqGSM49AwEHA0IABO9Zmgz9JJ+yYsdb
68yZNCiZH7nTRbPx2x7JRn1XHiHjASAYVGUodkvAlIRshFm4oykktaRIsKj1tFVs
Y+IhwA+jSzBJMB0GA1UdDgQWBBT190sAlEy0XYzmE9mPvTY+pHrusDAaBgNVHREE
EzARgglsb2NhbGhvc3SHBH8AAAEwDAYDVR0TAQH/BAIwADAKBggqhkjOPQQDAgNI
ADBFAiAptd6uWBDjWlh8EqsB14lrQ1pz+Rf+5OaPDPgTMtxrBQIhALF84JTwcteB
a9KHLuihxdmnOEJHCOvmkyPTo5C0l+eG
-----END CERTIFICATE-----)PEM"sv;
static constexpr auto self_signed_a_key = R"PEM(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgy9/s6BM6FpyYR9Lq
gbX/FNOKvPDW8NSdr0W2k9rHT2WhRANCAATvWZoM/SSfsmLHW+vMmTQomR+500Wz
8dseyUZ9Vx4h4wEgGFRlKHZLwJSEbIRZuKMpJLWkSLCo9bRVbGPiIcAP
-----END PRIVATE KEY-----)PEM"sv;
static constexpr auto self_signed_b_certificate = R"PEM(-----BEGIN CERTIFICATE-----
MIIBZDCCAQqgAwIBAgIBATAKBggqhkjOPQQDAjAUMRIwEAYDVQQDDAlsb2NhbGhv
c3QwIBcNMjAwMTAxMDAwMDAwWhgPMjA1MDAxMDEwMDAwMDBaMBQxEjAQBgNVBAMM
CWxvY2FsaG9zdDBZMBMGByqGSM49AgEGCCqGSM49AwEHA0IABO+lbIADnuw8EzYD
VxLZM5Ylb4/Bzub2K+mjBKpW2A5riYsVt8JlVVjLoDgR/A/wURFz875O4/bMllRT
Y/VLMy6jSzBJMB0GA1UdDgQWBBSN/v6hQeVmC9hdwpDiIv0gMCK/NzAaBgNVHREE
EzARgglsb2NhbGhvc3SHBH8AAAEwDAYDVR0TAQH/BAIwADAKBggqhkjOPQQDAgNI
ADBFAiBbHM0jkzqs8ayK9uudf0pDK+yBLqP+n1jjv1AwBY1wHwIhANevnjgnJYJ+
QYHtA90qRSZzF88B3WieMuZXzzszXEXM
-----END CERTIFICATE-----)PEM"sv;
static constexpr auto self_signed_b_key = R"PEM(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgCrzyL/sUk5v7lfIU
jK3bfEkeHFQEZOai+87RG+MwneKhRANCAATvpWyAA57sPBM2A1cS2TOWJW+Pwc7m
9ivpowSqVtgOa4mLFbfCZVVYy6A4EfwP8FERc/O+TuP2zJZUU2P1SzMu
-----END PRIVATE KEY-----)PEM"sv;
static constexpr auto ca_certificate = R"PEM(-----BEGIN CERTIFICATE-----
MIIBdDCCARmgAwIBAgIBATAKBggqhkjOPQQDAjAoMSYwJAYDVQQDDB1DZXJ0aWZp
Y2F0ZSBFeGNlcHRpb24gVGVzdCBDQTAgFw0yMDAxMDEwMDAwMDBaGA8yMDUwMDEw
MTAwMDAwMFowKDEmMCQGA1UEAwwdQ2VydGlmaWNhdGUgRXhjZXB0aW9uIFRlc3Qg
Q0EwWTATBgcqhkjOPQIBBggqhkjOPQMBBwNCAATEEi1yk5a7Sxn3oAC7KR8FPPAJ
qfOQXFxopx97KL3+llTlqWixgpxYcNm+WHryfAgTGo52y0OGKtAMqQp8xiLdozIw
MDAdBgNVHQ4EFgQU2xKqM4jS50/iBg0ClmBTVoKav7owDwYDVR0TAQH/BAUwAwEB
/zAKBggqhkjOPQQDAgNJADBGAiEAoYypwtjKWGMJWq3owgdvmMkRFUfxyFkq6Rb6
Ki145MkCIQDl+r81A2QTv68DQXITfh0KweDR/su9BcHjfPcoN6Kp7w==
-----END CERTIFICATE-----)PEM"sv;
static constexpr auto other_name_certificate = R"PEM(-----BEGIN CERTIFICATE-----
MIIBVTCB+6ADAgECAgECMAoGCCqGSM49BAMCMCgxJjAkBgNVBAMMHUNlcnRpZmlj
YXRlIEV4Y2VwdGlvbiBUZXN0IENBMCAXDTIwMDEwMTAwMDAwMFoYDzIwNTAwMTAx
MDAwMDAwWjAVMRMwEQYDVQQDDApvdGhlci50ZXN0MFkwEwYHKoZIzj0CAQYIKoZI
zj0DAQcDQgAE7Ie4T89950YWnYRiu/5tyIwPXEMkbX9Sh4tJds9U9FV/t3vf+ctw
LgP+ZlAOV2vrQHUmQxNMXt/vQiy1VxRAeKMnMCUwFQYDVR0RBA4wDIIKb3RoZXIu
dGVzdDAMBgNVHRMBAf8EAjAAMAoGCCqGSM49BAMCA0kAMEYCIQDhFTG2aruG4zor
PfaP1XzYuxuccAbe4FrDV2sC88gxzwIhAKKL4hQvPvGqzjS+ywwLRDyGaYikQxRU
NgSESoKH0saU
-----END CERTIFICATE-----)PEM"sv;
static constexpr auto other_name_key = R"PEM(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQglCB7+ZM1i/yF6sEH
mNmxWC7HIAKz6//mUkuLtAYJhLyhRANCAATsh7hPz33nRhadhGK7/m3IjA9cQyRt
f1KHi0l2z1T0VX+3e9/5y3AuA/5mUA5Xa+tAdSZDE0xe3+9CLLVXFEB4
-----END PRIVATE KEY-----)PEM"sv;
static constexpr auto device_name_certificate = R"PEM(-----BEGIN CERTIFICATE-----
MIIBVjCB/aADAgECAgECMAoGCCqGSM49BAMCMCgxJjAkBgNVBAMMHUNlcnRpZmlj
YXRlIEV4Y2VwdGlvbiBUZXN0IENBMCAXDTIwMDEwMTAwMDAwMFoYDzIwNTAwMTAx
MDAwMDAwWjAWMRQwEgYDVQQDDAtkZXZpY2UudGVzdDBZMBMGByqGSM49AgEGCCqG
SM49AwEHA0IABBp8Gr7JlmkYUyIg8blVKdDebqE2af/OjjAUFjFv1Y8Mswi8gDW3
6JRWhCV4wxPWhJ000b7WVfv9GkRTOKhp52OjKDAmMBYGA1UdEQQPMA2CC2Rldmlj
ZS50ZXN0MAwGA1UdEwEB/wQCMAAwCgYIKoZIzj0EAwIDSAAwRQIhAJLsK1rTUs3o
zg5qBMBnPx2PR9W88Vvb1VIiJR3L7FHaAiBER0MVsgsJagepf7V27sQpU8gZPGKy
SsZNJ2jsOiQ40g==
-----END CERTIFICATE-----)PEM"sv;
static constexpr auto device_name_key = R"PEM(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgTS9Xd7fs6qsAyqCu
7g6UKt+UqB8/PmOuaFLEzDmDM/ihRANCAAQafBq+yZZpGFMiIPG5VSnQ3m6hNmn/
zo4wFBYxb9WPDLMIvIA1t+iUVoQleMMT1oSdNNG+1lX7/RpEUzioaedj
-----END PRIVATE KEY-----)PEM"sv;

static constexpr auto expired_self_signed_certificate = R"PEM(-----BEGIN CERTIFICATE-----
MIIBYjCCAQigAwIBAgIBATAKBggqhkjOPQQDAjAUMRIwEAYDVQQDDAlsb2NhbGhv
c3QwHhcNMDAwMTAxMDAwMDAwWhcNMDEwMTAxMDAwMDAwWjAUMRIwEAYDVQQDDAls
b2NhbGhvc3QwWTATBgcqhkjOPQIBBggqhkjOPQMBBwNCAAQEBjjfZb5MsAjgRlGN
37BzpalJzdrzYVFJYjew4RGw3v2DxA1/RaA0cP+BvHYSQfZm8Rzb5d/NUeSxQc5W
2jrEo0swSTAdBgNVHQ4EFgQURRoQocTWe4dD4RGWWG4uZI1pygYwGgYDVR0RBBMw
EYIJbG9jYWxob3N0hwR/AAABMAwGA1UdEwEB/wQCMAAwCgYIKoZIzj0EAwIDSAAw
RQIgfUL3H3kiTmVkqFDhe4SZlldhC5u28Mfy/IHfWeGHKisCIQCGYZJHsFLw86SZ
AIz29iYsviCmrVeGHGU8dKf3qTBWlA==
-----END CERTIFICATE-----)PEM"sv;
static constexpr auto expired_self_signed_key = R"PEM(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgARDe2t0Cw7bhGF2R
WVNoBL6T0f7VohXoKklul39phsmhRANCAAQEBjjfZb5MsAjgRlGN37BzpalJzdrz
YVFJYjew4RGw3v2DxA1/RaA0cP+BvHYSQfZm8Rzb5d/NUeSxQc5W2jrE
-----END PRIVATE KEY-----)PEM"sv;
static constexpr auto name_test_ca_certificate = R"PEM(-----BEGIN CERTIFICATE-----
MIIBjTCCATOgAwIBAgIBATAKBggqhkjOPQQDAjAtMSswKQYDVQQDDCJDZXJ0aWZp
Y2F0ZSBFeGNlcHRpb24gTmFtZSBUZXN0IENBMCAXDTIwMDEwMTAwMDAwMFoYDzIw
NTAwMTAxMDAwMDAwWjAtMSswKQYDVQQDDCJDZXJ0aWZpY2F0ZSBFeGNlcHRpb24g
TmFtZSBUZXN0IENBMFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAE33kn8GusidyX
ZJQjryeTakIu3bR5t3l+pZjJLtoMXLCPnGowLz0QRlgYC72Y3ITXS5ET0JpNtRDr
PYe3mFAijqNCMEAwHQYDVR0OBBYEFGostUXUlgdCV5BohUWYhB3C3JLtMA8GA1Ud
EwEB/wQFMAMBAf8wDgYDVR0PAQH/BAQDAgIEMAoGCCqGSM49BAMCA0gAMEUCIQC/
RS3IsvtuvcYNkbKD8qNsZ4SKlAQPuVUL0yVBYPQnWQIgPGdAMao7UEMM5uDmukHC
MHGGMAGWj9WXAbtuLClBAH4=
-----END CERTIFICATE-----)PEM"sv;
static constexpr auto partial_wildcard_certificate = R"PEM(-----BEGIN CERTIFICATE-----
MIIBYjCCAQigAwIBAgIBBDAKBggqhkjOPQQDAjAtMSswKQYDVQQDDCJDZXJ0aWZp
Y2F0ZSBFeGNlcHRpb24gTmFtZSBUZXN0IENBMCAXDTIwMDEwMTAwMDAwMFoYDzIw
NTAwMTAxMDAwMDAwWjAZMRcwFQYDVQQDDA53Ki5kZXZpY2UudGVzdDBZMBMGByqG
SM49AgEGCCqGSM49AwEHA0IABKTU3Whq098R6YYEL9kzzxSEB/MrhZSqAiH4TAwD
RtJ9pRsvspz1TwrbjcVdovwLC60aTrVpx0vh05ZQnb+6tV+jKzApMBkGA1UdEQQS
MBCCDncqLmRldmljZS50ZXN0MAwGA1UdEwEB/wQCMAAwCgYIKoZIzj0EAwIDSAAw
RQIgCH+/ygRkA2V5+sHuSamsw+u1Ljy2rAV18tvMfDFwwAcCIQDQAwAggxuK4x33
sOi+3h6xHRmZCztULswCNeYo5yJ6Ng==
-----END CERTIFICATE-----)PEM"sv;
static constexpr auto partial_wildcard_key = R"PEM(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgtMywkaXIKfR0rgbc
l6guYImTnzidJxLlr9lEoraItwyhRANCAASk1N1oatPfEemGBC/ZM88UhAfzK4WU
qgIh+EwMA0bSfaUbL7Kc9U8K243FXaL8CwutGk61acdL4dOWUJ2/urVf
-----END PRIVATE KEY-----)PEM"sv;
static constexpr auto ip_as_dns_name_certificate = R"PEM(-----BEGIN CERTIFICATE-----
MIIBWDCB/qADAgECAgEDMAoGCCqGSM49BAMCMC0xKzApBgNVBAMMIkNlcnRpZmlj
YXRlIEV4Y2VwdGlvbiBOYW1lIFRlc3QgQ0EwIBcNMjAwMTAxMDAwMDAwWhgPMjA1
MDAxMDEwMDAwMDBaMBQxEjAQBgNVBAMMCTEyNy4wLjAuMTBZMBMGByqGSM49AgEG
CCqGSM49AwEHA0IABPlHM3ZKAim06qymiS63nC820LbiLon5VpAaKlbMMBVgubxd
Q423Z64t9m/w3qkcs6mGLCrTR4Elbp6W8WW8ky6jJjAkMBQGA1UdEQQNMAuCCTEy
Ny4wLjAuMTAMBgNVHRMBAf8EAjAAMAoGCCqGSM49BAMCA0kAMEYCIQCCjhjoa/Ju
Z15vfWF0RBYsj59P39qpLqZDe3B2DSngpAIhAM/wbF910sHBT5ojT7te6jZcla6H
mooAyalhHgxvFzib
-----END CERTIFICATE-----)PEM"sv;
static constexpr auto ip_as_dns_name_key = R"PEM(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgCGpiM3wQasG6TgSv
/oGrd8upHgYNoa73DD9nlq7mRvKhRANCAAT5RzN2SgIptOqspokut5wvNtC24i6J
+VaQGipWzDAVYLm8XUONt2euLfZv8N6pHLOphiwq00eBJW6elvFlvJMu
-----END PRIVATE KEY-----)PEM"sv;

static constexpr auto host = "device.test"sv;

// Serves HTTPS and WebSocket handshakes with the same certificate on a loopback port.
class TLSServer {
public:
    TLSServer(StringView certificate, StringView key)
    {
        // A client that rejects the certificate closes the connection mid-handshake.
        signal(SIGPIPE, SIG_IGN);

        m_context = SSL_CTX_new(TLS_server_method());
        VERIFY(m_context);

        auto* certificate_bio = BIO_new_mem_buf(certificate.characters_without_null_termination(), static_cast<int>(certificate.length()));
        auto* x509 = PEM_read_bio_X509(certificate_bio, nullptr, nullptr, nullptr);
        BIO_free(certificate_bio);
        auto* key_bio = BIO_new_mem_buf(key.characters_without_null_termination(), static_cast<int>(key.length()));
        auto* private_key = PEM_read_bio_PrivateKey(key_bio, nullptr, nullptr, nullptr);
        BIO_free(key_bio);
        VERIFY(x509 && private_key);
        VERIFY(SSL_CTX_use_certificate(m_context, x509) == 1);
        VERIFY(SSL_CTX_use_PrivateKey(m_context, private_key) == 1);
        X509_free(x509);
        EVP_PKEY_free(private_key);

        m_socket = socket(AF_INET, SOCK_STREAM, 0);
        VERIFY(m_socket >= 0);
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        VERIFY(bind(m_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        VERIFY(listen(m_socket, 8) == 0);
        socklen_t length = sizeof(address);
        VERIFY(getsockname(m_socket, reinterpret_cast<sockaddr*>(&address), &length) == 0);
        m_port = ntohs(address.sin_port);

        m_thread = Threading::Thread::construct("TLSServer"sv, [this] { return serve(); });
        m_thread->start();
    }

    ~TLSServer()
    {
        m_stopping.store(true);
        // Wake up accept() with a connection of our own.
        auto wake = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(m_port);
        (void)connect(wake, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        (void)m_thread->join();
        close(wake);
        close(m_socket);
        SSL_CTX_free(m_context);
    }

    u16 port() const { return m_port; }

private:
    intptr_t serve()
    {
        while (true) {
            auto connection = accept(m_socket, nullptr, nullptr);
            if (connection < 0)
                continue;
            if (m_stopping.load()) {
                close(connection);
                return 0;
            }

            timeval timeout { 10, 0 };
            VERIFY(setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
            VERIFY(setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0);
            auto* ssl = SSL_new(m_context);
            SSL_set_fd(ssl, connection);
            if (SSL_accept(ssl) == 1) {
                StringBuilder request;
                while (!request.string_view().contains("\r\n\r\n"sv) && request.length() < 16 * KiB) {
                    char buffer[4096];
                    auto count = SSL_read(ssl, buffer, sizeof(buffer));
                    if (count <= 0)
                        break;
                    request.append(StringView { buffer, static_cast<size_t>(count) });
                }
                if (request.string_view().contains("\r\n\r\n"sv)) {
                    ByteString response = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                    for (auto line : request.string_view().split_view("\r\n"sv)) {
                        if (!line.starts_with("Sec-WebSocket-Key:"sv, CaseSensitivity::CaseInsensitive))
                            continue;
                        auto key = line.substring_view("Sec-WebSocket-Key:"sv.length()).trim_whitespace();
                        auto accept_input = ByteString::formatted("{}258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
                        auto digest = Crypto::Hash::digest(Crypto::Hash::HashKind::SHA1, accept_input.bytes());
                        auto accept = MUST(encode_base64(digest.bytes()));
                        response = ByteString::formatted("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: {}\r\n\r\n", accept);
                    }
                    (void)SSL_write(ssl, response.characters(), static_cast<int>(response.length()));
                }
                SSL_shutdown(ssl);
            }
            SSL_free(ssl);
            close(connection);
        }
    }

    SSL_CTX* m_context { nullptr };
    int m_socket { -1 };
    u16 m_port { 0 };
    Atomic<bool> m_stopping { false };
    RefPtr<Threading::Thread> m_thread;
};

struct FetchResult {
    CURLcode code;
    Optional<ByteString> rejected_self_signed_certificate_fingerprint;
    long verify_result { X509_V_OK };
};

static FetchResult fetch(TLSServer const& server, bool connect_only = false, StringView hostname = host, StringView trusted_ca = ca_certificate)
{
    // A custom certificate path prevents macOS from sharing the root store, isolating each fetch's fixture root.
    RequestServer::set_default_certificate_path("fixture-root");
    ScopeGuard restore_certificate_path = [] { RequestServer::set_default_certificate_path({}); };

    auto collector = make_ref_counted<RequestServer::AIACollector>(RequestServer::IsPrivate::No);
    collector->host = hostname;
    collector->port = server.port();

    auto url = ByteString::formatted("https://{}:{}/", hostname, server.port());
    auto resolve_entry = ByteString::formatted("{}:{}:127.0.0.1", hostname, server.port());
    auto* resolve = curl_slist_append(nullptr, resolve_entry.characters());

    curl_blob ca_blob {
        .data = const_cast<char*>(trusted_ca.characters_without_null_termination()),
        .len = trusted_ca.length(),
        .flags = CURL_BLOB_COPY,
    };

    auto* curl = curl_easy_init();
    curl_easy_setopt(curl, CURLOPT_URL, url.characters());
    curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve);
    curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &ca_blob);
    curl_easy_setopt(curl, CURLOPT_CAPATH, nullptr);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, connect_only ? 1L : 0L);
    EXPECT_EQ(RequestServer::configure_certificate_verification(curl, *collector), CURLE_OK);
    auto code = curl_easy_perform(curl);
    long verify_result = X509_V_OK;
    curl_easy_getinfo(curl, CURLINFO_SSL_VERIFYRESULT, &verify_result);
    curl_easy_cleanup(curl);
    curl_slist_free_all(resolve);

    return { code, collector->rejected_self_signed_certificate_fingerprint, verify_result };
}

static CURLcode connect_websocket(TLSServer const& server, StringView hostname = host)
{
    auto* multi = curl_multi_init();
    VERIFY(multi);
    ScopeGuard cleanup = [&] { curl_multi_cleanup(multi); };
    auto impl = RequestServer::WebSocketImplCurl::create(multi, RequestServer::IsPrivate::No);
    impl->on_connected = [] { };
    impl->on_connection_error = [] { };
    impl->on_ready_to_read = [] { };

    auto dns_result = make_ref_counted<DNS::LookupResult>(hostname, Vector<DNS::LookupResult::Record> { { .type = DNS::Messages::ResourceType::A, .ttl = 60, .address = IPv4Address { 127, 0, 0, 1 }, .text = "127.0.0.1"_string } }, OptionalNone {});
    auto url = URL::Parser::basic_parse(ByteString::formatted("wss://{}:{}/", hostname, server.port())).release_value();
    WebSocket::ConnectionInfo info { move(url) };
    info.set_dns_result(move(dns_result));
    impl->connect(info);

    auto deadline = MonotonicTime::now() + AK::Duration::from_seconds(10);
    int running = 0;
    do {
        if (curl_multi_perform(multi, &running) != CURLM_OK)
            return CURLE_FAILED_INIT;
        int remaining = 0;
        while (auto* message = curl_multi_info_read(multi, &remaining)) {
            if (message->msg == CURLMSG_DONE)
                return message->data.result;
        }
        if (curl_multi_poll(multi, nullptr, 0, 100, nullptr) != CURLM_OK)
            return CURLE_FAILED_INIT;
    } while (running && MonotonicTime::now() < deadline);
    return CURLE_OPERATION_TIMEDOUT;
}

TEST_CASE(self_signed_certificate_for_another_name_can_be_trusted)
{
    TLSServer server { self_signed_a_certificate, self_signed_a_key };

    auto rejected = fetch(server);
    EXPECT_EQ(rejected.code, CURLE_PEER_FAILED_VERIFICATION);
    EXPECT(rejected.rejected_self_signed_certificate_fingerprint.has_value());

    RequestServer::add_certificate_exception(ByteString { host }, server.port(), *rejected.rejected_self_signed_certificate_fingerprint);
    auto trusted = fetch(server);
    EXPECT_EQ(trusted.code, CURLE_OK);
    EXPECT(!trusted.rejected_self_signed_certificate_fingerprint.has_value());

    TLSServer same_certificate_elsewhere { self_signed_a_certificate, self_signed_a_key };
    EXPECT_EQ(fetch(same_certificate_elsewhere).code, CURLE_PEER_FAILED_VERIFICATION);
}

TEST_CASE(exception_does_not_cover_another_self_signed_certificate)
{
    TLSServer server { self_signed_b_certificate, self_signed_b_key };
    RequestServer::add_certificate_exception(ByteString { host }, server.port(), "00"sv);

    // Still offered to the user, who may want to trust the new certificate instead.
    auto result = fetch(server);
    EXPECT_EQ(result.code, CURLE_PEER_FAILED_VERIFICATION);
    EXPECT(result.rejected_self_signed_certificate_fingerprint.has_value());
}

TEST_CASE(exception_does_not_lift_the_name_check_for_other_certificates)
{
    TLSServer server { other_name_certificate, other_name_key };
    RequestServer::add_certificate_exception(ByteString { host }, server.port(), "00"sv);

    auto result = fetch(server);
    EXPECT_EQ(result.code, CURLE_PEER_FAILED_VERIFICATION);
    EXPECT_EQ(result.verify_result, X509_V_ERR_HOSTNAME_MISMATCH);
    EXPECT(!result.rejected_self_signed_certificate_fingerprint.has_value());
}

static ByteString fingerprint_of(StringView pem)
{
    auto* bio = BIO_new_mem_buf(pem.characters_without_null_termination(), static_cast<int>(pem.length()));
    auto* certificate = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    VERIFY(certificate);
    auto fingerprint = RequestServer::certificate_fingerprint(certificate);
    X509_free(certificate);
    return fingerprint;
}

// OpenSSL reports a self-signed certificate before it checks validity periods.
TEST_CASE(exception_does_not_accept_an_expired_certificate)
{
    TLSServer server { expired_self_signed_certificate, expired_self_signed_key };
    RequestServer::add_certificate_exception(ByteString { host }, server.port(), fingerprint_of(expired_self_signed_certificate));

    // Not offered to the user either, as trusting it could not make it valid.
    auto result = fetch(server);
    EXPECT_EQ(result.code, CURLE_PEER_FAILED_VERIFICATION);
    EXPECT(!result.rejected_self_signed_certificate_fingerprint.has_value());
}

// curl matches a fully qualified name without its trailing dot.
TEST_CASE(exception_keeps_a_trailing_dot_name_working)
{
    TLSServer server { device_name_certificate, device_name_key };
    RequestServer::add_certificate_exception("device.test."sv, server.port(), "00"sv);

    EXPECT_EQ(fetch(server, false, "device.test."sv).code, CURLE_OK);
}

// Match curl's rejection of partial wildcards.
TEST_CASE(exception_does_not_allow_partial_wildcards_for_other_certificates)
{
    TLSServer server { partial_wildcard_certificate, partial_wildcard_key };
    RequestServer::add_certificate_exception("www.device.test"sv, server.port(), "00"sv);

    auto result = fetch(server, false, "www.device.test"sv, name_test_ca_certificate);
    EXPECT_EQ(result.code, CURLE_PEER_FAILED_VERIFICATION);
    EXPECT_EQ(result.verify_result, X509_V_ERR_HOSTNAME_MISMATCH);
}

// curl matches an IP address only against iPAddress names.
TEST_CASE(exception_does_not_match_an_ip_address_against_dns_names)
{
    TLSServer server { ip_as_dns_name_certificate, ip_as_dns_name_key };
    RequestServer::add_certificate_exception("127.0.0.1"sv, server.port(), "00"sv);

    auto result = fetch(server, false, "127.0.0.1"sv, name_test_ca_certificate);
    EXPECT_EQ(result.code, CURLE_PEER_FAILED_VERIFICATION);
    EXPECT_EQ(result.verify_result, X509_V_ERR_HOSTNAME_MISMATCH);
}

TEST_CASE(exception_keeps_certificates_for_the_right_name_working)
{
    TLSServer server { device_name_certificate, device_name_key };
    EXPECT_EQ(fetch(server).code, CURLE_OK);

    RequestServer::add_certificate_exception(ByteString { host }, server.port(), "00"sv);
    EXPECT_EQ(fetch(server).code, CURLE_OK);
}

TEST_CASE(certificate_exception_allows_secure_websockets_and_preconnects)
{
    TLSServer server { self_signed_a_certificate, self_signed_a_key };
    auto rejected = fetch(server);
    EXPECT_EQ(rejected.code, CURLE_PEER_FAILED_VERIFICATION);
    VERIFY(rejected.rejected_self_signed_certificate_fingerprint.has_value());
    EXPECT_EQ(connect_websocket(server), CURLE_PEER_FAILED_VERIFICATION);
    EXPECT_EQ(fetch(server, true).code, CURLE_PEER_FAILED_VERIFICATION);

    RequestServer::add_certificate_exception(ByteString { host }, server.port(), *rejected.rejected_self_signed_certificate_fingerprint);
    EXPECT_EQ(connect_websocket(server), CURLE_OK);
    EXPECT_EQ(fetch(server, true).code, CURLE_OK);
    EXPECT_EQ(connect_websocket(server, "other.test"sv), CURLE_PEER_FAILED_VERIFICATION);

    TLSServer another_port { self_signed_a_certificate, self_signed_a_key };
    EXPECT_EQ(connect_websocket(another_port), CURLE_PEER_FAILED_VERIFICATION);
    EXPECT_EQ(fetch(another_port, true).code, CURLE_PEER_FAILED_VERIFICATION);
}

TEST_CASE(certificate_exception_does_not_allow_other_websocket_or_preconnect_certificates)
{
    TLSServer trusted { self_signed_a_certificate, self_signed_a_key };
    auto rejected = fetch(trusted);
    VERIFY(rejected.rejected_self_signed_certificate_fingerprint.has_value());

    TLSServer another_certificate { self_signed_b_certificate, self_signed_b_key };
    RequestServer::add_certificate_exception(ByteString { host }, another_certificate.port(), *rejected.rejected_self_signed_certificate_fingerprint);
    EXPECT_EQ(connect_websocket(another_certificate), CURLE_PEER_FAILED_VERIFICATION);
    EXPECT_EQ(fetch(another_certificate, true).code, CURLE_PEER_FAILED_VERIFICATION);
}
