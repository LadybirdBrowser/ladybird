/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/ValueInlines.h>
#include <LibWeb/Bindings/HTMLImageElement.h>
#include <LibWeb/Bindings/ImageConstructor.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/WebIDL/ExceptionOrUtils.h>

namespace Web::Bindings {

GC_DEFINE_ALLOCATOR(ImageConstructor);

ImageConstructor::ImageConstructor(JS::Realm& realm)
    : NativeFunction(realm.intrinsics().function_prototype())
{
}

void ImageConstructor::initialize(JS::Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    define_direct_property(vm.names.length, JS::Value(0), JS::Attribute::Configurable);
    define_direct_property(vm.names.name, JS::PrimitiveString::create(vm, "Image"_string), JS::Attribute::Configurable);
    define_direct_property(vm.names.prototype, &ensure_web_prototype<Bindings::HTMLImageElementPrototype>(realm, "HTMLImageElement"_fly_string), 0);
}

JS::ThrowCompletionOr<JS::Value> ImageConstructor::call()
{
    return vm().throw_completion<JS::TypeError>(JS::ErrorType::ConstructorWithoutNew, "Image");
}

// https://html.spec.whatwg.org/multipage/embedded-content.html#dom-image
// https://webidl.spec.whatwg.org/#legacy-factory-functions
JS::ThrowCompletionOr<GC::Ref<JS::Object>> ImageConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();
    auto& realm = *this->realm();

    // 1. Let document be the current global object's associated Document.
    auto& window = HTML::current_window();
    auto& document = window.associated_document();

    // 2. Let img be the result of creating an element given document, "img", and the HTML namespace.
    auto image_element = TRY(WebIDL::throw_dom_exception_if_needed(vm, realm, [&]() { return DOM::create_element(document, HTML::TagNames::img, Namespace::HTML); }));
    auto& html_image_element = as<HTML::HTMLImageElement>(*image_element);
    auto wrapped_image_element = Bindings::wrap(host_defined_wrapper_world(realm), realm, GC::Ref { html_image_element });

    // https://webidl.spec.whatwg.org/#internally-create-a-new-object-implementing-the-interface
    TRY(set_prototype_from_new_target<HTMLImageElementPrototype>(vm, new_target, "HTMLImageElement"_fly_string, *wrapped_image_element));

    // 3. If width is given, then set an attribute value for img using "width" and width.
    if (vm.argument_count() > 0) {
        u32 width = TRY(vm.argument(0).to_u32(vm));
        image_element->set_attribute_value(HTML::AttributeNames::width, MUST(String::formatted("{}", width)));
    }

    // 4. If height is given, then set an attribute value for img using "height" and height.
    if (vm.argument_count() > 1) {
        u32 height = TRY(vm.argument(1).to_u32(vm));
        image_element->set_attribute_value(HTML::AttributeNames::height, MUST(String::formatted("{}", height)));
    }

    // 5. Return img.
    return GC::Ref<JS::Object> { *wrapped_image_element };
}

}
