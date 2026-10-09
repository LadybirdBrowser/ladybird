/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>

namespace Web::Fetch::Infrastructure {

// A response as code outside the fetch algorithms reads it.
class WEB_API ResponseView final : public JS::Cell {
    GC_CELL(ResponseView, JS::Cell);
    GC_DECLARE_ALLOCATOR(ResponseView);

public:
    // body_view is the view of the body of record's unsafe response.
    [[nodiscard]] static GC::Ref<ResponseView> create(NonnullRefPtr<Response const> record, GC::Ptr<Body> body_view);
    // The body of record's unsafe response is null or bytes.
    [[nodiscard]] static GC::Ref<ResponseView> create(JS::Realm&, NonnullRefPtr<Response const> record);
    [[nodiscard]] static GC::Ref<ResponseView> network_error(String message);
    [[nodiscard]] static GC::Ref<ResponseView> aborted_network_error();

    [[nodiscard]] NonnullRefPtr<Response const> const& record() const { return m_record; }

    [[nodiscard]] Response::Type type() const { return m_record->type(); }
    [[nodiscard]] bool aborted() const { return m_record->aborted(); }
    [[nodiscard]] Vector<URL::URL> const& url_list() const { return m_record->url_list(); }
    [[nodiscard]] Status status() const { return m_record->status(); }
    [[nodiscard]] ByteString const& status_message() const { return m_record->status_message(); }
    [[nodiscard]] NonnullRefPtr<HTTP::HeaderList> const& header_list() const { return m_record->header_list(); }
    [[nodiscard]] GC::Ptr<Body> body() const { return m_record->body().has_value() ? m_body_view : nullptr; }
    [[nodiscard]] Optional<u64> body_length() const;
    [[nodiscard]] Optional<Core::ImmutableBytes> body_source() const;
    [[nodiscard]] Optional<Response::CacheState> const& cache_state() const { return m_record->cache_state(); }
    [[nodiscard]] bool timing_allow_passed() const { return m_record->timing_allow_passed(); }
    [[nodiscard]] Vector<Vector<String>> const& navigation_timing_allow_values_list() const { return m_record->navigation_timing_allow_values_list(); }
    [[nodiscard]] Response::BodyInfo const& body_info() const { return m_record->body_info(); }
    [[nodiscard]] RedirectTaint redirect_taint() const { return m_record->redirect_taint(); }
    [[nodiscard]] Optional<Core::ImmutableBytes> const& javascript_bytecode_cache() const { return m_record->javascript_bytecode_cache(); }
    [[nodiscard]] Optional<u64> javascript_bytecode_cache_vary_key() const { return m_record->javascript_bytecode_cache_vary_key(); }
    [[nodiscard]] Optional<NonnullRefPtr<HTTP::HeaderList>> const& javascript_bytecode_cache_memory_cache_request_headers() const { return m_record->javascript_bytecode_cache_memory_cache_request_headers(); }

    [[nodiscard]] bool is_aborted_network_error() const { return m_record->is_aborted_network_error(); }
    [[nodiscard]] bool is_network_error() const { return m_record->is_network_error(); }
    [[nodiscard]] Optional<URL::URL const&> url() const { return m_record->url(); }
    [[nodiscard]] ErrorOr<Optional<URL::URL>> location_url(Optional<StringView> request_fragment) const { return m_record->location_url(request_fragment); }
    [[nodiscard]] bool is_cors_same_origin() const { return m_record->is_cors_same_origin(); }
    [[nodiscard]] bool is_cors_cross_origin() const { return m_record->is_cors_cross_origin(); }
    [[nodiscard]] Optional<String> const& network_error_message() const { return m_record->network_error_message(); }
    [[nodiscard]] MonotonicTime monotonic_response_time() const { return m_record->monotonic_response_time(); }
    [[nodiscard]] Optional<Response::NetworkTransport> const& network_transport() const { return m_record->network_transport(); }

    [[nodiscard]] GC::Ref<ResponseView> unsafe_response();
    [[nodiscard]] GC::Ref<ResponseView> clone(JS::Realm&) const;

    void release_request_transfer_lease() const { m_record->release_request_transfer_lease(); }
    void resume_body_delivery() const { m_record->resume_body_delivery(); }
    void resume_body_delivery_up_to(size_t byte_count) const { m_record->resume_body_delivery_up_to(byte_count); }
    [[nodiscard]] Vector<Core::ImmutableBytes> release_body_for_transfer() const { return m_record->release_body_for_transfer(); }

private:
    ResponseView(NonnullRefPtr<Response const>, GC::Ptr<Body>);

    virtual void visit_edges(Visitor&) override;

    NonnullRefPtr<Response const> m_record;
    GC::Ptr<Body> m_body_view;
};

}
