/*
 * Copyright (c) 2021, Peter Bocan  <me@pbocan.net>
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

ErrorOr<Vector<Certificate>> load_certificates();
ByteString locate_ca_certs_file();

ByteString locate_ca_certs_file()
{
    if (FileSystem::exists(ca_certs_file)) {
        return ca_certs_file;
    }
    auto on_target_path = ByteString("/etc/cacert.pem");
    if (FileSystem::exists(on_target_path)) {
        return on_target_path;
    }
    return "";
}

ErrorOr<Vector<Certificate>> load_certificates()
{
    auto cacert_file = TRY(Core::File::open(locate_ca_certs_file(), Core::File::OpenMode::Read));
    auto data = TRY(cacert_file->read_until_eof());
    return TRY(DefaultRootCACertificates::parse_pem_root_certificate_authorities(data));
}

TEST_CASE(test_TLS_hello_handshake)
{
    Core::EventLoop loop;
    TLS::Options options;
    options.set_root_certificates(TRY_OR_FAIL(load_certificates()));
    options.set_alert_handler([&](TLS::AlertDescription) {
        FAIL("Connection failure");
        loop.quit(1);
    });
    options.set_finish_callback([&] {
        loop.quit(0);
    });

    auto tls = TRY_OR_FAIL(TLS::TLSv12::connect(DEFAULT_SERVER, port, move(options)));
    ByteBuffer contents;
    tls->on_ready_to_read = [&] {
        (void)TRY_OR_FAIL(tls->read_some(contents.must_get_bytes_for_writing(4 * KiB)));
        loop.quit(0);
    };

    if (tls->write_until_depleted("GET / HTTP/1.1\r\nHost: "_b).is_error()) {
        FAIL("write(0) failed");
        return;
    }

    auto the_server = DEFAULT_SERVER;
    if (tls->write_until_depleted(the_server.bytes()).is_error()) {
        FAIL("write(1) failed");
        return;
    }
    if (tls->write_until_depleted("\r\nConnection : close\r\n\r\n"_b).is_error()) {
        FAIL("write(2) failed");
        return;
    }

    loop.exec();
}
