/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/AudioConstructor.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/HTMLAudioElementPrototype.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Namespace.h>

namespace Web::Bindings {

GC_DEFINE_ALLOCATOR(AudioConstructor);

AudioConstructor::AudioConstructor(JS::Realm& realm)
    : NativeFunction(realm.intrinsics().function_prototype())
{
}

void AudioConstructor::initialize(JS::Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    define_direct_property(vm.names.length, JS::Value(0), JS::Attribute::Configurable);
    define_direct_property(vm.names.name, JS::PrimitiveString::create(vm, "Audio"_string), JS::Attribute::Configurable);
    define_direct_property(vm.names.prototype, &ensure_web_prototype<Bindings::HTMLAudioElementPrototype>(realm, "HTMLAudioElement"_fly_string), 0);
}

JS::ThrowCompletionOr<JS::Value> AudioConstructor::call()
{
    return vm().throw_completion<JS::TypeError>(JS::ErrorType::ConstructorWithoutNew, "Audio");
}

// https://html.spec.whatwg.org/multipage/media.html#dom-audio
JS::ThrowCompletionOr<GC::Ref<JS::Object>> AudioConstructor::construct(FunctionObject&)
{
    auto& vm = this->vm();

    // 1. Let document be the current global object's associated Document.
    auto& window = as<HTML::Window>(HTML::current_principal_global_object());
    auto& document = window.associated_document();

    // 2. Let audio be the result of creating an element given document, "audio", and the HTML namespace.
    auto audio = TRY(Bindings::throw_dom_exception_if_needed(vm, [&]() { return DOM::create_element(document, HTML::TagNames::audio, Namespace::HTML); }));

    // 3. Set an attribute value for audio using "preload" and "auto".
    MUST(audio->set_attribute(HTML::AttributeNames::preload, "auto"_string));

    auto src_value = vm.argument(0);

    // 4. If src is given, then set an attribute value for audio using "src" and src.
    //    (This will cause the user agent to invoke the object's resource selection algorithm before returning.)
    if (!src_value.is_undefined()) {
        auto src = TRY(src_value.to_string(vm));
        MUST(audio->set_attribute(HTML::AttributeNames::src, move(src)));
    }

    // 5. Return audio.
    return audio;
}

}
