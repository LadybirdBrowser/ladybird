/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

struct UsedUserSelect {
    GC::Ptr<Element const> element;
    CSS::UserSelect value { CSS::UserSelect::Text };
};

CSS::UserSelect used_user_select_for_element(Element const&, UsedUserSelect inherited = {});

}
