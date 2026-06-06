/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/DelayNode.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/DelayNode.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(DelayNode);

DelayNode::DelayNode(GC::Ref<BaseAudioContext> context, Bindings::DelayOptions const& options)
    : AudioNode(context)
    , m_delay_time(AudioParam::create(context, options.delay_time, 0, options.max_delay_time, Bindings::AutomationRate::ARate))
{
}

DelayNode::~DelayNode() = default;

WebIDL::ExceptionOr<GC::Ref<DelayNode>> DelayNode::create(GC::Ref<BaseAudioContext> context, Bindings::DelayOptions const& options)
{
    auto node = GC::Heap::the().allocate<DelayNode>(context, options);

    // Default options for channel count and interpretation
    // https://webaudio.github.io/web-audio-api/#DelayNode
    AudioNodeDefaultOptions default_options;
    default_options.channel_count = 2;
    default_options.channel_count_mode = Bindings::ChannelCountMode::Max;
    default_options.channel_interpretation = Bindings::ChannelInterpretation::Speakers;
    // FIXME: Set tail-time to yes

    TRY(node->initialize_audio_node_options(options, default_options));

    return node;
}

WebIDL::ExceptionOr<void> DelayNode::validate_options(Bindings::DelayOptions const& options)
{
    // https://webaudio.github.io/web-audio-api/#dom-delayoptions-maxdelaytime
    // If specified, this value MUST be greater than zero and less than three minutes or a NotSupportedError exception MUST be thrown.
    static constexpr double maximum_delay_time_seconds = 180;
    if (options.max_delay_time <= 0 || options.max_delay_time >= maximum_delay_time_seconds || isnan(options.max_delay_time))
        return WebIDL::NotSupportedError::create("Max delay time must be between 0 and 180 seconds exclusive"_utf16);

    return {};
}

WebIDL::ExceptionOr<GC::Ref<DelayNode>> DelayNode::construct_impl(GC::Ref<BaseAudioContext> context, Bindings::DelayOptions const& options)
{
    TRY(validate_options(options));
    return create(context, options);
}

void DelayNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_delay_time);
}

}
