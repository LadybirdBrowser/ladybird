/*
 * Copyright (c) 2020-2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibHTTP/HttpsJob.h>

namespace HTTP {

void HttpsJob::set_certificate(ByteString certificate, ByteString key)
{
    m_received_client_certificates = TLS::TLSv12::parse_pem_certificate(certificate.bytes(), key.bytes());
}

}
