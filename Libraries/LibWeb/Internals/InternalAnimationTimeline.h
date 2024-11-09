/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Animations/AnimationTimeline.h>

namespace Web::Internals {

class InternalAnimationTimeline : public Web::Animations::AnimationTimeline {
public:
    WEB_PLATFORM_OBJECT(InternalAnimationTimeline, Web::Animations::AnimationTimeline);
    JS_DECLARE_ALLOCATOR(InternalAnimationTimeline);

    virtual void set_current_time(Optional<double> current_time) override;

    void set_time(Optional<double> time);

private:
    explicit InternalAnimationTimeline(JS::Realm&);
    virtual ~InternalAnimationTimeline() override = default;

    virtual void initialize(JS::Realm&) override;
};

}
