/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/AbstractRange.h>

namespace Web::DOM {

// NOTE: We must use GCP instead of NNGCP here, otherwise the generated code cannot default initialize this struct.
//       They will never be null, as they are marked as required and non-null in the dictionary.
struct StaticRangeInit {
    JS::GCPtr<Node> start_container;
    u32 start_offset { 0 };
    JS::GCPtr<Node> end_container;
    u32 end_offset { 0 };
};

class StaticRange final : public AbstractRange {
    WEB_PLATFORM_OBJECT(StaticRange, AbstractRange);
    JS_DECLARE_ALLOCATOR(StaticRange);

public:
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<StaticRange>> construct_impl(JS::Realm&, StaticRangeInit& init);

    StaticRange(Node& start_container, u32 start_offset, Node& end_container, u32 end_offset);
    virtual ~StaticRange() override;

    virtual void initialize(JS::Realm&) override;
};

}
