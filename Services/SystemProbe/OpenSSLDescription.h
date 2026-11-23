/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibPlatformFeatures/SystemFacts.h>

// The probe links curl and OpenSSL the way RequestServer does, so it loads the same libraries that RequestServer does.
PlatformFeatures::OpenSSLFacts describe_openssl();
