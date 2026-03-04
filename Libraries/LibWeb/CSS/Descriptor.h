/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibWeb/CSS/DescriptorNameAndID.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

struct Descriptor {
    ~Descriptor();

    DescriptorNameAndID descriptor_name_and_id;
    NonnullRefPtr<StyleValue const> value;
};

}
