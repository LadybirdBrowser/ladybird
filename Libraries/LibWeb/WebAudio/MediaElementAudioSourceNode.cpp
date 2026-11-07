/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
namespace Web::WebAudio {

extern "C" HTML::HTMLMediaElement* web_audio_media_element_from_wrapper(JS::Object*);
extern "C" JS::Object* web_audio_media_element_wrap(JS::Realm*, HTML::HTMLMediaElement*);
HTML::HTMLMediaElement* web_audio_media_element_from_wrapper(JS::Object* object) { return Bindings::impl_from<HTML::HTMLMediaElement>(object); }
JS::Object* web_audio_media_element_wrap(JS::Realm* realm, HTML::HTMLMediaElement* object) { return Bindings::wrap(Bindings::host_defined_wrapper_world(*realm), *realm, GC::Ref { *object }).ptr(); }

}
