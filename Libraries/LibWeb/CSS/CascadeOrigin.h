/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::CSS {

// https://www.w3.org/TR/css-cascade/#origin
enum class CascadeOrigin : u8 {
    Author,
    User,
    UserAgent,
    Animation,
    Transition,
};

}
