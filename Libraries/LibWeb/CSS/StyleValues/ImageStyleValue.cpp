/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Weak.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/Fetch.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/URLStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/HTML/PotentialCORSRequest.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/SharedResourceRequest.h>

namespace Web::CSS {

StyleValueFFI::StyleValueData const* ImageStyleValue::make_image_url_data(URL const& url, Optional<::URL::URL> const& style_resource_base_url, Optional<bool> parent_style_sheet_origin_clean, bool should_absolutize_url_for_computed_value)
{
    // The Rust allocation takes ownership of one leaked reference to each retained string.
    auto modifiers = retain_url_modifiers_for_rust(url);
    auto url_string = url.url();
    auto url_bytes = url_string.bytes();
    auto resource_base_url = style_resource_base_url.has_value() ? style_resource_base_url->to_string() : String {};
    auto resource_base_url_bytes = resource_base_url.bytes();
    return StyleValueFFI::rust_style_value_create_image(
        url_string.to_raw_leaked(), url_bytes.data(), url_bytes.size(), to_underlying(url.type()), modifiers.data(), modifiers.size(),
        resource_base_url.to_raw_leaked(), resource_base_url_bytes.data(), resource_base_url_bytes.size(), style_resource_base_url.has_value(),
        parent_style_sheet_origin_clean.has_value(), parent_style_sheet_origin_clean.value_or(false), should_absolutize_url_for_computed_value);
}

URL ImageStyleValue::url_value() const
{
    auto const& data = m_value->image;
    return url_from_rust_data(data.url, data.url_type, data.url_modifiers);
}

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
    VERIFY(m_registration_counts_by_image_style_value.is_empty());
    unregister_with_decoded_image_data_if_needed();
}

void ImageStyleValueResource::visit_edges(JS::Cell::Visitor& visitor)
{
    visitor.visit(m_resource_request);
}

void ImageStyleValueResource::register_image_style_value(ImageStyleValue const& image_style_value)
{
    ++m_registration_counts_by_image_style_value.ensure(&image_style_value);
    register_with_decoded_image_data_if_needed();
}

void ImageStyleValueResource::unregister_image_style_value(ImageStyleValue const& image_style_value)
{
    auto it = m_registration_counts_by_image_style_value.find(&image_style_value);
    VERIFY(it != m_registration_counts_by_image_style_value.end());
    if (--it->value == 0)
        m_registration_counts_by_image_style_value.remove(it);
    if (m_registration_counts_by_image_style_value.is_empty())
        unregister_with_decoded_image_data_if_needed();
}

::URL::URL const& ImageStyleValueResource::url() const
{
    return m_resource_request->url();
}

GC::Ptr<HTML::DecodedImageData> ImageStyleValueResource::decoded_image_data() const
{
    return m_resource_request->image_data();
}

void ImageStyleValueResource::on_decoded_image_data_loaded()
{
    notify_image_style_values_did_update();
    if (!m_registration_counts_by_image_style_value.is_empty())
        register_with_decoded_image_data_if_needed();
}

