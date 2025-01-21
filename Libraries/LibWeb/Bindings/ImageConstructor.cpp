/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/HTMLImageElementPrototype.h>
#include <LibWeb/Bindings/ImageConstructor.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Namespace.h>

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
// https://whatpr.org/html/9893/embedded-content.html#dom-image
JS::ThrowCompletionOr<GC::Ref<JS::Object>> ImageConstructor::construct(FunctionObject&)
{
    auto& vm = this->vm();

    // 1. Let document be the current principal global object's associated Document.
    auto& window = as<HTML::Window>(HTML::current_principal_global_object());
    auto& document = window.associated_document();

    // 2. Let img be the result of creating an element given document, "img", and the HTML namespace.
    auto image_element = TRY(Bindings::throw_dom_exception_if_needed(vm, [&]() { return DOM::create_element(document, HTML::TagNames::img, Namespace::HTML); }));

    // 3. If width is given, then set an attribute value for img using "width" and width.
    if (vm.argument_count() > 0) {
        u32 width = TRY(vm.argument(0).to_u32(vm));
        MUST(image_element->set_attribute(HTML::AttributeNames::width, MUST(String::formatted("{}", width))));
    }

    // 4. If height is given, then set an attribute value for img using "height" and height.
    if (vm.argument_count() > 1) {
        u32 height = TRY(vm.argument(1).to_u32(vm));
        MUST(image_element->set_attribute(HTML::AttributeNames::height, MUST(String::formatted("{}", height))));
    }

    // 5. Return img.
    return image_element;
}

}
