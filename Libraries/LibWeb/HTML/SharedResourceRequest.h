/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/OwnPtr.h>
#include <LibGC/Function.h>
#include <LibGC/Root.h>
#include <LibGfx/Size.h>
#include <LibURL/URL.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

class SharedResourceRequest final : public JS::Cell {
    GC_CELL(SharedResourceRequest, JS::Cell);
    GC_DECLARE_ALLOCATOR(SharedResourceRequest);

public:
    [[nodiscard]] static GC::Ref<SharedResourceRequest> get_or_create(JS::Realm&, GC::Ref<Page>, URL::URL const&);

    virtual ~SharedResourceRequest() override;

    URL::URL const& url() const { return m_url; }

    [[nodiscard]] GC::Ptr<DecodedImageData> image_data() const;

    [[nodiscard]] GC::Ptr<Fetch::Infrastructure::FetchController> fetch_controller();
    void set_fetch_controller(GC::Ptr<Fetch::Infrastructure::FetchController>);

    void fetch_resource(JS::Realm&, GC::Ref<Fetch::Infrastructure::Request>);

    void add_callbacks(Function<void()> on_finish, Function<void()> on_fail);

    bool is_fetching() const;
    bool needs_fetching() const;

private:
    explicit SharedResourceRequest(GC::Ref<Page>, URL::URL, GC::Ref<DOM::Document>);

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

    GC::Ref<Page> m_page;

    struct Callbacks {
        GC::Ptr<GC::Function<void()>> on_finish;
        GC::Ptr<GC::Function<void()>> on_fail;
    };
    Vector<Callbacks> m_callbacks;

    URL::URL m_url;
    GC::Ptr<DecodedImageData> m_image_data;
    GC::Ptr<Fetch::Infrastructure::FetchController> m_fetch_controller;

    GC::Ptr<DOM::Document> m_document;
};

}
