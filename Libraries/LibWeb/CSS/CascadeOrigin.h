/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::CSS {

// https://drafts.csswg.org/css-cascade/#origin
enum class CascadeOrigin : u8 {
    Author,
    // https://drafts.csswg.org/css-cascade/#author-presentational-hint-origin
    AuthorPresentationalHint,
    User,
    UserAgent,
    Animation,
    Transition,
};

}
