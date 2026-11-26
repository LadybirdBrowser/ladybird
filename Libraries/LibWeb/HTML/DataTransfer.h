/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/DataTransfer.h>
#include <LibWeb/Bindings/RustInterface.h>

namespace Web::HTML {

class DataTransfer final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(DataTransfer, data_transfer)
};

}
