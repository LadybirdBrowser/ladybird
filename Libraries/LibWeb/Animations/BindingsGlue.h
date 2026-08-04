/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibGC/Forward.h>
#include <LibJS/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Animations {

class Animation;
class AnimationEffect;
class AnimationTimeline;

}

namespace Web::Bindings {

WEB_API GC::Ref<Animations::Animation> construct_animation(JS::Realm&, GC::Ptr<Animations::AnimationEffect>, Optional<GC::Ptr<Animations::AnimationTimeline>>, size_t argument_count);
WEB_API void resolve_animation_promise(WebIDL::Promise&, Animations::Animation const&);
WEB_API GC::Ref<WebIDL::Promise> create_resolved_animation_promise(Animations::Animation const&);

}
