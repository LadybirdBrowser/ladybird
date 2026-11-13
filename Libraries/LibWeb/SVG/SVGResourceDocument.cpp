/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibGC/Weak.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/ResponseView.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/SVG/SVGResourceDocument.h>
#include <LibWeb/XML/XMLDocumentBuilder.h>
#include <LibWebCommon/Fetch/Infrastructure/HTTP/Statuses.h>
#include <LibXML/Parser/Parser.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGResourceDocument);

GC::Ref<SVGResourceDocument> SVGResourceDocument::get_or_create(DOM::Document& document, URL::URL const& url)
{
    // A resource document's own use elements share the cache of the document that loaded it, so every URL loads once
    // per page, and documents that refer to each other find each other there rather than loading each other anew.
    auto& owner = document.svg_resource_document_owner() ? *document.svg_resource_document_owner() : document;
    auto& resource_documents = owner.svg_resource_documents();
    if (auto it = resource_documents.find(url); it != resource_documents.end())
        return *it->value;
    auto resource_document = document.heap().allocate<SVGResourceDocument>(owner, url);
    resource_documents.set(url, resource_document);
    return resource_document;
}

SVGResourceDocument::SVGResourceDocument(DOM::Document& document, URL::URL url)
    : m_document(document)
    , m_url(move(url))
{
}

void SVGResourceDocument::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
    visitor.visit(m_resource_document);
    visitor.visit(m_fetch_controller);
    for (auto const& callback : m_callbacks)
        visitor.visit(callback.on_done);
}

void SVGResourceDocument::fetch(NonnullRefPtr<Fetch::Infrastructure::Request> request)
{
    VERIFY(needs_fetching());
    m_state = State::Fetching;

    if (!ResourceLoader::is_initialized()) {
        finish(nullptr);
        return;
    }

    auto& realm = HTML::relevant_realm(*m_document);
    GC::Weak weak_this { *this };
    Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input { Fetch::Engine::BodyIntent::DeliverStreamToConsumerAgent };
    fetch_algorithms_input.process_response = GC::create_function<void(GC::Ref<Fetch::Infrastructure::ResponseView>)>(GC::Heap::the(), GC::captures(weak_this, realm), [](auto& weak_this, auto& realm, GC::Ref<Fetch::Infrastructure::ResponseView> response) {
        auto self = weak_this.ptr();
        if (!self)
            return;
        if (!response->body()) {
            self->finish(nullptr);
            return;
        }
        if (!Fetch::Infrastructure::is_ok_status(response->status())) {
            // NB: The body of an error response still has to be read, or its transport stays paused until the network
            //     gives up on it.
            auto ignore_chunk = GC::create_function(GC::Heap::the(), [](ByteBuffer) { });
            auto ignore_end_of_body = GC::create_function(GC::Heap::the(), [] { });
            auto ignore_body_error = GC::create_function(GC::Heap::the(), [](JS::Value) { });
            (void)response->body()->incrementally_read(realm, ignore_chunk, ignore_end_of_body, ignore_body_error, GC::Ref { realm.global_object() });
            self->finish(nullptr);
            return;
        }
        auto url = response->url().value_or(self->m_url);
        auto process_body = GC::create_function(GC::Heap::the(), GC::captures(weak_this, url), [](auto& weak_this, auto& url, ByteBuffer data) {
            auto self = weak_this.ptr();
            if (!self)
                return;

            // The document is parsed with scripting disabled, like an image's, and only lends elements to the shadow
            // trees of use elements, so it needs no browsing context of its own.
            auto& document = *self->m_document;
            auto resource_document = DOM::Document::create(document.page(), document.relevant_global_event_target(), url);
            resource_document->set_content_type("image/svg+xml"_utf16_fly_string);
            resource_document->set_document_type(DOM::Document::Type::XML);
            resource_document->set_origin(document.origin());
            resource_document->set_svg_resource_document_owner(document);

            XML::Parser parser(StringView { data.bytes() }, { .resolve_named_html_entity = resolve_named_html_entity });
            XMLDocumentBuilder builder { *resource_document, XMLScriptingSupport::Disabled };
            auto result = parser.parse_with_listener(builder);
            if (result.is_error() || builder.has_error()) {
                dbgln("SVGResourceDocument: Failed to parse {}", url);
                self->finish(nullptr);
                return;
            }
            self->finish(resource_document);
        });
        auto process_body_error = GC::create_function(GC::Heap::the(), GC::captures(weak_this), [](auto& weak_this, JS::Value) {
            if (auto self = weak_this.ptr())
                self->finish(nullptr);
        });
        response->body()->fully_read(realm, process_body, process_body_error, GC::Ref { realm.global_object() });
    });

    m_fetch_controller = Fetch::Fetching::fetch(realm, request, Fetch::Infrastructure::FetchAlgorithms::create(move(fetch_algorithms_input)));
    m_fetch_controller->set_stop_steps(GC::create_function(GC::Heap::the(), GC::captures(weak_this), [](auto& weak_this) {
        if (auto self = weak_this.ptr())
            self->stop();
    }));
}

Optional<u64> SVGResourceDocument::add_callback(GC::Ref<GC::Function<void()>> on_done)
{
    if (m_state == State::Done) {
        on_done->function()();
        return {};
    }
    auto id = m_next_callback_id++;
    m_callbacks.append({ id, on_done });
    return id;
}

void SVGResourceDocument::remove_callback(u64 id)
{
    // NB: A callback that is being called can get here, while finish() goes on over m_callbacks, so a finished
    //     resource only marks the entry as no longer waiting.
    if (m_state != State::Done) {
        m_callbacks.remove_first_matching([id](auto const& callback) { return callback.id == id; });
        return;
    }
    for (auto& callback : m_callbacks) {
        if (callback.id == id)
            callback.id = 0;
    }
}

void SVGResourceDocument::finish(GC::Ptr<DOM::Document> resource_document)
{
    if (m_state == State::Done)
        return;
    m_state = State::Done;
    m_resource_document = resource_document;
    m_fetch_controller = nullptr;
    for (auto const& callback : m_callbacks) {
        if (callback.id != 0)
            callback.on_done->function()();
    }
    m_callbacks.clear();
}

// NB: A stopped fetch never responds, so its waiters end without a document, and a later reference to the URL fetches
//     it again.
void SVGResourceDocument::stop()
{
    if (m_state != State::Fetching)
        return;
    auto& resource_documents = m_document->svg_resource_documents();
    if (auto it = resource_documents.find(m_url); it != resource_documents.end() && it->value.ptr() == this)
        resource_documents.remove(it);
    finish(nullptr);
}

}
