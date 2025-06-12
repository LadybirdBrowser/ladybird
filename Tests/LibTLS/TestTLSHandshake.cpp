/*
 * Copyright (c) 2021, Peter Bocan  <me@pbocan.net>
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/EventLoop.h>
#include <LibCrypto/ASN1/ASN1.h>
#include <LibCrypto/ASN1/PEM.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTLS/TLSv12.h>
#include <LibTest/TestCase.h>

static StringView ca_certs_file = "./cacert.pem"sv;
static int port = 443;

constexpr auto DEFAULT_SERVER = "www.google.com"sv;

static ByteBuffer operator""_b(char const* string, size_t length)
{
    return ByteBuffer::copy(string, length).release_value();
}

static Optional<ByteString> locate_ca_certs_file()
{
    if (FileSystem::exists(ca_certs_file)) {
        return ca_certs_file;
    }
    auto on_target_path = ByteString("/etc/cacert.pem");
    if (FileSystem::exists(on_target_path)) {
        return on_target_path;
    }
    return {};
}

TEST_CASE(test_TLS_hello_handshake)
{
    TLS::Options options;
    options.set_root_certificates_path(locate_ca_certs_file());

    auto tls = TRY_OR_FAIL(TLS::TLSv12::connect(DEFAULT_SERVER, port, move(options)));

    TRY_OR_FAIL(tls->write_until_depleted("GET /generate_204 HTTP/1.1\r\nHost: "_b));

    auto the_server = DEFAULT_SERVER;
    TRY_OR_FAIL(tls->write_until_depleted(the_server.bytes()));
    TRY_OR_FAIL(tls->write_until_depleted("\r\nConnection: close\r\n\r\n"_b));

    auto tmp = TRY_OR_FAIL(ByteBuffer::create_zeroed(128));
    auto contents = TRY_OR_FAIL(tls->read_some(tmp));
    EXPECT(contents.starts_with("HTTP/1.1 204 No Content\r\n"_b));
}
