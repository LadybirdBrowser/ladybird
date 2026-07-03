/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <LibGC/Weak.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/Fetch.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/HTML/AnimatedBitmapDecodedImageData.h>
#include <LibWeb/HTML/BitmapDecodedImageData.h>
#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/HTML/PotentialCORSRequest.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/SharedResourceRequest.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/DisplayListRecordingContext.h>

namespace Web::CSS {

ImageStyleValueResource::ImageStyleValueResource(GC::Ref<HTML::SharedResourceRequest> request, GC::Ref<DOM::Document> const& document)
    : m_resource_request(move(request))
{
    m_resource_request->add_callbacks(
        [weak_document = GC::Weak(document), url = m_resource_request->url()] {
            // FIXME: Can we directly access the resource (i.e. this) here instead of looking it up in the document?
            if (auto document = weak_document.ptr()) {
                if (auto* resource = document->css_image_resource(url))
                    resource->on_decoded_image_data_loaded();
            }
        },
        nullptr);
}

ImageStyleValueResource::~ImageStyleValueResource()
{
    VERIFY(m_image_style_values.is_empty());
    unregister_with_decoded_image_data_if_needed();
}

void ImageStyleValueResource::visit_edges(JS::Cell::Visitor& visitor)
{
    visitor.visit(m_resource_request);
}

void ImageStyleValueResource::register_image_style_value(ImageStyleValue const& image_style_value)
{
    m_image_style_values.set(&image_style_value);
    register_with_decoded_image_data_if_needed();
}

void ImageStyleValueResource::unregister_image_style_value(ImageStyleValue const& image_style_value)
{
    m_image_style_values.remove(&image_style_value);
    if (m_image_style_values.is_empty())
        unregister_with_decoded_image_data_if_needed();
}

GC::Ptr<HTML::DecodedImageData> ImageStyleValueResource::decoded_image_data() const
{
    return m_resource_request->image_data();
}

void ImageStyleValueResource::on_decoded_image_data_loaded()
{
    notify_image_style_values_did_update();
    if (!m_image_style_values.is_empty())
        register_with_decoded_image_data_if_needed();
}

void ImageStyleValueResource::notify_image_style_values_did_update()
{
    for (auto const* image_style_value : m_image_style_values)
        image_style_value->notify_clients_did_update();
}

ValueComparingNonnullRefPtr<ImageStyleValue const> ImageStyleValue::create(URL const& url)
{
    return adopt_ref(*new (nothrow) ImageStyleValue(url));
}

ValueComparingNonnullRefPtr<ImageStyleValue const> ImageStyleValue::create(URL const& url, Optional<::URL::URL> style_resource_base_url)
{
    return adopt_ref(*new (nothrow) ImageStyleValue(url, move(style_resource_base_url)));
}

ValueComparingNonnullRefPtr<ImageStyleValue const> ImageStyleValue::create(::URL::URL const& url)
{
    return adopt_ref(*new (nothrow) ImageStyleValue(URL { url.to_string() }));
}

ImageStyleValue::ImageStyleValue(URL const& url, Optional<::URL::URL> style_resource_base_url)
    : AbstractImageStyleValue(Type::Image)
    , m_url(url)
    , m_style_resource_base_url(move(style_resource_base_url))
{
}

ImageStyleValue::~ImageStyleValue() = default;

GC::Ptr<HTML::SharedResourceRequest> ImageStyleValue::fetch_image(DOM::Document& document) const
{
    RuleOrDeclaration rule_or_declaration {
        .environment_settings_object = document.relevant_settings_object(),
        .value = RuleOrDeclaration::Rule {},
        .style_resource_base_url = m_style_resource_base_url,
        .parent_style_sheet_origin_clean = m_parent_style_sheet_origin_clean,
    };

    return fetch_an_external_image_for_a_stylesheet(m_url, rule_or_declaration, document);
}

void ImageStyleValue::load_any_resources(DOM::Document& document)
{
    fetch_image(document);
}

void ImageStyleValue::serialize(StringBuilder& builder, SerializationMode) const
{
    builder.append(m_url.to_string());
}

bool ImageStyleValue::equals(StyleValue const& other) const
{
    if (type() != other.type())
        return false;
    return m_url == other.as_image().m_url;
}

Optional<CSSPixels> ImageStyleValue::natural_width(DOM::Document const& document) const
{
    if (auto image_data = this->image_data(document))
        return image_data->intrinsic_width();
    return {};
}

Optional<CSSPixels> ImageStyleValue::natural_height(DOM::Document const& document) const
{
    if (auto image_data = this->image_data(document))
        return image_data->intrinsic_height();
    return {};
}

Optional<CSSPixelFraction> ImageStyleValue::natural_aspect_ratio(DOM::Document const& document) const
{
    if (auto image_data = this->image_data(document))
        return image_data->intrinsic_aspect_ratio();
    return {};
}

bool ImageStyleValue::is_paintable(DOM::Document const& document) const
{
    return image_data(document);
}

void ImageStyleValue::paint(DisplayListRecordingContext& context, DOM::Document const& document, DevicePixelRect const& dest_rect, CSS::ImageRendering image_rendering) const
{
    auto image_data = this->image_data(document);
    if (!image_data)
        return;

    auto dest_int_rect = dest_rect.to_type<int>();
    image_data->paint(context, dest_int_rect, image_rendering);
}

Optional<Painting::DisplayListResource> ImageStyleValue::record_display_list(DisplayListRecordingContext& context, DOM::Document const& document, DevicePixelRect const& dest_rect) const
{
    auto image_data = this->image_data(document);
    if (!image_data)
        return {};

    return image_data->record_display_list(dest_rect.size().to_type<int>(), context.display_list_recorder().resource_storage());
}

Optional<Gfx::DecodedImageFrame> ImageStyleValue::current_frame(DOM::Document const& document, DevicePixelRect const& dest_rect) const
{
    if (auto image_data = this->image_data(document))
        return image_data->current_frame(dest_rect.size().to_type<int>());
    return {};
}

GC::Ptr<HTML::DecodedImageData> ImageStyleValue::image_data(DOM::Document const& document) const
{
    auto resolved_url = this->resolved_url(document);
    if (!resolved_url.has_value())
        return nullptr;

    auto const* resource = document.css_image_resource(*resolved_url);

    // NB: We should have registered a client for this before now which ensures a resource exists if we have a valid
    //     resolved URL.
    VERIFY(resource);

    return resource->decoded_image_data();
}

Optional<Gfx::Color> ImageStyleValue::color_if_single_pixel_bitmap(DOM::Document const& document) const
{
    auto image_data = this->image_data(document);
    if (!image_data)
        return {};

    if (!is<HTML::BitmapDecodedImageData>(*image_data) && !is<HTML::AnimatedBitmapDecodedImageData>(*image_data))
        return {};

    auto decoded_frame = image_data->current_frame();
    if (!decoded_frame.has_value())
        return {};

    auto const& bitmap = decoded_frame->bitmap();
    if (bitmap.width() == 1 && bitmap.height() == 1)
        return bitmap.get_pixel(0, 0);

    return {};
}

void ImageStyleValue::set_style_sheet(GC::Ptr<CSSStyleSheet> style_sheet)
{
    Base::set_style_sheet(style_sheet);

    m_style_resource_base_url.clear();
    m_parent_style_sheet_origin_clean.clear();
    m_should_absolutize_url_for_computed_value = false;

    if (style_sheet) {
        update_style_sheet_resource_context(*style_sheet);
        style_sheet->register_pending_image_value(*this);
    }
}

void ImageStyleValue::update_style_sheet_resource_context(CSSStyleSheet const& style_sheet)
{
    m_style_resource_base_url = style_sheet.base_url()
                                    .value_or_lazy_evaluated_optional([&]() { return style_sheet.location(); })
                                    .value_or_lazy_evaluated_optional([&]() { return HTML::relevant_settings_object(style_sheet).api_base_url(); });
    m_parent_style_sheet_origin_clean = style_sheet.is_origin_clean();
    m_should_absolutize_url_for_computed_value = true;
}

ValueComparingNonnullRefPtr<StyleValue const> ImageStyleValue::absolutized(ComputationContext const& context) const
{
    if (m_url.url().is_empty())
        return *this;

    // FIXME: The spec has been updated to handle this better. The computation of the base URL here is roughly based on:
    //        https://drafts.csswg.org/css-values-4/#style-resource-base-url
    //        https://github.com/w3c/csswg-drafts/pull/12261
    auto base_url = m_style_resource_base_url;
    if (!base_url.has_value() && context.abstract_element.has_value())
        base_url = context.abstract_element->document().base_url();

    if (base_url.has_value()) {
        if (m_should_absolutize_url_for_computed_value) {
            if (DOMURL::parse(m_url.url()).has_value()) {
                auto absolutized_image = adopt_ref(*new (nothrow) ImageStyleValue(m_url, *base_url));
                absolutized_image->m_parent_style_sheet_origin_clean = m_parent_style_sheet_origin_clean;
                absolutized_image->m_should_absolutize_url_for_computed_value = true;
                return absolutized_image;
            }

            if (auto resolved_url = DOMURL::parse(m_url.url(), *base_url); resolved_url.has_value()) {
                auto absolutized_image = adopt_ref(*new (nothrow) ImageStyleValue(URL { resolved_url->to_string(), m_url.type(), m_url.request_url_modifiers() }, *base_url));
                absolutized_image->m_parent_style_sheet_origin_clean = m_parent_style_sheet_origin_clean;
                absolutized_image->m_should_absolutize_url_for_computed_value = true;
                return absolutized_image;
            }

            return *this;
        }

        auto absolutized_image = adopt_ref(*new (nothrow) ImageStyleValue(m_url, *base_url));
        absolutized_image->m_parent_style_sheet_origin_clean = m_parent_style_sheet_origin_clean;
        return absolutized_image;
    }

    return *this;
}

void ImageStyleValue::register_client(Client& client) const
{
    auto result = m_clients.set(&client);
    VERIFY(result == AK::HashSetResult::InsertedNewEntry);
    auto document = client.document();
    if (!document)
        return;

    auto resolved_url = this->resolved_url(*document);
    if (!resolved_url.has_value())
        return;

    // NB: Store the resolved URL so that we can unregister from the resource later even if the document's base URL
    //     changes in the interim.
    client.m_registered_url = *resolved_url;

    GC::Ptr<CSS::ImageStyleValueResource> resource;

    if (auto* existing_resource = document->css_image_resource(*resolved_url)) {
        resource = existing_resource;
    } else {
        auto resource_request = fetch_image(*document);

        // NB: This can only fail if the URL is invalid or ResourceLoader is not initialized, neither of which should be
        //     the case here.
        VERIFY(resource_request);

        resource = document->create_css_image_resource(*resource_request);
    }

    resource->register_image_style_value(*this);
}

void ImageStyleValue::unregister_client(Client& client) const
{
    auto document = client.document();
    auto registered_url = move(client.m_registered_url);
    client.m_registered_url.clear();

    auto did_remove = m_clients.remove(&client);
    VERIFY(did_remove);

    if (!document || !registered_url.has_value())
        return;

    if (any_of(m_clients, [&](auto const* remaining_client) {
            return remaining_client->document() == document
                && remaining_client->m_registered_url.has_value()
                && *remaining_client->m_registered_url == *registered_url;
        }))
        return;

    if (auto* resource = document->css_image_resource(*registered_url))
        resource->unregister_image_style_value(*this);
    document->remove_css_image_resource_if_unused(*registered_url);
}

void ImageStyleValue::notify_clients_did_update() const
{
    for (auto* client : m_clients)
        client->image_style_value_did_update(const_cast<ImageStyleValue&>(*this));
}

Optional<::URL::URL> ImageStyleValue::resolved_url(DOM::Document const& document) const
{
    if (m_url.url().is_empty())
        return {};

    return DOMURL::parse(m_url.url(), style_resource_base_url(document));
}

::URL::URL ImageStyleValue::style_resource_base_url(DOM::Document const& document) const
{
    return m_style_resource_base_url.value_or_lazy_evaluated([&] {
        return document.relevant_settings_object().api_base_url();
    });
}

ImageStyleValue::Client::Client(DOM::Document& document, ImageStyleValue const& image_style_value)
    : m_image_style_value(image_style_value)
    , m_document(document)
{
    m_image_style_value.register_client(*this);
}

ImageStyleValue::Client::~Client()
{
}

void ImageStyleValue::Client::image_style_value_finalize()
{
    m_image_style_value.unregister_client(*this);
}

}
