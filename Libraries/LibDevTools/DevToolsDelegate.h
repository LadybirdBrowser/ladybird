/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/Forward.h>

namespace DevTools {

class DevToolsDelegate {
public:
    virtual ~DevToolsDelegate() = default;

    virtual Vector<TabDescription> tab_list() const { return {}; }
};

}
