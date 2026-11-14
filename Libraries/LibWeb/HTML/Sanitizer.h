/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Bindings/Sanitizer.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/SanitizerConfiguration.h>

namespace Web::HTML {

class WEB_API Sanitizer final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(Sanitizer, sanitizer)

public:
    SanitizerConfiguration configuration() const;
};

}
