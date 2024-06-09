/*
 * Copyright (c) 2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Function.h>
#include <LibCore/MimeData.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/Loader/Resource.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/Platform/EventLoopPlugin.h>

namespace Web {

NonnullRefPtr<Resource> Resource::create(Badge<ResourceLoader>, Type type, LoadRequest const& request)
{
    return adopt_ref(*new Resource(type, request));
}

Resource::Resource(Type type, LoadRequest const& request)
    : m_request(request)
    , m_type(type)
{
}

Resource::Resource(Type type, Resource& resource)
    : m_request(resource.m_request)
    , m_encoded_data(move(resource.m_encoded_data))
    , m_type(type)
    , m_state(resource.m_state)
    , m_error(move(resource.m_error))
    , m_encoding(move(resource.m_encoding))
    , m_mime_type(move(resource.m_mime_type))
    , m_response_headers(move(resource.m_response_headers))
    , m_status_code(move(resource.m_status_code))
{
    ResourceLoader::the().evict_from_cache(m_request);
}

Resource::~Resource() = default;

void Resource::for_each_client(Function<void(ResourceClient&)> callback)
{
    Vector<WeakPtr<ResourceClient>, 16> clients_copy;
    clients_copy.ensure_capacity(m_clients.size());
    for (auto* client : m_clients)
        clients_copy.append(client->make_weak_ptr());
    for (auto client : clients_copy) {
        if (client)
            callback(*client);
    }
}

static Optional<ByteString> encoding_from_content_type(ByteString const& content_type)
{
    auto offset = content_type.find("charset="sv);
    if (offset.has_value()) {
        auto encoding = content_type.substring(offset.value() + 8, content_type.length() - offset.value() - 8).to_lowercase();
        if (encoding.length() >= 2 && encoding.starts_with('"') && encoding.ends_with('"'))
            return encoding.substring(1, encoding.length() - 2);
        if (encoding.length() >= 2 && encoding.starts_with('\'') && encoding.ends_with('\''))
            return encoding.substring(1, encoding.length() - 2);
        return encoding;
    }

    return {};
}

static ByteString mime_type_from_content_type(ByteString const& content_type)
{
    auto offset = content_type.find(';');
    if (offset.has_value())
        return content_type.substring(0, offset.value()).to_lowercase();

    return content_type;
}

static bool is_valid_encoding(StringView encoding)
{
    return TextCodec::decoder_for(encoding).has_value();
}

void Resource::did_load(Badge<ResourceLoader>, ReadonlyBytes data, HTTP::HeaderMap const& headers, Optional<u32> status_code)
{
    VERIFY(m_state == State::Pending);
    // FIXME: Handle OOM failure.
    m_encoded_data = ByteBuffer::copy(data).release_value_but_fixme_should_propagate_errors();
    m_response_headers = headers;
    m_status_code = move(status_code);
    m_state = State::Loaded;

    auto content_type = headers.get("Content-Type");

    if (content_type.has_value()) {
        dbgln_if(RESOURCE_DEBUG, "Content-Type header: '{}'", content_type.value());
        m_mime_type = mime_type_from_content_type(content_type.value());
        // FIXME: "The Quite OK Image Format" doesn't have an official mime type yet,
        //        and servers like nginx will send a generic octet-stream mime type instead.
        //        Let's use image/x-qoi for now, which is also what our Core::MimeData uses & would guess.
        if (m_mime_type == "application/octet-stream" && url().serialize_path().ends_with(".qoi"sv))
            m_mime_type = "image/x-qoi";
    } else {
        auto content_type_options = headers.get("X-Content-Type-Options");
        if (content_type_options.value_or("").equals_ignoring_ascii_case("nosniff"sv)) {
            m_mime_type = "text/plain";
        } else {
            m_mime_type = Core::guess_mime_type_based_on_filename(url().serialize_path());
        }
    }

    m_encoding = {};
    if (content_type.has_value()) {
        auto encoding = encoding_from_content_type(content_type.value());
        if (encoding.has_value() && is_valid_encoding(encoding.value())) {
            dbgln_if(RESOURCE_DEBUG, "Set encoding '{}' from Content-Type", encoding.value());
            m_encoding = encoding.value();
        }
    }

    for_each_client([](auto& client) {
        client.resource_did_load();
    });
}

void Resource::did_fail(Badge<ResourceLoader>, ByteString const& error, Optional<u32> status_code)
{
    m_error = error;
    m_status_code = move(status_code);
    m_state = State::Failed;

    for_each_client([](auto& client) {
        client.resource_did_fail();
    });
}

void Resource::register_client(Badge<ResourceClient>, ResourceClient& client)
{
    VERIFY(!m_clients.contains(&client));
    m_clients.set(&client);
}

void Resource::unregister_client(Badge<ResourceClient>, ResourceClient& client)
{
    VERIFY(m_clients.contains(&client));
    m_clients.remove(&client);
}

void ResourceClient::set_resource(Resource* resource)
{
    if (m_resource)
        m_resource->unregister_client({}, *this);
    m_resource = resource;
    if (m_resource) {
        VERIFY(resource->type() == client_type());

        m_resource->register_client({}, *this);

        // For resources that are already loaded, we fire their load/fail callbacks via the event loop.
        // This ensures that these callbacks always happen in a consistent way, instead of being invoked
        // synchronously in some cases, and asynchronously in others.
        if (resource->is_loaded() || resource->is_failed()) {
            Platform::EventLoopPlugin::the().deferred_invoke([weak_this = make_weak_ptr(), strong_resource = NonnullRefPtr { *m_resource }] {
                if (!weak_this)
                    return;

                if (weak_this->m_resource != strong_resource.ptr())
                    return;

                // Make sure that reused resources also have their load callback fired.
                if (weak_this->m_resource->is_loaded()) {
                    weak_this->resource_did_load();
                    return;
                }

                // Make sure that reused resources also have their fail callback fired.
                if (weak_this->m_resource->is_failed()) {
                    weak_this->resource_did_fail();
                    return;
                }
            });
        }
    }
}

ResourceClient::~ResourceClient()
{
    if (m_resource)
        m_resource->unregister_client({}, *this);
}

}
