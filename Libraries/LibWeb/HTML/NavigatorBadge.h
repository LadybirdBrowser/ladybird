/*
 * Copyright (c) 2025, Estefania Sanchez <e.snchez.c@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

class NavigatorBadgeMixin {
public:
    virtual ~NavigatorBadgeMixin() = default;

    GC::Ref<WebIDL::Promise> set_app_badge(Optional<u64> contents);
    GC::Ref<WebIDL::Promise> clear_app_badge();

protected:
    virtual HTML::Window& window() = 0;
};

}
