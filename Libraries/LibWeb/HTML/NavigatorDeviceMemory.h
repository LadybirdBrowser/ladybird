/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

class NavigatorDeviceMemoryMixin {
public:
    WebIDL::Double device_memory() const;
};

}
