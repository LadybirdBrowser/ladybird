/*
 * Copyright (c) 2020, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Endian.h>
#include <AK/Random.h>

#include <LibCore/Timer.h>
#include <LibCrypto/ASN1/DER.h>
#include <LibTLS/TLSv12.h>

namespace TLS {

ssize_t TLSv12::handle_certificate(ReadonlyBytes buffer)
{
    ssize_t res = 0;

    if (buffer.size() < 3) {
        dbgln_if(TLS_DEBUG, "not enough certificate header data");
        return (i8)Error::NeedMoreData;
    }

    u32 certificate_total_length = buffer[0] * 0x10000 + buffer[1] * 0x100 + buffer[2];

    dbgln_if(TLS_DEBUG, "total length: {}", certificate_total_length);

    if (certificate_total_length <= 4)
        return 3 * certificate_total_length;

    res += 3;

    if (certificate_total_length > buffer.size() - res) {
        dbgln_if(TLS_DEBUG, "not enough data for claimed total cert length");
        return (i8)Error::NeedMoreData;
    }
    size_t size = certificate_total_length;

    bool valid_certificate = false;

    while (size > 0) {
        if (buffer.size() - res < 3) {
            dbgln_if(TLS_DEBUG, "not enough data for certificate length");
            return (i8)Error::NeedMoreData;
        }
        size_t certificate_size = buffer[res] * 0x10000 + buffer[res + 1] * 0x100 + buffer[res + 2];
        res += 3;

        if (buffer.size() - res < certificate_size) {
            dbgln_if(TLS_DEBUG, "not enough data for certificate body");
            return (i8)Error::NeedMoreData;
        }

        auto res_cert = res;
        auto remaining = certificate_size;

        do {
            if (remaining <= 3) {
                dbgln("Ran out of data");
                break;
            }
            if (buffer.size() < (size_t)res_cert + 3) {
                dbgln("not enough data to read cert size ({} < {})", buffer.size(), res_cert + 3);
                break;
            }
            size_t certificate_size_specific = buffer[res_cert] * 0x10000 + buffer[res_cert + 1] * 0x100 + buffer[res_cert + 2];
            res_cert += 3;
            remaining -= 3;

            if (certificate_size_specific > remaining) {
                dbgln("invalid certificate size (expected {} but got {})", remaining, certificate_size_specific);
                break;
            }
            remaining -= certificate_size_specific;

            auto certificate = Certificate::parse_certificate(buffer.slice(res_cert, certificate_size_specific), false);
            if (!certificate.is_error()) {
                m_context.certificates.empend(certificate.value());
                valid_certificate = true;
            } else {
                dbgln("Failed to parse client cert: {}", certificate.error());
                dbgln("{:hex-dump}", buffer.slice(res_cert, certificate_size_specific));
                dbgln("");
            }
            res_cert += certificate_size_specific;
        } while (remaining > 0);
        if (remaining) {
            dbgln("extraneous {} bytes left over after parsing certificates", remaining);
        }
        size -= certificate_size + 3;
        res += certificate_size;
    }
    if (!valid_certificate)
        return (i8)Error::UnsupportedCertificate;

    if ((size_t)res != buffer.size())
        dbgln("some data left unread: {} bytes out of {}", res, buffer.size());

    return res;
}

ssize_t TLSv12::handle_certificate_verify(ReadonlyBytes)
{
    dbgln("FIXME: parse_verify");
    return 0;
}

}
