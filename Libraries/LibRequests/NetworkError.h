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
    Unknown
};

constexpr StringView network_error_to_string(NetworkError network_error)
{
    switch (network_error) {
    case NetworkError::UnableToResolveProxy:
        return "Unable to resolve proxy"sv;
    case NetworkError::UnableToResolveHost:
        return "Unable to resolve host"sv;
    case NetworkError::UnableToConnect:
        return "Unable to connect"sv;
    case NetworkError::TimeoutReached:
        return "Timeout reached"sv;
    case NetworkError::TooManyRedirects:
        return "Too many redirects"sv;
    case NetworkError::SSLHandshakeFailed:
        return "SSL handshake failed"sv;
    case NetworkError::SSLVerificationFailed:
        return "SSL verification failed"sv;
    case NetworkError::MalformedUrl:
        return "The URL is not formatted properly"sv;
    case NetworkError::Unknown:
        return "An unexpected network error occurred"sv;
    }

    VERIFY_NOT_REACHED();
}

}
