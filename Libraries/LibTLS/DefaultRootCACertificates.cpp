/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <LibCore/StandardPaths.h>
#include <LibCrypto/ASN1/PEM.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTLS/DefaultRootCACertificates.h>

namespace TLS {

static Vector<ByteString> s_default_ca_certificate_paths;

void DefaultRootCACertificates::set_default_certificate_paths(Span<ByteString> paths)
{
    s_default_ca_certificate_paths.clear();
    s_default_ca_certificate_paths.ensure_capacity(paths.size());
    for (auto& path : paths)
        s_default_ca_certificate_paths.unchecked_append(path);
}

DefaultRootCACertificates::DefaultRootCACertificates()
{
    auto load_result = load_certificates(s_default_ca_certificate_paths);
    if (load_result.is_error()) {
        dbgln("Failed to load CA Certificates: {}", load_result.error());
        return;
    }

    m_ca_certificates = load_result.release_value();
}

DefaultRootCACertificates& DefaultRootCACertificates::the()
{
    static thread_local DefaultRootCACertificates s_the;
    return s_the;
}

ErrorOr<Vector<Certificate>> DefaultRootCACertificates::load_certificates(Span<ByteString> custom_cert_paths)
{
    auto cacert_file_or_error = Core::File::open("/etc/cacert.pem"sv, Core::File::OpenMode::Read);
    ByteBuffer data;
    if (!cacert_file_or_error.is_error())
        data = TRY(cacert_file_or_error.value()->read_until_eof());

    auto user_cert_path = TRY(String::formatted("{}/.config/certs.pem", Core::StandardPaths::home_directory()));
    if (FileSystem::exists(user_cert_path)) {
        auto user_cert_file = TRY(Core::File::open(user_cert_path, Core::File::OpenMode::Read));
        TRY(data.try_append(TRY(user_cert_file->read_until_eof())));
    }

    for (auto& custom_cert_path : custom_cert_paths) {
        if (FileSystem::exists(custom_cert_path)) {
            auto custom_cert_file = TRY(Core::File::open(custom_cert_path, Core::File::OpenMode::Read));
            TRY(data.try_append(TRY(custom_cert_file->read_until_eof())));
        }
    }

    return TRY(parse_pem_root_certificate_authorities(data));
}

ErrorOr<Vector<Certificate>> DefaultRootCACertificates::parse_pem_root_certificate_authorities(ByteBuffer& data)
{
    Vector<Certificate> certificates;

    auto certs = TRY(Crypto::decode_pems(data));

    for (auto& cert : certs) {
        auto certificate_result = Certificate::parse_certificate(cert.data);
        if (certificate_result.is_error()) {
            // FIXME: It would be nice to have more informations about the certificate we failed to parse.
            //        Like: Issuer, Algorithm, CN, etc
            dbgln("Failed to load certificate: {}", certificate_result.error());
            continue;
        }
        auto certificate = certificate_result.release_value();
        if (certificate.is_certificate_authority && certificate.is_self_signed()) {
            TRY(certificates.try_append(move(certificate)));
        } else {
            dbgln("Skipped '{}' because it is not a valid root CA", TRY(certificate.subject.to_string()));
        }
    }

    dbgln_if(TLS_DEBUG, "Loaded {} of {} ({:.2}%) provided CA Certificates", certificates.size(), certs.size(), (certificates.size() * 100.0) / certs.size());

    return certificates;
}

}
