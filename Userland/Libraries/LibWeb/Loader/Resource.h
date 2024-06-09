/*
 * Copyright (c) 2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/Noncopyable.h>
#include <AK/RefCounted.h>
#include <AK/WeakPtr.h>
#include <AK/Weakable.h>
#include <LibGfx/Forward.h>
#include <LibHTTP/HeaderMap.h>
#include <LibURL/URL.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Loader/LoadRequest.h>

namespace Web {

class ResourceClient;

class Resource : public RefCounted<Resource> {
    AK_MAKE_NONCOPYABLE(Resource);
    AK_MAKE_NONMOVABLE(Resource);

public:
    enum class Type {
        Generic,
    };

    static NonnullRefPtr<Resource> create(Badge<ResourceLoader>, Type, LoadRequest const&);
    virtual ~Resource();

    Type type() const { return m_type; }

    enum class State {
        Pending,
        Loaded,
        Failed,
    };

    bool is_pending() const { return m_state == State::Pending; }
    bool is_loaded() const { return m_state == State::Loaded; }
    bool is_failed() const { return m_state == State::Failed; }

    ByteString const& error() const { return m_error; }

    bool has_encoded_data() const { return !m_encoded_data.is_empty(); }

    const URL::URL& url() const { return m_request.url(); }
    ByteBuffer const& encoded_data() const { return m_encoded_data; }

    [[nodiscard]] HTTP::HeaderMap const& response_headers() const { return m_response_headers; }

    [[nodiscard]] Optional<u32> status_code() const { return m_status_code; }

    void register_client(Badge<ResourceClient>, ResourceClient&);
    void unregister_client(Badge<ResourceClient>, ResourceClient&);

    bool has_encoding() const { return m_encoding.has_value(); }
    Optional<ByteString> const& encoding() const { return m_encoding; }
    ByteString const& mime_type() const { return m_mime_type; }

    void for_each_client(Function<void(ResourceClient&)>);

    void did_load(Badge<ResourceLoader>, ReadonlyBytes data, HTTP::HeaderMap const&, Optional<u32> status_code);
    void did_fail(Badge<ResourceLoader>, ByteString const& error, Optional<u32> status_code);

protected:
    explicit Resource(Type, LoadRequest const&);
    Resource(Type, Resource&);

    LoadRequest request() const { return m_request; }

private:
    LoadRequest m_request;
    ByteBuffer m_encoded_data;
    Type m_type { Type::Generic };
    State m_state { State::Pending };
    ByteString m_error;
    Optional<ByteString> m_encoding;

    ByteString m_mime_type;
    HTTP::HeaderMap m_response_headers;
    Optional<u32> m_status_code;
    HashTable<ResourceClient*> m_clients;
};

class ResourceClient : public Weakable<ResourceClient> {
public:
    virtual ~ResourceClient();

    virtual void resource_did_load() { }
    virtual void resource_did_fail() { }

protected:
    virtual Resource::Type client_type() const { return Resource::Type::Generic; }

    Resource* resource() { return m_resource; }
    Resource const* resource() const { return m_resource; }
    void set_resource(Resource*);

private:
    RefPtr<Resource> m_resource;
};

}
