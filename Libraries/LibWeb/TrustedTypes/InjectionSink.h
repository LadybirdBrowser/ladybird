/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::TrustedTypes {

// https://w3c.github.io/trusted-types/dist/spec/#injection-sink
enum class InjectionSink {
    DocumentWrite,
    DocumentWriteln,
    Function,
};

}
