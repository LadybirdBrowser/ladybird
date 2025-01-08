/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <LibWeb/Bindings/ConstantSourceNodePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/ConstantSourceNode.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(ConstantSourceNode);

ConstantSourceNode::ConstantSourceNode(JS::Realm& realm, GC::Ref<BaseAudioContext> context, ConstantSourceOptions const& options)
    : AudioScheduledSourceNode(realm, context)
    , m_offset(AudioParam::create(realm, context, options.offset, NumericLimits<float>::lowest(), NumericLimits<float>::max(), Bindings::AutomationRate::ARate))
{
}

ConstantSourceNode::~ConstantSourceNode() = default;

WebIDL::ExceptionOr<GC::Ref<ConstantSourceNode>> ConstantSourceNode::create(JS::Realm& realm, GC::Ref<BaseAudioContext> context, ConstantSourceOptions const& options)
{
    return construct_impl(realm, context, options);
}

WebIDL::ExceptionOr<GC::Ref<ConstantSourceNode>> ConstantSourceNode::construct_impl(JS::Realm& realm, GC::Ref<BaseAudioContext> context, ConstantSourceOptions const& options)
{
    return realm.create<ConstantSourceNode>(realm, context, options);
}

void ConstantSourceNode::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ConstantSourceNode);
}

void ConstantSourceNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_offset);
}

}
