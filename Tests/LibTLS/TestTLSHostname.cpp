/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibTLS/TLSv12.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <sys/socket.h>

namespace {

X509* make_certificate(EVP_PKEY* key, char const* common_name, char const* subject_alt_names)
{
    auto* certificate = X509_new();
    VERIFY(certificate);
    VERIFY(X509_set_version(certificate, 2) == 1);
    VERIFY(ASN1_INTEGER_set(X509_get_serialNumber(certificate), 1) == 1);
    VERIFY(X509_gmtime_adj(X509_getm_notBefore(certificate), 0));
    VERIFY(X509_gmtime_adj(X509_getm_notAfter(certificate), 60 * 60));
    VERIFY(X509_set_pubkey(certificate, key) == 1);

    auto* subject = X509_get_subject_name(certificate);
    VERIFY(X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC, reinterpret_cast<u8 const*>(common_name), -1, -1, 0) == 1);
    VERIFY(X509_set_issuer_name(certificate, subject) == 1);

    if (subject_alt_names) {
        auto* extension = X509V3_EXT_conf_nid(nullptr, nullptr, NID_subject_alt_name, const_cast<char*>(subject_alt_names));
        VERIFY(extension);
        VERIFY(X509_add_ext(certificate, extension, -1) == 1);
        X509_EXTENSION_free(extension);
    }
    VERIFY(X509_sign(certificate, key, EVP_sha256()) > 0);
    return certificate;
}

ByteString write_trust_anchor(X509* certificate)
{
    auto path = ByteString::formatted("{}/ladybird-libtls-hostname-{}.pem", Core::StandardPaths::tempfile_directory(), get_random<u64>());
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Write));
    auto* bio = BIO_new(BIO_s_mem());
    VERIFY(bio);
    VERIFY(PEM_write_bio_X509(bio, certificate) == 1);
    char* bytes = nullptr;
    auto size = BIO_get_mem_data(bio, &bytes);
    MUST(file->write_until_depleted({ reinterpret_cast<u8 const*>(bytes), static_cast<size_t>(size) }));
    file->close();
    BIO_free(bio);
    return path;
}

bool tls_connects(X509* certificate, EVP_PKEY* key, StringView host)
{
    auto context = SSL_CTX_new(TLS_server_method());
    VERIFY(context);
    ScopeGuard free_context = [&] { SSL_CTX_free(context); };
    VERIFY(SSL_CTX_use_certificate(context, certificate) == 1);
    VERIFY(SSL_CTX_use_PrivateKey(context, key) == 1);

    auto socket = MUST(Core::System::socket(AF_INET, SOCK_STREAM, 0));
    ScopeGuard close_socket = [&] { MUST(Core::System::close(socket)); };
    int reuse_address = 1;
    MUST(Core::System::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address)));
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    MUST(Core::System::bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)));
    MUST(Core::System::listen(socket, 1));
    socklen_t address_size = sizeof(address);
    VERIFY(getsockname(socket, reinterpret_cast<sockaddr*>(&address), &address_size) == 0);

    auto server = Threading::Thread::construct("TLS hostname test"sv, [socket, context] {
        auto client = accept(socket, nullptr, nullptr);
        if (client < 0)
            return static_cast<intptr_t>(1);
        auto* ssl = SSL_new(context);
        VERIFY(ssl);
        SSL_set_fd(ssl, client);
        SSL_accept(ssl);
        SSL_free(ssl);
        MUST(Core::System::close(client));
        return static_cast<intptr_t>(0);
    });
    server->start();

    auto root_path = write_trust_anchor(certificate);
    ScopeGuard remove_root = [&] { MUST(Core::System::unlink(root_path)); };
    TLS::Options options;
    options.root_certificates_path = root_path;
    auto ip = IPv4Address::from_string("127.0.0.1"sv).release_value();
    auto connection = TLS::TLSv12::connect(Core::SocketAddress { ip, ntohs(address.sin_port) }, host.to_byte_string(), move(options));
    EXPECT(!server->join().is_error());
    return !connection.is_error();
}

}

TEST_CASE(reject_legacy_dns_certificate_identities)
{
    auto* key = EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", 2048);
    VERIFY(key);
    ScopeGuard free_key = [&] { EVP_PKEY_free(key); };

    auto* wildcard = make_certificate(key, "ignored", "DNS:d*.example.localhost,DNS:exact.example.localhost");
    ScopeGuard free_wildcard = [&] { X509_free(wildcard); };
    EXPECT(!tls_connects(wildcard, key, "dns.example.localhost"sv));
    EXPECT(tls_connects(wildcard, key, "exact.example.localhost"sv));

    auto* common_name = make_certificate(key, "dns.example.localhost", nullptr);
    ScopeGuard free_common_name = [&] { X509_free(common_name); };
    EXPECT(!tls_connects(common_name, key, "dns.example.localhost"sv));
}
