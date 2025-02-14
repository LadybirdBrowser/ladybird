/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCrypto/Certificate/Certificate.h>

namespace TLS {

using Crypto::Certificate::Certificate;

class DefaultRootCACertificates {
public:
    DefaultRootCACertificates();

    Vector<Certificate> const& certificates() const { return m_ca_certificates; }

    static ErrorOr<Vector<Certificate>> parse_pem_root_certificate_authorities(ByteBuffer&);
    static ErrorOr<Vector<Certificate>> load_certificates(Span<ByteString> custom_cert_paths = {});

    static DefaultRootCACertificates& the();

    static void set_default_certificate_paths(Span<ByteString> paths);

private:
    Vector<Certificate> m_ca_certificates;
};
}

using TLS::DefaultRootCACertificates;
