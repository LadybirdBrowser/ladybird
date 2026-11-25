/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/EnumBits.h>
#include <AK/Types.h>
#include <LibRequests/NetworkError.h>

namespace Requests {

// One certificate may fail for several reasons.
enum class CertificateError : u8 {
    None = 0,
    // The certificate is self-signed, or its chain ends at a self-signed root that isn't trusted.
    SelfSigned = 1 << 0,
    UnknownIssuer = 1 << 1,
    Expired = 1 << 2,
    NotYetValid = 1 << 3,
    NameMismatch = 1 << 4,
    Other = 1 << 5,
};

AK_ENUM_BITWISE_OPERATORS(CertificateError);

// NB: Devices on a local network often come with a self-signed certificate for another name, such as localhost.
constexpr bool is_only_self_signed(CertificateError errors)
{
    return has_flag(errors, CertificateError::SelfSigned)
        && !has_any_flag(errors, ~(CertificateError::SelfSigned | CertificateError::NameMismatch));
}

constexpr NetworkError network_error_for_certificate_errors(CertificateError errors)
{
    if (is_only_self_signed(errors))
        return NetworkError::SSLSelfSignedCertificate;
    if (has_flag(errors, CertificateError::Expired))
        return NetworkError::SSLCertificateExpired;
    if (has_flag(errors, CertificateError::NotYetValid))
        return NetworkError::SSLCertificateNotYetValid;
    if (has_any_flag(errors, CertificateError::SelfSigned | CertificateError::UnknownIssuer))
        return NetworkError::SSLCertificateUnknownIssuer;
    if (has_flag(errors, CertificateError::NameMismatch))
        return NetworkError::SSLCertificateNameMismatch;
    return NetworkError::SSLCertificateInvalid;
}

}
