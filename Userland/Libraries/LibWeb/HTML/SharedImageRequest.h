/*
 * Copyright (c) 2023, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/OwnPtr.h>
#include <LibGfx/Size.h>
#include <LibJS/Heap/Handle.h>
#include <LibJS/Heap/HeapFunction.h>
#include <LibJS/SafeFunction.h>
#include <LibURL/URL.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

class SharedImageRequest final : public JS::Cell {
    JS_CELL(SharedImageRequest, JS::Cell);
    JS_DECLARE_ALLOCATOR(SharedImageRequest);

public:
    [[nodiscard]] static JS::NonnullGCPtr<SharedImageRequest> get_or_create(JS::Realm&, JS::NonnullGCPtr<Page>, URL::URL const&);

    virtual ~SharedImageRequest() override;

    URL::URL const& url() const { return m_url; }

    [[nodiscard]] JS::GCPtr<DecodedImageData> image_data() const;

    [[nodiscard]] JS::GCPtr<Fetch::Infrastructure::FetchController> fetch_controller();
    void set_fetch_controller(JS::GCPtr<Fetch::Infrastructure::FetchController>);

    void fetch_image(JS::Realm&, JS::NonnullGCPtr<Fetch::Infrastructure::Request>);

    void add_callbacks(Function<void()> on_finish, Function<void()> on_fail);

    bool is_fetching() const;
    bool needs_fetching() const;

private:
    explicit SharedImageRequest(JS::NonnullGCPtr<Page>, URL::URL, JS::NonnullGCPtr<DOM::Document>);

    virtual void finalize() override;
    virtual void visit_edges(JS::Cell::Visitor&) override;

    void handle_successful_fetch(URL::URL const&, StringView mime_type, ByteBuffer data);
    void handle_failed_fetch();
    void handle_successful_resource_load();

    enum class State {
        New,
        Fetching,
        Finished,
        Failed,
    };

    State m_state { State::New };

    JS::NonnullGCPtr<Page> m_page;

    struct Callbacks {
        JS::GCPtr<JS::HeapFunction<void()>> on_finish;
        JS::GCPtr<JS::HeapFunction<void()>> on_fail;
    };
    Vector<Callbacks> m_callbacks;

    URL::URL m_url;
    JS::GCPtr<DecodedImageData> m_image_data;
    JS::GCPtr<Fetch::Infrastructure::FetchController> m_fetch_controller;

    JS::GCPtr<DOM::Document> m_document;
};

}
