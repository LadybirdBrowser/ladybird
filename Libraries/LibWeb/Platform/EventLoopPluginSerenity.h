/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Export.h>
#include <LibWeb/Platform/EventLoopPlugin.h>

namespace Web::Platform {

class WEB_API EventLoopPluginSerenity final : public Web::Platform::EventLoopPlugin {
public:
    EventLoopPluginSerenity();
    virtual ~EventLoopPluginSerenity() override;

    virtual void spin_until(GC::Root<GC::Function<bool()>> goal_condition) override;
    virtual void deferred_invoke(GC::Root<GC::Function<void()>>) override;
    virtual void quit() override;
};

}
