/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <LibGC/Heap.h>
#include <LibWeb/Bindings/ConstantSourceNode.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/ConstantSourceNode.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(ConstantSourceNode);

ConstantSourceNode::ConstantSourceNode(GC::Ref<BaseAudioContext> context, float offset)
    : AudioScheduledSourceNode(context)
    , m_offset(AudioParam::create(context, offset, NumericLimits<float>::lowest(), NumericLimits<float>::max(), AutomationRate::ARate))
{
}

ConstantSourceNode::~ConstantSourceNode() = default;

WebIDL::ExceptionOr<GC::Ref<ConstantSourceNode>> ConstantSourceNode::create(GC::Ref<BaseAudioContext> context, float offset)
{
    return GC::Heap::the().allocate<ConstantSourceNode>(context, offset);
}

WebIDL::ExceptionOr<GC::Ref<ConstantSourceNode>> ConstantSourceNode::create_for_constructor(GC::Ref<BaseAudioContext> context, Bindings::ConstantSourceOptions const& options)
{
    return create_for_constructor(context, options.offset);
}

WebIDL::ExceptionOr<GC::Ref<ConstantSourceNode>> ConstantSourceNode::create_for_constructor(GC::Ref<BaseAudioContext> context, float offset)
{
    return create(context, offset);
}

void ConstantSourceNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_offset);
}

}
