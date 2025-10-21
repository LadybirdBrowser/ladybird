/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/CSS/Important.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

struct WEB_API StyleProperty {
    ~StyleProperty();

    Important important { Important::No };
    PropertyID property_id;
    NonnullRefPtr<StyleValue const> value;
    FlyString custom_name {};

    bool operator==(StyleProperty const& other) const;
};

}
