#include <AK/StringBuilder.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/MediaSession/Utils.h>

namespace Web::MediaSession {

// https://www.w3.org/TR/mediasession/#convert-artwork-algorithm
WebIDL::ExceptionOr<Vector<MediaImage>> convert_artwork(Vector<MediaImage> const& artwork) {
    // 1. Let output be an empty list of type MediaImage.
    Vector<MediaImage> output;
    // 2. For each entry in input (which is a MediaImage list), perform the following steps:
    for (auto entry : artwork) {
        // 1. Let image be a new MediaImage.
        MediaImage image;
        // 2. Let baseURL be the API base URL specified by the entry settings object.
        auto baseURL = HTML::entry_settings_object().api_base_url();
        // 3. Parse entry’s src using baseURL. If it does not return failure, set image’s src to the return value. Otherwise, throw a TypeError and abort these steps.
        auto final_url = DOMURL::parse(entry.src.value(), baseURL);
        // FIXME: check if entry.src is null
        if (!final_url.has_value())
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "could not parse entry's src using baseURL"sv };

        image.src = final_url.release_value().to_string();
        // 4. Set image’s sizes to entry’s sizes.
        image.sizes = entry.sizes;
        // 5. Set image’s type to entry’s type.
        image.type = entry.type;
        // 6. Append image to the output.
        output.append(image);
    }

    // 3. Return output as result.
    return output;
}

WebIDL::ExceptionOr<GC::RootVector<JS::Object*>> convert_artwork_to_js(JS::Realm& realm, Vector<MediaImage> const& artwork) {
    GC::RootVector<JS::Object*> artwork_js { realm.heap() };

    auto& vm = realm.vm();

    for (auto const& image : artwork) {
        auto image_js_obj = JS::Object::create(realm, nullptr);

        if (!image.src.has_value())
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "MediaImage must have non-null src"sv };
        image_js_obj->define_direct_property("src"_utf16, JS::PrimitiveString::create(vm, image.src.value()), JS::default_attributes);
        image_js_obj->define_direct_property("sizes"_utf16, JS::PrimitiveString::create(vm, image.sizes), JS::default_attributes);
        image_js_obj->define_direct_property("type"_utf16, JS::PrimitiveString::create(vm, image.type), JS::default_attributes);
        //    image_js_obj->define_direct_property("type"_utf16, JS::js_null(), JS::default_attributes);

        artwork_js.append(image_js_obj);
    }

    return artwork_js;
}

WebIDL::ExceptionOr<Vector<MediaImage>> convert_artwork_to_cpp(Vector<GC::Root<JS::Object>> const& artwork_obj) {
    Vector<MediaImage> artwork;

    for (auto const& image_obj : artwork_obj) {
        MediaImage image;

        auto src_val = TRY(image_obj->get("src"_utf16));
        auto sizes_val = TRY(image_obj->get("sizes"_utf16));
        auto type_val = TRY(image_obj->get("types"_utf16));

        if (!src_val.is_string()) {
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "src must be a string and not null"sv };
        }

        image.src = src_val.as_string().utf8_string();

        if (sizes_val.is_string())
            // FIXME: use utf16
            image.sizes = sizes_val.as_string().utf8_string();

        if (type_val.is_string())
            // FIXME: use utf16
            image.type = type_val.as_string().utf8_string();
    }


    return artwork;

}

}
