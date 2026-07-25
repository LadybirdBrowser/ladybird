/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <LibWeb/Bindings/ConstantSourceNode.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAudio/AudioParam.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/ConstantSourceNode.h>
#include <LibWeb/WebAudio/Rendering/RenderNodes.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(ConstantSourceNode);

ConstantSourceNode::ConstantSourceNode(JS::Realm& realm, GC::Ref<BaseAudioContext> context, Bindings::ConstantSourceOptions const& options)
    : AudioScheduledSourceNode(realm, context)
    , m_offset(AudioParam::create(realm, context, this, options.offset, NumericLimits<float>::lowest(), NumericLimits<float>::max(), Bindings::AutomationRate::ARate))
{
}

ConstantSourceNode::~ConstantSourceNode() = default;

WebIDL::ExceptionOr<GC::Ref<ConstantSourceNode>> ConstantSourceNode::create(JS::Realm& realm, GC::Ref<BaseAudioContext> context, Bindings::ConstantSourceOptions const& options)
{
    return construct_impl(realm, context, options);
}

WebIDL::ExceptionOr<GC::Ref<ConstantSourceNode>> ConstantSourceNode::construct_impl(JS::Realm& realm, GC::Ref<BaseAudioContext> context, Bindings::ConstantSourceOptions const& options)
{
    auto node = realm.create<ConstantSourceNode>(realm, context, options);
    node->queue_render_node_creation(make<Rendering::ConstantSourceRenderNode>(node->node_id(), BaseAudioContext::render_quantum_size(), node->m_offset->render_param()));
    return node;
}

void ConstantSourceNode::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ConstantSourceNode);
    Base::initialize(realm);
}

void ConstantSourceNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_offset);
}

}