void ImageStyleValueResource::notify_image_style_values_did_update()
{
    for (auto const& registration : m_registration_counts_by_image_style_value)
        registration.key->notify_clients_did_update();
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

ImageStyleValue::ImageStyleValue(URL const& url, Optional<::URL::URL> style_resource_base_url, Optional<bool> parent_style_sheet_origin_clean, bool should_absolutize_url_for_computed_value)
    : AbstractImageStyleValue(Type::Image, make_image_url_data(url, style_resource_base_url, parent_style_sheet_origin_clean, should_absolutize_url_for_computed_value))
    , m_style_resource_base_url(move(style_resource_base_url))
    , m_parent_style_sheet_origin_clean(parent_style_sheet_origin_clean)
    , m_should_absolutize_url_for_computed_value(should_absolutize_url_for_computed_value)
{
}

ImageStyleValue::ImageStyleValue(StyleValueFFI::StyleValueData const* data)
    : AbstractImageStyleValue(Type::Image, data)
{
    auto const& context = data->image.resource_context;
    if (context.has_base_url) {
        auto serialized_base_url = string_from_rust_data(context.base_url);
        m_style_resource_base_url = DOMURL::parse_from_byte_string(serialized_base_url.bytes_as_string_view());
    }
    if (context.has_parent_style_sheet_origin_clean)
        m_parent_style_sheet_origin_clean = context.parent_style_sheet_origin_clean;
    m_should_absolutize_url_for_computed_value = context.should_absolutize_url_for_computed_value;
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

    return fetch_an_external_image_for_a_stylesheet(url_value(), rule_or_declaration, document);
}

void ImageStyleValue::load_any_resources(DOM::Document& document)
{
    fetch_image(document);
}

void ImageStyleValue::set_style_sheet(GC::Ptr<CSSStyleSheet> style_sheet)
{

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
    m_style_resource_base_url = style_sheet.style_resource_base_url();
    m_parent_style_sheet_origin_clean = style_sheet.is_origin_clean();
    m_should_absolutize_url_for_computed_value = true;
}

ValueComparingNonnullRefPtr<StyleValue const> ImageStyleValue::absolutized(ComputationContext const& context) const
{
    // NB: Materialize the URL once; rebuilding it per use re-marshals the string and modifier list each time.
    auto url_value = this->url_value();

    if (url_value.url().is_empty())
        return *this;

    // FIXME: The spec has been updated to handle this better. The computation of the base URL here is roughly based on:
    //        https://drafts.csswg.org/css-values-4/#style-resource-base-url
    //        https://github.com/w3c/csswg-drafts/pull/12261
    auto base_url = m_style_resource_base_url;
    if (!base_url.has_value() && context.abstract_element.has_value())
        base_url = context.abstract_element->document().base_url();

    if (base_url.has_value()) {
        if (m_should_absolutize_url_for_computed_value) {
            if (DOMURL::parse_from_byte_string(url_value.url().bytes_as_string_view()).has_value()) {
                auto absolutized_image = adopt_ref(*new (nothrow) ImageStyleValue(url_value, *base_url, m_parent_style_sheet_origin_clean, true));
                return absolutized_image;
            }

            if (auto resolved_url = DOMURL::parse_from_byte_string(url_value.url().bytes_as_string_view(), *base_url); resolved_url.has_value()) {
                auto absolutized_image = adopt_ref(*new (nothrow) ImageStyleValue(URL { resolved_url->to_string(), url_value.type(), url_value.request_url_modifiers() }, *base_url, m_parent_style_sheet_origin_clean, true));
                return absolutized_image;
            }

            return *this;
        }

        auto absolutized_image = adopt_ref(*new (nothrow) ImageStyleValue(url_value, *base_url, m_parent_style_sheet_origin_clean));
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

    ImageStyleValueResource* resource = document->css_image_resource(*resolved_url);
    if (!resource) {
        auto resource_request = fetch_image(*document);

        // NB: This can only fail if the URL is invalid or ResourceLoader is not initialized, neither of which should be
        //     the case here.
        VERIFY(resource_request);

        resource = &document->create_css_image_resource(*resource_request);
    }

    resource->register_image_style_value(*this);
    client.m_resource = resource;
}

void ImageStyleValue::unregister_client(Client& client) const
{
    auto did_remove = m_clients.remove(&client);
    VERIFY(did_remove);

    auto* resource = exchange(client.m_resource, nullptr);
    if (!resource)
        return;

    // The document owns the resource, so a collected document has already destroyed it.
    auto document = client.document();
    if (!document)
        return;

    auto url = resource->url();
    resource->unregister_image_style_value(*this);
    document->remove_css_image_resource_if_unused(url);
}

void ImageStyleValue::notify_clients_did_update() const
{
    for (auto* client : m_clients)
        client->image_style_value_did_update(const_cast<ImageStyleValue&>(*this));
}

Optional<::URL::URL> ImageStyleValue::resolved_url(DOM::Document const& document) const
{
    auto url = url_value().url();
    if (url.is_empty())
        return {};

    return DOMURL::parse_from_byte_string(url.bytes_as_string_view(), style_resource_base_url(document));
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

GC::Ptr<HTML::DecodedImageData> ImageStyleValue::Client::decoded_image_data() const
{
    if (!m_resource)
        return nullptr;
    return m_resource->decoded_image_data();
}

}
