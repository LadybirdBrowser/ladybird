/*
 * Copyright (c) 2024, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>

namespace Requests {

enum class NetworkError {
    UnableToResolveProxy,
    UnableToResolveHost,
    UnableToConnect,
    TimeoutReached,
    TooManyRedirects,
    SSLHandshakeFailed,
    SSLVerificationFailed,
    MalformedUrl,
    InvalidContentEncoding,
    Unknown,
};

constexpr StringView network_error_to_string(NetworkError network_error)
{
    switch (network_error) {
    case NetworkError::UnableToResolveProxy:
        return "Unable to resolve proxy"_sv;
    case NetworkError::UnableToResolveHost:
        return "Unable to resolve host"_sv;
    case NetworkError::UnableToConnect:
        return "Unable to connect"_sv;
    case NetworkError::TimeoutReached:
        return "Timeout reached"_sv;
    case NetworkError::TooManyRedirects:
        return "Too many redirects"_sv;
    case NetworkError::SSLHandshakeFailed:
        return "SSL handshake failed"_sv;
    case NetworkError::SSLVerificationFailed:
        return "SSL verification failed"_sv;
    case NetworkError::MalformedUrl:
        return "The URL is not formatted properly"_sv;
    case NetworkError::InvalidContentEncoding:
        return "Response could not be decoded with its Content-Encoding"_sv;
    case NetworkError::Unknown:
        return "An unexpected network error occurred"_sv;
    }

    VERIFY_NOT_REACHED();
}

}
