/*
 * Copyright (c) 2025, Ladybird contributors
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/FlyString.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibWeb/HTML/Canvas/CanvasSettings.h>

// https://html.spec.whatwg.org/multipage/canvas.html#canvasrenderingcontext2dsettings
JS::ThrowCompletionOr<Web::HTML::CanvasRenderingContext2DSettings> Web::HTML::CanvasRenderingContext2DSettings::from_js_value(JS::VM& vm, JS::Value value)
{
    if (!value.is_nullish() && !value.is_object())
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "CanvasRenderingContext2DSettings");

    CanvasRenderingContext2DSettings settings;
    if (value.is_nullish())
        return settings;

    auto& value_object = value.as_object();

    JS::Value alpha = TRY(value_object.get("alpha"_fly_string));
    settings.alpha = alpha.is_undefined() ? true : alpha.to_boolean();

    JS::Value desynchronized = TRY(value_object.get("desynchronized"_fly_string));
    settings.desynchronized = desynchronized.is_undefined() ? false : desynchronized.to_boolean();

    JS::Value color_space = TRY(value_object.get("colorSpace"_fly_string));
    if (!color_space.is_undefined()) {
        auto color_space_string = TRY(color_space.to_string(vm));
        if (color_space_string == "srgb"sv)
            settings.color_space = Bindings::PredefinedColorSpace::Srgb;
        else if (color_space_string == "display-p3"sv)
            settings.color_space = Bindings::PredefinedColorSpace::DisplayP3;
        else
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::InvalidEnumerationValue, color_space_string, "colorSpace");
    }

    JS::Value color_type = TRY(value_object.get("colorType"_fly_string));
    if (!color_type.is_undefined()) {
        auto color_type_string = TRY(color_type.to_string(vm));
        if (color_type_string == "unorm8"sv)
            settings.color_type = Bindings::CanvasColorType::Unorm8;
        else if (color_type_string == "float16"sv)
            settings.color_type = Bindings::CanvasColorType::Float16;
        else
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::InvalidEnumerationValue, color_type_string, "colorType");
    }

    JS::Value will_read_frequently = TRY(value_object.get("willReadFrequently"_fly_string));
    settings.will_read_frequently = will_read_frequently.is_undefined() ? false : will_read_frequently.to_boolean();

    return settings;
}
