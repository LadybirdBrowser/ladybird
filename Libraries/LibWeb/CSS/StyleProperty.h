/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/CSS/PropertyID.h>

namespace Web::CSS {

enum class Important : u8 {
    No,
    Yes,
};

struct WEB_API StyleProperty {
    ~StyleProperty();

    Important important { Important::No };
    PropertyID property_id;
    NonnullRefPtr<StyleValue const> value;
    FlyString custom_name {};

    bool operator==(StyleProperty const& other) const;
};

}
