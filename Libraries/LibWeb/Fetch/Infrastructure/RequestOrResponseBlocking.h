/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibWeb/Forward.h>

namespace Web::Fetch::Infrastructure {

enum class RequestOrResponseBlocking {
    Blocked,
    Allowed,
};

}
