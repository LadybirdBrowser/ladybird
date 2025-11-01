/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Important.h>
#include <LibWeb/CSS/PropertyNameAndID.h>

namespace Web::CSS {

struct WEB_API StyleProperty {
    ~StyleProperty();

    PropertyNameAndID name_and_id;
    NonnullRefPtr<StyleValue const> value;
    Important important { Important::No };

    bool operator==(StyleProperty const& other) const;
};

}
