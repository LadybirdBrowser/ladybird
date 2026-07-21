/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibURL/URL.h>

namespace Web::CSS {

struct StyleSheetResourceContext {
    ::URL::URL base_url;
    bool origin_clean { false };
};

}
