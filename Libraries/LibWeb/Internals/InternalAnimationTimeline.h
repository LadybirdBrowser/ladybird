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
    WEB_WRAPPABLE(InternalAnimationTimeline, Web::Animations::AnimationTimeline);
    GC_DECLARE_ALLOCATOR(InternalAnimationTimeline);

    [[nodiscard]] static GC::Ref<InternalAnimationTimeline> create(GC::Ref<DOM::Document>);

    virtual Optional<Animations::TimeValue> duration() const override { return {}; }

    virtual Optional<double> convert_a_timeline_time_to_an_origin_relative_time(Optional<Animations::TimeValue>) override { return {}; }

    virtual void update_current_time(double timestamp) override;

    void set_time(Optional<double> time);
    void set_time_for_observation(double time);

protected:
    virtual bool can_sample_current_time_at_timestamp() const override { return m_time_for_observation.has_value(); }
    virtual Optional<Animations::TimeValue> current_time_at_timestamp(double) const override { return m_time_for_observation; }

private:
    explicit InternalAnimationTimeline(GC::Ref<DOM::Document>);
    virtual ~InternalAnimationTimeline() override = default;

    Optional<Animations::TimeValue> m_time_for_observation;
};

}
