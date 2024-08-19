/*
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Platform/EventLoopPlugin.h>

namespace Web::Platform {

class EventLoopPluginSerenity final : public Web::Platform::EventLoopPlugin {
public:
    EventLoopPluginSerenity();
    virtual ~EventLoopPluginSerenity() override;

    virtual void spin_until(GC::SafeFunction<bool()> goal_condition) override;
    virtual void deferred_invoke(GC::SafeFunction<void()>) override;
    virtual NonnullRefPtr<Timer> create_timer() override;
    virtual void quit() override;
};

}
