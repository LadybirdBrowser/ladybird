/*
 * Copyright (c) 2024, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

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

}
