/*
 * Copyright (c) 2022-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AllOf.h>
#include <AK/AnyOf.h>
#include <AK/NeverDestroyed.h>
#include <AK/TemporaryChange.h>
#include <AK/Utf16String.h>
#include <AK/Utf16StringBuilder.h>
#include <AK/Variant.h>
#include <LibCore/Timer.h>
#include <LibGfx/PaintingSurface.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/PseudoElement.h>
#include <LibWeb/CSS/SerializationMode.h>
#include <LibWeb/CSS/SystemColor.h>
#include <LibWeb/CSS/VisualViewport.h>
#include <LibWeb/Compositor/CompositorHost.h>
#include <LibWeb/ContentSecurityPolicy/BlockingAlgorithms.h>
#include <LibWeb/ContentSecurityPolicy/Directives/DirectiveOperations.h>
#include <LibWeb/ContentSecurityPolicy/PolicyList.h>
#include <LibWeb/ContentSecurityPolicy/Violation.h>
#include <LibWeb/Crypto/Crypto.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentFragment.h>
#include <LibWeb/DOM/DocumentLoading.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/Position.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Editing/ClipboardSerializer.h>
#include <LibWeb/Editing/EditingHistory.h>
#include <LibWeb/Editing/Internal/Algorithms.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/URL.h>
#include <LibWeb/FileAPI/File.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/BrowsingContextGroup.h>
#include <LibWeb/HTML/DocumentState.h>
#include <LibWeb/HTML/DragDataStore.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/HTML/HTMLIFrameElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLParagraphElement.h>
#include <LibWeb/HTML/History.h>
#include <LibWeb/HTML/HistoryHandlingBehavior.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/HTML/Navigation.h>
#include <LibWeb/HTML/NavigationObserver.h>
#include <LibWeb/HTML/NavigationParams.h>
#include <LibWeb/HTML/NavigationPopulationRequest.h>
#include <LibWeb/HTML/POSTResource.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/HTML/SandboxingFlagSet.h>
#include <LibWeb/HTML/Scripting/ClassicScript.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/HTML/SourceSnapshotParams.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/HTML/XMLSerializer.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Loader/DownloadFilename.h>
#include <LibWeb/Loader/GeneratedPagesLoader.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/DisplayListDamage.h>
#include <LibWeb/Painting/DocumentPaintState.h>
#include <LibWeb/Painting/PaintableTypes.h>
#include <LibWeb/Painting/ScrollSnap.h>
#include <LibWeb/Selection/Selection.h>
#include <LibWeb/UIEvents/InputTypes.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWeb/XHR/FormData.h>

#include <AK/Debug.h>
#include <AK/LexicalPath.h>
#include <AK/StdLibExtras.h>
#include <AK/StringBuilder.h>
#include <LibHTTP/HTTP.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(LocalNavigable);

struct NavigationParamsFetchStateHolder : public JS::Cell {
    GC_CELL(NavigationParamsFetchStateHolder, JS::Cell);
    GC_DECLARE_ALLOCATOR(NavigationParamsFetchStateHolder);

    NavigationParamsFetchStateHolder(OpenerPolicyEnforcementResult&& coop_enforcement_result, URL::URL current_url, GC::Ref<Fetch::Infrastructure::Request> request,
        Optional<URL::Origin> initiator_origin,
        Variant<SerializedPolicyContainer, DocumentState::Client> history_policy_container,
        Optional<URL::URL> about_base_url,
        GC::Ref<SourceSnapshotParams> source_snapshot_params,
        Fetch::Infrastructure::Request::ReferrerType request_referrer,
        ReferrerPolicy::ReferrerPolicy request_referrer_policy,
        Optional<URL::Origin> origin,
        DocumentResource resource,
        bool ever_populated,
        Utf16String navigable_target_name)
        : coop_enforcement_result(move(coop_enforcement_result))
        , current_url(move(current_url))
        , request(request)
        , initiator_origin(move(initiator_origin))
        , history_policy_container(move(history_policy_container))
        , about_base_url(move(about_base_url))
        , source_snapshot_params(source_snapshot_params)
        , request_referrer(move(request_referrer))
        , request_referrer_policy(request_referrer_policy)
        , origin(move(origin))
        , resource(move(resource))
        , ever_populated(ever_populated)
        , navigable_target_name(move(navigable_target_name))
    {
    }

    GC::Ptr<Fetch::Infrastructure::Response> response;
    Optional<URL::Origin> response_origin;
    GC::Ptr<Fetch::Infrastructure::FetchController> fetch_controller;
    OpenerPolicyEnforcementResult coop_enforcement_result;
    SandboxingFlagSet final_sandbox_flags {};
    GC::Ptr<PolicyContainer> response_policy_container;
    OpenerPolicy response_coop {};
    ErrorOr<Optional<URL::URL>> location_url { OptionalNone {} };
    URL::URL current_url;
    GC::Ptr<GC::Function<void(DOM::Document&)>> commit_early_hints;

    GC::Ref<Fetch::Infrastructure::Request> request;
    GC::Ptr<LocalNavigable> navigable;
    ContentSecurityPolicy::Directives::Directive::NavigationType csp_navigation_type;
    TargetSnapshotParams target_snapshot_params;
    Optional<Utf16String> navigation_id;

    // Fields extracted from entry's document_state
    Optional<URL::Origin> initiator_origin;
    Variant<SerializedPolicyContainer, DocumentState::Client> history_policy_container;
    Optional<URL::URL> about_base_url;
    GC::Ref<SourceSnapshotParams> source_snapshot_params;

    // Document state fields for redirect DocumentState rebuild
    Fetch::Infrastructure::Request::ReferrerType request_referrer { Fetch::Infrastructure::Request::Referrer::Client };
    ReferrerPolicy::ReferrerPolicy request_referrer_policy { ReferrerPolicy::DEFAULT_REFERRER_POLICY };
    Optional<URL::Origin> origin;
    DocumentResource resource;
    bool ever_populated = false;
    Utf16String navigable_target_name;

    // Accumulated redirect output
    Optional<URL::URL> redirected_url;
    Optional<StorageSerializationRecord> redirect_classic_history_api_state;
    RefPtr<DocumentState> replacement_document_state;
    bool resource_cleared = false;

    enum class ContinuationReason {
        GotResponse,
        OngoingNavigationChanged,
    };

    GC::Ptr<GC::Function<void(ContinuationReason)>> continuation_steps;

    virtual void visit_edges(Cell::Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(response);
        visitor.visit(fetch_controller);
        visitor.visit(response_policy_container);
        visitor.visit(commit_early_hints);
        visitor.visit(request);
        visitor.visit(navigable);
        visitor.visit(source_snapshot_params);
        visitor.visit(continuation_steps);
    }
};

GC_DEFINE_ALLOCATOR(NavigationParamsFetchStateHolder);

// Carries the redirect metadata alongside navigation params from the fetch path back to
// populate_session_history_entry_document's received_navigation_params callback.
struct InternalNavigationResult final : public JS::Cell {
    GC_CELL(InternalNavigationResult, JS::Cell);
    GC_DECLARE_ALLOCATOR(InternalNavigationResult);

public:
    LocalNavigable::NavigationParamsVariant navigation_params { LocalNavigable::NullOrError {} };

    // Redirect mutations (only set by fetch path)
    Optional<URL::URL> redirected_url;
    Optional<StorageSerializationRecord> classic_history_api_state;
    RefPtr<DocumentState> replacement_document_state;
    bool resource_cleared = false;

private:
    virtual void visit_edges(Cell::Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(navigation_params);
    }
};

GC_DEFINE_ALLOCATOR(InternalNavigationResult);

GC_DEFINE_ALLOCATOR(PopulateSessionHistoryEntryDocumentOutput);

static Vector<StringView> split_content_disposition_parameters(StringView value)
{
    Vector<StringView> parameters;
    bool in_quoted_string = false;
    bool escaped = false;
    size_t parameter_start = 0;

    for (size_t i = 0; i < value.length(); ++i) {
        auto ch = value[i];
        if (escaped) {
            escaped = false;
            continue;
        }

        if (in_quoted_string && ch == '\\') {
            escaped = true;
            continue;
        }

        if (ch == '"') {
            in_quoted_string = !in_quoted_string;
            continue;
        }

        if (ch != ';' || in_quoted_string)
            continue;

        parameters.append(value.substring_view(parameter_start, i - parameter_start).trim(HTTP::HTTP_TAB_OR_SPACE, TrimMode::Both));
        parameter_start = i + 1;
    }

    parameters.append(value.substring_view(parameter_start).trim(HTTP::HTTP_TAB_OR_SPACE, TrimMode::Both));
    return parameters;
}

static ByteString parse_content_disposition_parameter_value(StringView value)
{
    value = value.trim(HTTP::HTTP_TAB_OR_SPACE, TrimMode::Both);
    if (!value.starts_with('"'))
        return value;

    StringBuilder builder;
    bool escaped = false;
    for (size_t i = 1; i < value.length(); ++i) {
        auto ch = value[i];
        if (escaped) {
            builder.append(ch);
            escaped = false;
            continue;
        }

        if (ch == '\\') {
            escaped = true;
            continue;
        }

        if (ch == '"')
            break;

        builder.append(ch);
    }
    return builder.to_byte_string();
}

static Optional<ByteString> parse_content_disposition_extended_filename(StringView value)
{
    auto decoded_value = parse_content_disposition_parameter_value(value);
    auto decoded_view = decoded_value.view();
    auto first_separator = decoded_view.find('\'');
    if (!first_separator.has_value())
        return URL::percent_decode(decoded_view);

    auto second_separator = decoded_view.find('\'', *first_separator + 1);
    if (!second_separator.has_value())
        return {};

    return URL::percent_decode(decoded_view.substring_view(*second_separator + 1));
}

struct ContentDispositionInfo {
    bool is_attachment { false };
    Optional<ByteString> filename;
};

static ContentDispositionInfo parse_content_disposition(HTTP::HeaderList const& headers)
{
    auto header = headers.get("Content-Disposition"sv);
    if (!header.has_value())
        return {};

    auto parameters = split_content_disposition_parameters(header->view());
    if (parameters.is_empty())
        return {};

    ContentDispositionInfo info;
    info.is_attachment = parameters.first().equals_ignoring_ascii_case("attachment"sv);

    Optional<ByteString> filename;
    Optional<ByteString> extended_filename;
    for (size_t i = 1; i < parameters.size(); ++i) {
        auto parameter = parameters[i];
        auto equals_index = parameter.find('=');
        if (!equals_index.has_value())
            continue;

        auto name = parameter.substring_view(0, *equals_index).trim(HTTP::HTTP_TAB_OR_SPACE, TrimMode::Both);
        auto value = parameter.substring_view(*equals_index + 1);
        if (name.equals_ignoring_ascii_case("filename*"sv))
            extended_filename = parse_content_disposition_extended_filename(value);
        else if (name.equals_ignoring_ascii_case("filename"sv))
            filename = parse_content_disposition_parameter_value(value);
    }

    info.filename = extended_filename.has_value() ? move(extended_filename) : move(filename);
    return info;
}

// https://html.spec.whatwg.org/multipage/links.html#getting-the-suggested-filename
static ByteString suggested_download_filename(URL::URL const& url, HTTP::HeaderList const& headers, Optional<ByteString> const& proposed_filename, Optional<URL::Origin> const& interface_origin)
{
    // 1. Let filename be the undefined value.
    Optional<ByteString> filename;

    // 2. If response has a `Content-Disposition` header, that header specifies the attachment disposition type,
    //    and the header includes filename information, then let filename have the value specified by the header,
    //    and jump to the step labeled sanitize below.
    auto content_disposition = parse_content_disposition(headers);
    if (content_disposition.is_attachment && content_disposition.filename.has_value()) {
        filename = content_disposition.filename.value();
        goto sanitize;
    }

    // NB: Steps 3 to 10 are enclosed in a scope, so the jumps below may bypass the variables declared here.
    {
        // 3. Let interface origin be the origin of the Document in which the download or navigate action resulting
        //    in the download was initiated, if any.
        // NB: The interface origin is provided by the caller.

        // 4. Let response origin be the origin of the URL of response, unless that URL's scheme component is data,
        //    in which case let response origin be the same as the interface origin, if any.
        Optional<URL::Origin> response_origin = url.scheme() == "data"sv ? interface_origin : url.origin();

        // 5. If there is no interface origin, then let trusted operation be true. Otherwise, let trusted operation
        //    be true if response origin is the same origin as interface origin, and false otherwise.
        auto trusted_operation = !interface_origin.has_value() || response_origin->is_same_origin(*interface_origin);

        // 6. If trusted operation is true and response has a `Content-Disposition` header and that header includes
        //    filename information, then let filename have the value specified by the header, and jump to the step
        //    labeled sanitize below.
        if (trusted_operation && content_disposition.filename.has_value()) {
            filename = content_disposition.filename.value();
            goto sanitize;
        }

        // 7. If the download was not initiated from a hyperlink created by an a or area element, or if the element
        //    of the hyperlink from which it was initiated did not have a download attribute when the download was
        //    initiated, or if there was such an attribute but its value when the download was initiated was the
        //    empty string, then jump to the step labeled no proposed filename.
        if (!proposed_filename.has_value() || proposed_filename->is_empty())
            goto no_proposed_filename;

        // 8. Let proposed filename have the value of the download attribute of the element of the hyperlink that
        //    initiated the download at the time the download was initiated.
        // NB: The proposed filename is provided by the caller.

        // 9. If trusted operation is true, let filename have the value of proposed filename, and jump to the step
        //    labeled sanitize below.
        if (trusted_operation) {
            filename = proposed_filename.value();
            goto sanitize;
        }

        // 10. If response has a `Content-Disposition` header and that header specifies the attachment disposition
        //     type, let filename have the value of proposed filename, and jump to the step labeled sanitize below.
        if (content_disposition.is_attachment) {
            filename = proposed_filename.value();
            goto sanitize;
        }
    }

    // 11. No proposed filename: If trusted operation is true, or if the user indicated a preference for having the
    //     response in question downloaded, let filename have a value derived from the URL of response in an
    //     implementation-defined manner, and jump to the step labeled sanitize below.
no_proposed_filename:
    // 12. Let filename be set to the user's preferred filename or to a filename selected by the user agent, and
    //     jump to the step labeled sanitize below.
    // NB: In both cases the filename is derived from the URL of the response. URLs with an opaque path, such as
    //     data: URLs, do not contain a usable filename, so the default filename is used instead.
    if (!url.has_an_opaque_path())
        filename = url.basename();

    // 13. Sanitize: Optionally, allow the user to influence filename. For example, a user agent could prompt the
    //     user for a filename, potentially providing the value of filename as determined above as a default value.
sanitize:

    // 14. Adjust filename to be suitable for the local file system.
    filename = sanitize_suggested_download_filename(filename.value_or({}));

    // FIXME: 15. If the platform conventions do not in any way use extensions to determine the types of file on
    //        the file system, then return filename as the filename.

    // FIXME: 16. Let claimed type be the type given by response's Content-Type metadata, if any is known. Let
    //        named type be the type given by filename's extension, if any is known. For the purposes of this step, a
    //        type is a mapping of a MIME type to an extension.

    // FIXME: 17. If named type is consistent with the user's preferences (e.g., because the value of filename was
    //        determined by prompting the user), then return filename as the filename.

    // FIXME: 18. If claimed type and named type are the same type (i.e., the type given by response's Content-Type
    //        metadata is consistent with the type given by filename's extension), then return filename as the filename.

    // FIXME: 19. If the claimed type is known, then alter filename to add an extension corresponding to claimed
    //        type. Otherwise, if named type is known to be potentially dangerous (e.g. it will be treated by the
    //        platform conventions as a native executable, shell script, HTML application, or
    //        executable-macro-capable document), then optionally alter filename to add a known-safe extension
    //        (e.g. ".txt").

    // 20. Return filename as the filename.
    return filename.release_value();
}

static Optional<u64> response_content_length(HTTP::HeaderList const& headers)
{
    auto extracted_length = headers.extract_length();
    if (extracted_length.has<u64>())
        return extracted_length.get<u64>();
    return {};
}

void LocalNavigable::start_download_for_response(GC::Ref<Fetch::Infrastructure::Response> response, URL::URL const& download_url, ByteString suggested_filename, GC::Ptr<Fetch::Infrastructure::FetchController> fetch_controller)
{
    auto active_window = this->active_window();
    if (!active_window) {
        response->release_request_transfer_lease();
        return;
    }
    auto& realm = active_window->principal_realm();

    auto download_id = page().client().page_did_start_download(download_url, suggested_filename, response_content_length(*response->header_list()));
    if (!download_id.has_value()) {
        if (fetch_controller)
            fetch_controller->stop_fetch();
        return;
    }

    if (fetch_controller)
        page().client().page_did_register_download_controller(*download_id, *fetch_controller);

    auto process_body_chunk = GC::create_function(realm.heap(), [navigable = GC::Ref { *this }, download_id = *download_id](ByteBuffer data) {
        if (navigable->page().client().page_is_download_canceled(download_id))
            return;

        navigable->page().client().page_did_receive_download_data(download_id, move(data));
    });

    auto process_end_of_body = GC::create_function(realm.heap(), [navigable = GC::Ref { *this }, download_id = *download_id]() {
        if (navigable->page().client().page_is_download_canceled(download_id))
            return;

        navigable->page().client().page_did_finish_download(download_id);
    });

    auto process_body_error = GC::create_function(realm.heap(), [navigable = GC::Ref { *this }, download_id = *download_id](JS::Value) {
        if (navigable->page().client().page_is_download_canceled(download_id))
            return;

        navigable->page().client().page_did_fail_download(download_id, "Unable to read downloaded file"_string);
    });

    // https://fetch.spec.whatwg.org/#body-incrementally-read
    auto reader = response->body()->incrementally_read(process_body_chunk, process_end_of_body, process_body_error, GC::Ref { realm.global_object() });
    page().client().page_did_register_download_reader(*download_id, reader);
    response->resume_body_delivery();
}

// https://html.spec.whatwg.org/multipage/links.html#handle-as-a-download
void LocalNavigable::handle_as_a_download(GC::Ref<Fetch::Infrastructure::Response> response, URL::URL const& fallback_url, GC::Ptr<Fetch::Infrastructure::FetchController> fetch_controller, Optional<ByteString> proposed_filename, Optional<URL::Origin> interface_origin)
{
    if (!response->body())
        return;

    auto download_url = response->url().value_or(fallback_url);
    auto suggested_filename = suggested_download_filename(download_url, *response->header_list(), proposed_filename, interface_origin);

    start_download_for_response(response, download_url, move(suggested_filename), fetch_controller);
}

static bool handle_navigation_response_as_download(GC::Ref<NavigationParams> navigation_params, bool source_allows_downloading, Optional<URL::Origin> interface_origin, Optional<ReadonlyBytes> initial_data = {})
{
    auto response = navigation_params->response;
    if (!response || !response->body())
        return true;

    // FIXME: Implement the WebDriver BiDi download will begin/end hooks.
    //        uaAllowsDownloading is currently always true.
    auto target_allows_downloading = !has_flag(navigation_params->final_sandboxing_flag_set, SandboxingFlagSet::SandboxedDownloads);
    if (!source_allows_downloading || !target_allows_downloading) {
        if (navigation_params->fetch_controller)
            navigation_params->fetch_controller->stop_fetch();
        else
            response->resume_body_delivery();
        return true;
    }

    VERIFY(navigation_params->navigable);
    auto active_window = navigation_params->navigable->active_window();
    if (!active_window) {
        response->release_request_transfer_lease();
        return true;
    }

    auto download_url = response->url().value_or(navigation_params->request ? navigation_params->request->current_url() : URL::about_blank());
    if (auto const& request_server_request = response->request_server_request(); request_server_request.has_value()) {
        auto suggested_filename = suggested_download_filename(download_url, *response->header_list(), {}, interface_origin);
        auto response_body_will_be_transferred_in_full = request_server_request->request && request_server_request->request->has_file_backed_response_body();
        ByteBuffer initial_data_buffer;
        if (initial_data.has_value() && !initial_data->is_empty() && !response_body_will_be_transferred_in_full)
            initial_data_buffer = MUST(ByteBuffer::copy(*initial_data));

        auto download_id = navigation_params->navigable->page().client().page_did_start_download(navigation_params->navigable->id(), navigation_params->id, download_url, suggested_filename, response_content_length(*response->header_list()), request_server_request->client_id, request_server_request->request_id, move(initial_data_buffer));
        if (!download_id.has_value()) {
            if (navigation_params->fetch_controller)
                navigation_params->fetch_controller->stop_fetch();
            else
                response->release_request_transfer_lease();
            return true;
        }

        if (navigation_params->fetch_controller)
            navigation_params->fetch_controller->terminate();

        return true;
    }

    navigation_params->navigable->handle_as_a_download(*response, download_url, navigation_params->fetch_controller, {}, interface_origin);
    return true;
}

static void stop_or_resume_response_body_delivery(LocalNavigable::NavigationParamsVariant const& navigation_params)
{
    // AD-HOC: Fetch controller stop_fetch() is an implementation hook for
    //         tearing down a paused network body when no spec consumer remains.
    if (!navigation_params.has<GC::Ref<NavigationParams>>())
        return;

    auto const& nav_params = navigation_params.get<GC::Ref<NavigationParams>>();
    if (nav_params->fetch_controller)
        nav_params->fetch_controller->stop_fetch();
    else
        nav_params->response->resume_body_delivery();
}

void PopulateSessionHistoryEntryDocumentOutput::apply_to(NonnullRefPtr<SessionHistoryEntry> entry)
{
    if (replacement_document_state)
        entry->set_document_state(replacement_document_state);
    if (redirected_url.has_value())
        entry->set_url(redirected_url.value());
    if (classic_history_api_state.has_value())
        entry->set_classic_history_api_state(classic_history_api_state.release_value());
    if (resource_cleared)
        entry->document_state()->set_resource(Empty {});

    // Step from populate_session_history_entry_document()
    // 7. If entry's document state's document is not null, then:
    if (document) {
        entry->document_state()->set_document_id(document->unique_id());

        // 1. Set entry's document state's ever populated to true.
        entry->document_state()->set_ever_populated(true);

        // 2. If saveExtraDocumentState is true:
        if (save_extra_document_state) {
            // 1. Set entry's document state's origin to document's origin.
            entry->document_state()->set_origin(document->origin());

            // 2. If document's URL requires storing the policy container in history, then:
            if (url_requires_storing_the_policy_container_in_history(document->url())) {
                // 1. Assert: navigationParams is a navigation params (i.e., neither null nor a non-fetch scheme navigation params).
                VERIFY(navigation_params.has<GC::Ref<NavigationParams>>());

                // 2. Set entry's document state's history policy container to navigationParams's policy container.
                entry->document_state()->set_history_policy_container(
                    navigation_params.get<GC::Ref<NavigationParams>>()->policy_container->serialize());
            }
        }

        // 3. If entry's document state's request referrer is "client", and navigationParams is a navigation params (i.e., neither null nor a non-fetch scheme navigation params), then:
        if (entry->document_state()->request_referrer() == Fetch::Infrastructure::Request::Referrer::Client
            && navigation_params.has<GC::Ref<NavigationParams>>()
            && navigation_params.get<GC::Ref<NavigationParams>>()->request) {
            // NB: We don't assert navigationParams's request is not null because srcdoc navigations create NavigationParams with a null request.

            // 1. Set entry's document state's request referrer to navigationParams's request's referrer.
            entry->document_state()->set_request_referrer(
                navigation_params.get<GC::Ref<NavigationParams>>()->request->referrer());
        }
    }
}

void PopulateSessionHistoryEntryDocumentOutput::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(document);
    visitor.visit(navigation_params);
}

HashTable<GC::RawRef<LocalNavigable>>& all_local_navigables()
{
    static NeverDestroyed<HashTable<GC::RawRef<LocalNavigable>>> set;
    return *set;
}

GC::Ptr<LocalNavigable> local_navigable_with_id(CrossProcessId id)
{
    for (auto& navigable : all_local_navigables()) {
        if (navigable->id() == id)
            return navigable;
    }
    return nullptr;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#child-navigable
Vector<GC::Root<LocalNavigable>> LocalNavigable::child_navigables() const
{
    Vector<GC::Root<LocalNavigable>> results;
    for (auto& entry : all_local_navigables()) {
        if (entry->current_session_history_entry()->step() == SessionHistoryEntry::Pending::Tag)
            continue;
        if (entry->parent().ptr() == this)
            results.append(entry);
    }

    return results;
}

LocalNavigable::LocalNavigable(
    GC::Ref<Page> page,
    bool is_svg_page,
    Compositor::PagePresentationRegistration page_presentation_registration)
    : m_page(page)
    , m_event_handler({}, *this)
    , m_is_svg_page(is_svg_page)
{
    all_local_navigables().set(*this);

    if (!m_is_svg_page && page->has_compositor_host()) {
        auto context_id = page->client().allocate_compositor_context_id(page_presentation_registration);
        m_compositor_context = page->compositor_host().create_context(context_id);
    }
}

LocalNavigable::~LocalNavigable() = default;

void LocalNavigable::set_has_been_destroyed()
{
    if (!m_has_been_destroyed && parent())
        page().client().page_did_destroy_child_frame(id());

    cancel_hover_update_after_async_scroll();
    destroy_compositor_context();
    m_has_been_destroyed = true;
    resolve_all_pending_async_scroll_operations();
    cancel_user_scroll_settlement();
}

void LocalNavigable::remove_from_all_local_navigables()
{
    cancel_hover_update_after_async_scroll();
    destroy_compositor_context();
    resolve_all_pending_async_scroll_operations();
    cancel_user_scroll_settlement();

    if (m_active_document)
        m_active_document->set_navigable(nullptr);
    all_local_navigables().remove(*this);
}

void LocalNavigable::finalize()
{
    cancel_hover_update_after_async_scroll();
    cancel_user_scroll_settlement();
    destroy_compositor_context();
    all_local_navigables().remove(*this);
    Base::finalize();
}

void LocalNavigable::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_page);
    visitor.visit(m_active_document);
    visitor.visit(m_input_method_composition_node);
    visitor.visit(m_container);
    m_event_handler.visit_edges(visitor);

    for (auto& pending_navigation : m_pending_navigations) {
        if (pending_navigation.navigation.has_value())
            pending_navigation.navigation->visit_edges(visitor);
        visitor.visit(pending_navigation.continue_steps);
    }

    for (auto& async_scroll_operation : m_pending_async_scroll_operations)
        visitor.visit(async_scroll_operation.promises);
    for (auto& smooth_scroll : m_main_thread_smooth_scrolls)
        visitor.visit(smooth_scroll.promises);
    for (auto& entry : m_pending_user_scrollend_targets)
        visitor.visit(entry.target);
}

void LocalNavigable::NavigateParams::visit_edges(Cell::Visitor& visitor)
{
    visitor.visit(response);
    visitor.visit(source_document);
    visitor.visit(source_element);
    if (form_data_entry_list.has_value()) {
        for (auto& entry : form_data_entry_list.value()) {
            entry.value.visit([&](GC::Ref<FileAPI::File> const& file) { visitor.visit(file); },
                [&](auto const&) {});
        }
    }
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#script-closable
bool LocalNavigable::is_script_closable()
{
    // A navigable is script-closable if it is a top-level traversable, and any of the following are true:
    // - its is created by web content is true; or
    // - its session history entries's size is 1.
    if (!is_top_level_traversable())
        return false;

    return as<LocalTraversableNavigable>(this)->is_created_by_web_content()
        || as<LocalTraversableNavigable>(this)->session_history_entry_count() == 1;
}

void LocalNavigable::set_delaying_load_events(bool value)
{
    if (value) {
        auto document = container_document();
        VERIFY(document);
        m_delaying_the_load_event.emplace(*document);
    } else {
        m_delaying_the_load_event.clear();
    }
}

void LocalNavigable::set_navigation_load_event_guard(DOM::Document& parent_doc)
{
    m_navigation_load_event_guard.emplace(parent_doc);
}

void LocalNavigable::clear_navigation_load_event_guard()
{
    m_navigation_load_event_guard.clear();
}

RefPtr<SessionHistoryEntry> LocalNavigable::active_session_history_entry() const
{
    return m_active_session_history_entry;
}

void LocalNavigable::set_active_session_history_entry(RefPtr<SessionHistoryEntry> entry)
{
    m_active_session_history_entry = move(entry);
}

RefPtr<SessionHistoryEntry> LocalNavigable::current_session_history_entry() const
{
    return m_current_session_history_entry;
}

void LocalNavigable::set_current_session_history_entry(RefPtr<SessionHistoryEntry> entry)
{
    m_current_session_history_entry = move(entry);
}

Optional<CrossProcessId> LocalNavigable::child_navigable_history_reconstruction_id(size_t index) const
{
    if (index >= m_child_navigable_history_reconstruction_ids.size())
        return {};
    return m_child_navigable_history_reconstruction_ids[index];
}

void LocalNavigable::consume_child_navigable_history_reconstruction_id(size_t index)
{
    VERIFY(index < m_child_navigable_history_reconstruction_ids.size());
    m_child_navigable_history_reconstruction_ids[index].clear();
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#initialize-the-navigable
void LocalNavigable::initialize_navigable(NonnullRefPtr<DocumentState> document_state, GC::Ptr<LocalNavigable> parent, GC::Ref<DOM::Document> document)
{
    set_id(page().client().allocate_navigable_id());

    // 1. Assert: documentState's document is non-null.
    // NOTE: DocumentState no longer owns the document; it is passed separately and owned by the LocalNavigable.

    // 2. Let entry be a new session history entry, with
    auto entry = SessionHistoryEntry::create();
    // URL: document's URL
    entry->set_url(document->url());
    // document state: documentState
    entry->set_document_state(document_state);
    document_state->set_document_id(document->unique_id());

    // 3. Set navigable's current session history entry to entry.
    m_current_session_history_entry = entry;

    // 4. Set navigable's active session history entry to entry.
    m_active_session_history_entry = entry;
    m_active_document = document;
    document->set_navigable(this);

    // 5. Set navigable's parent to parent.
    set_parent(parent);
    if (parent) {
        m_should_show_line_box_borders = parent->m_should_show_line_box_borders;
        m_should_show_caret_hit_test_debug_overlay = parent->m_should_show_caret_hit_test_debug_overlay;
    }
    if (parent && !m_is_svg_page && has_compositor_context() && parent->has_compositor_context()) {
        compositor_context().set_parent_context(parent->compositor_context().id());
    }

    // 6. Set the initial visibility state of documentState's document to navigable's traversable navigable's system visibility state.
    document->set_initial_visibility_state(traversable_navigable()->system_visibility_state());
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#activate-history-entry
void LocalNavigable::activate_history_entry(RefPtr<SessionHistoryEntry> entry, GC::Ref<DOM::Document> document)
{
    // 1. Save persisted state to the navigable's active session history entry.
    save_persisted_state_to_active_session_history_entry();

    // 2. Let newDocument be entry's document.
    auto new_document = document;

    // 3. Assert: newDocument's is initial about:blank is false, i.e., we never traverse
    //    back to the initial about:blank Document because it always gets replaced when we
    //    navigate away from it.
    VERIFY(!new_document->is_initial_about_blank());

    // DocumentState identifies its associated Document by a process-local ID. Restore that association at the
    // transition that makes a reconstructed entry's Document active.
    VERIFY(entry);
    auto document_state = entry->document_state();
    VERIFY(document_state);
    document_state->set_document_id(new_document->unique_id());

    // 4. Set navigable's active session history entry to entry.
    m_active_session_history_entry = entry;
    if (m_active_document && m_active_document != new_document) {
        // The pending post-scroll hover refresh and scrollend settlement belong to the outgoing document; drop them.
        cancel_hover_update_after_async_scroll();
        cancel_user_scroll_settlement();
        m_active_document->set_navigable(nullptr);
    }
    m_active_document = new_document;
    new_document->set_navigable(this);
    set_needs_to_record_display_list();

    // 5. Make active newDocument.
    new_document->make_active();

    // 6. Set the initial visibility state of newDocument to navigable's traversable navigable's system visibility state.
    new_document->set_initial_visibility_state(traversable_navigable()->system_visibility_state());

    // AD-HOC: In the async state machine, documents created during populate may have completed
    //         their loading lifecycle before being activated (when they had no navigable).
    //         Re-trigger the post-load steps that were skipped:
    //         - completely_finish_loading: fires the iframe load event on the container
    //         - clear_navigation_load_event_guard: clears the parent's load event delayer
    //           that was set in finalize_a_cross_document_navigation
    //         - schedule_html_parser_end_check: allows the parent's parser end state to progress
    if (new_document->ready_for_post_load_tasks()) {
        clear_navigation_load_event_guard();
        if (auto nav_container = container())
            nav_container->document().schedule_html_parser_end_check();
    }
    if (new_document->completely_loaded_deferred())
        new_document->completely_finish_loading();

    notify_navigation_observers_navigation_complete();
}

void LocalNavigable::notify_navigation_observers_navigation_complete()
{
    if (!m_ongoing_navigation.has<Empty>())
        return;

    for (auto& navigation_observer : m_navigation_observers) {
        if (navigation_observer.navigation_complete())
            navigation_observer.navigation_complete()->function()();
    }
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#save-persisted-state
void LocalNavigable::save_persisted_state_to_active_session_history_entry()
{
    auto entry = active_session_history_entry();
    if (!entry)
        return;

    // 1. Set the scroll position data of entry to contain the scroll positions for all of entry's document's
    //    restorable scrollable regions.
    auto scroll_position_data = entry->scroll_position_data();
    scroll_position_data.viewport_scroll_position = viewport_scroll_offset();
    entry->set_scroll_position_data(move(scroll_position_data));

    // FIXME: 2. Optionally, update entry's persisted user state.
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#restore-persisted-user-state
void LocalNavigable::restore_persisted_state_from_session_history_entry(SessionHistoryEntry const& entry)
{
    m_pending_persisted_state_restoration.clear();

    // 1. If entry's scroll restoration mode is "auto", and entry's document's relevant global object's navigation
    //    API's suppress normal scroll restoration during ongoing navigation is false, then restore scroll position
    //    data given entry.
    if (entry.scroll_restoration_mode() == ScrollRestorationMode::Auto) {
        if (auto window = active_window()) {
            if (!window->navigation()->suppress_normal_scroll_restoration_during_ongoing_navigation()) {
                restore_scroll_position_data(entry);
            }
        }
    }

    // FIXME: 2. Optionally, update other aspects of entry's document and its rendering, for instance values of form
    //        fields, that the user agent had previously recorded in entry's persisted user state.
}

void LocalNavigable::schedule_persisted_state_restoration_retry(SessionHistoryEntry const& entry)
{
    auto document = active_document();
    auto document_state = entry.document_state();
    if (!document || !document_state || document->readiness() == DocumentReadyState::Complete)
        return;
    if (entry.scroll_restoration_mode() != ScrollRestorationMode::Auto
        || !entry.scroll_position_data().viewport_scroll_position.has_value()) {
        return;
    }
    m_pending_persisted_state_restoration = PendingPersistedStateRestoration {
        .document = document,
        .document_state_id = document_state->cross_process_id(),
        .navigation_api_key = entry.navigation_api_key(),
    };
}

void LocalNavigable::restore_pending_persisted_state_for_completed_document(GC::Ref<DOM::Document> document)
{
    if (!m_pending_persisted_state_restoration.has_value()
        || m_pending_persisted_state_restoration->document != document) {
        return;
    }

    auto restoration = m_pending_persisted_state_restoration.release_value();
    auto entry = active_session_history_entry();
    if (!entry || entry->navigation_api_key() != restoration.navigation_api_key)
        return;
    auto document_state = entry->document_state();
    if (!document_state || document_state->cross_process_id() != restoration.document_state_id)
        return;
    restore_persisted_state_from_session_history_entry(*entry);
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#restore-scroll-position-data
void LocalNavigable::restore_scroll_position_data(SessionHistoryEntry const& entry)
{
    auto const& scroll_position_data = entry.scroll_position_data();
    if (!scroll_position_data.viewport_scroll_position.has_value())
        return;

    // FIXME: If the document has been scrolled by the user, return.
    perform_scroll_of_viewport_scrolling_box(*scroll_position_data.viewport_scroll_position);
    clamp_viewport_scroll_offset();
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#nav-document
GC::Ptr<DOM::Document> LocalNavigable::active_document() const
{
    // A navigable's active document is its active session history entry's document.
    return m_active_document;
}

Optional<URL::URL> LocalNavigable::active_document_url() const
{
    if (!m_active_document)
        return {};
    return m_active_document->url();
}

Optional<URL::Origin> LocalNavigable::active_document_origin() const
{
    if (!m_active_document)
        return {};
    return m_active_document->origin();
}

ReplicatedNavigableState LocalNavigable::replicated_state() const
{
    VERIFY(m_active_document);
    VERIFY(m_active_session_history_entry);
    return {
        .target_name = target_name(),
        .active_document_url = m_active_document->url(),
        .active_document_origin = m_active_document->origin(),
        .active_document_is_fully_active = m_active_document->is_fully_active(),
        .active_session_history_entry_identity = session_history_entry_identity(*m_active_session_history_entry),
    };
}

Optional<UniqueNodeID> LocalNavigable::active_document_id() const
{
    if (!m_active_document)
        return {};
    return m_active_document->unique_id();
}

void LocalNavigable::set_active_document(GC::Ptr<DOM::Document> document)
{
    if (m_active_document && m_active_document != document) {
        // The pending post-scroll hover refresh and scrollend settlement belong to the outgoing document; drop them.
        cancel_hover_update_after_async_scroll();
        cancel_user_scroll_settlement();
        m_active_document->set_navigable(nullptr);
    }
    m_active_document = document;
    if (document)
        document->set_navigable(this);
    set_needs_to_record_display_list();

    VERIFY(m_active_session_history_entry);
    Optional<UniqueNodeID> document_id;
    if (document)
        document_id = document->unique_id();
    m_active_session_history_entry->document_state()->set_document_id(document_id);
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#nav-bc
GC::Ptr<BrowsingContext> LocalNavigable::active_browsing_context()
{
    // A navigable's active browsing context is its active document's browsing context.
    // If this navigable is a traversable navigable, then its active browsing context will be a top-level browsing context.
    if (auto document = active_document())
        return document->browsing_context();
    return nullptr;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#nav-wp
GC::Ptr<HTML::WindowProxy> LocalNavigable::active_window_proxy()
{
    // A navigable's active WindowProxy is its active browsing context's associated WindowProxy.
    if (auto browsing_context = active_browsing_context())
        return browsing_context->window_proxy();
    return nullptr;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#nav-window
GC::Ptr<HTML::Window> LocalNavigable::active_window()
{
    // A navigable's active window is its active WindowProxy's [[Window]].
    if (auto window_proxy = active_window_proxy())
        return window_proxy->window();
    return nullptr;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#nav-target
Utf16String const& LocalNavigable::target_name() const
{
    // A navigable's target name is its active session history entry's document state's navigable target name.
    return active_session_history_entry()->document_state()->navigable_target_name();
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#nav-container
GC::Ptr<NavigableContainer> LocalNavigable::container() const
{
    // The container of a navigable navigable is the navigable container whose nested navigable is navigable, or null if there is no such element.
    return m_container;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#nav-container-document
GC::Ptr<DOM::Document> LocalNavigable::container_document() const
{
    auto container = this->container();

    // 1. If navigable's container is null, then return null.
    if (!container)
        return nullptr;

    // 2. Return navigable's container's node document.
    return container->document();
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#nav-traversable
GC::Ptr<LocalTraversableNavigable> LocalNavigable::traversable_navigable() const
{
    // 1. Let navigable be inputNavigable.
    GC::Ptr<Navigable> navigable = const_cast<LocalNavigable*>(this);

    // 2. While navigable is not a traversable navigable, set navigable to navigable's parent.
    while (navigable && !is<LocalTraversableNavigable>(*navigable))
        navigable = navigable->parent();

    // 3. Return navigable.
    return navigable ? &as<LocalTraversableNavigable>(*navigable) : nullptr;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#set-the-ongoing-navigation
void LocalNavigable::set_ongoing_navigation(Variant<Empty, Traversal, Utf16String> ongoing_navigation, NavigationAPIAbortBehavior navigation_api_abort_behavior)
{
    // 1. If navigable's ongoing navigation is equal to newValue, then return.
    if (m_ongoing_navigation == ongoing_navigation)
        return;

    // 2. Inform the navigation API about aborting navigation given navigable.
    if (navigation_api_abort_behavior == NavigationAPIAbortBehavior::Abort)
        inform_the_navigation_api_about_aborting_navigation();

    // A UI-approved traversal supersedes any older navigation parked while the UI process
    // coordinates population. Do not let that navigation resume after the traversal finishes.
    if (ongoing_navigation.has<Traversal>() && m_ongoing_navigation.has<Utf16String>()) {
        if (take_navigation_parked_for_population(m_ongoing_navigation.get<Utf16String>()).has_value())
            set_delaying_load_events(false);
    }

    // 3. Set navigable's ongoing navigation to newValue.
    auto was_traversal = m_ongoing_navigation.has<Traversal>();
    m_ongoing_navigation = ongoing_navigation;

    for (auto& navigation_observer : m_navigation_observers) {
        if (navigation_observer.ongoing_navigation_changed())
            navigation_observer.ongoing_navigation_changed()->function()();
    }

    // AD-HOC: If we just finished a traversal and there are navigations that were deferred because the traversal was
    //         ongoing, process them now. A freshly-created child navigable can also have pending navigations while
    //         its initial session history entry is being installed, so only drain once both gates are open.
    if (was_traversal && !ongoing_navigation.has<Traversal>() && m_has_session_history_entry_and_ready_for_navigation)
        process_pending_navigations();
}

void LocalNavigable::queue_pending_navigation(PreparedNavigation navigation, PendingNavigationBehavior behavior)
{
    if (behavior == PendingNavigationBehavior::Replace)
        m_pending_navigations.remove_all_matching([](auto const& pending) { return !pending.population_navigation_id.has_value(); });
    m_pending_navigations.append({
        .navigation = move(navigation),
        .population_navigation_id = {},
        .continue_steps = nullptr,
    });
}

void LocalNavigable::clear_pending_navigations()
{
    auto had_navigation_parked_for_population = any_of(m_pending_navigations, [](auto const& pending) {
        return pending.population_navigation_id.has_value();
    });
    m_pending_navigations.clear();
    if (had_navigation_parked_for_population)
        set_delaying_load_events(false);
}

void LocalNavigable::park_navigation_for_population(Utf16String navigation_id, Optional<PreparedNavigation> navigation, GC::Ref<GC::Function<void(Optional<PreparedNavigation>, Optional<NavigationPopulationRequest>)>> continue_steps)
{
    // An overlapping navigation supersedes the previous one, but the previous navigation's
    // population dispatch can still be in flight. Keep it parked until its response or
    // cancellation arrives so that it can release any load-event delay it owns.
    m_pending_navigations.remove_all_matching([&](auto const& pending) { return pending.population_navigation_id == navigation_id; });
    m_pending_navigations.append({
        .navigation = move(navigation),
        .population_navigation_id = move(navigation_id),
        .continue_steps = continue_steps,
    });
}

Optional<LocalNavigable::PendingNavigation> LocalNavigable::take_navigation_parked_for_population(Utf16String const& navigation_id)
{
    auto index = m_pending_navigations.find_first_index_if([&](auto const& pending) {
        return pending.population_navigation_id == navigation_id;
    });
    if (!index.has_value())
        return {};
    return m_pending_navigations.take(*index);
}

void LocalNavigable::process_pending_navigations()
{
    if (!m_has_session_history_entry_and_ready_for_navigation || ongoing_navigation().has<Traversal>())
        return;

    while (true) {
        auto index = m_pending_navigations.find_first_index_if([](auto const& pending) {
            return !pending.population_navigation_id.has_value();
        });
        if (!index.has_value())
            return;
        auto pending = m_pending_navigations.take(*index);
        VERIFY(pending.navigation.has_value());
        begin_navigation(pending.navigation.release_value());
    }
}

void LocalNavigable::prepare_to_populate_reconstructed_history_entry(Utf16String navigation_api_key)
{
    auto index = m_pending_navigations.find_first_index_if([](auto const& pending) {
        return !pending.population_navigation_id.has_value();
    });
    if (index.has_value())
        m_pending_navigations.remove(*index);

    auto initial_entry = active_session_history_entry();
    VERIFY(initial_entry);
    initial_entry->set_navigation_api_key(move(navigation_api_key));

    set_delaying_load_events(true);
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#the-rules-for-choosing-a-navigable
LocalNavigable::ChosenNavigable LocalNavigable::choose_a_navigable(Utf16View name, TokenizedFeature::NoOpener no_opener, ActivateTab activate_tab, Optional<TokenizedFeature::Map const&> window_features)
{
    // 1. Let chosen be null.
    GC::Ptr<LocalNavigable> chosen = nullptr;

    // 2. Let windowType be "existing or none".
    auto window_type = WindowType::ExistingOrNone;

    // 3. Let sandboxingFlagSet be currentNavigable's active document's active sandboxing flag set.
    auto sandboxing_flag_set = active_document()->active_sandboxing_flag_set();

    // 4. If name is the empty string or an ASCII case-insensitive match for "_self", then set chosen to currentNavigable.
    if (name.is_empty() || name.equals_ignoring_ascii_case(u"_self"sv)) {
        chosen = this;
    }

    // 5. Otherwise, if name is an ASCII case-insensitive match for "_parent",
    //    set chosen to currentNavigable's parent, if any, and currentNavigable otherwise.
    else if (name.equals_ignoring_ascii_case(u"_parent"sv)) {
        if (auto parent = this->parent())
            chosen = as<LocalNavigable>(*parent);
        else
            chosen = this;
    }

    // 6. Otherwise, if name is an ASCII case-insensitive match for "_top",
    //    set chosen to currentNavigable's traversable navigable.
    else if (name.equals_ignoring_ascii_case(u"_top"sv)) {
        chosen = traversable_navigable();
    }

    // 7. Otherwise, if name is not an ASCII case-insensitive match for "_blank" and noopener is false, then set chosen
    //    to the result of finding a navigable by target name given name and currentNavigable.
    else if (!name.equals_ignoring_ascii_case(u"_blank"sv) && no_opener == TokenizedFeature::NoOpener::No) {
        chosen = find_a_navigable_by_target_name(name);
    }

    // 8. If chosen is null, then a new top-level traversable is being requested, and what happens depends on the user
    //    agent's configuration and abilities — it is determined by the rules given for the first applicable option
    //    from the following list:
    if (!chosen) {
        auto request_new_web_view = [&] {
            TokenizedFeature::Map empty_window_features;
            auto hints = WebViewHints::from_tokenised_features(window_features.has_value() ? *window_features : empty_window_features, traversable_navigable()->page());
            return traversable_navigable()->page().client().page_did_request_new_web_view(activate_tab, hints, no_opener);
        };

        // --> If currentNavigable's active window does not have transient activation and the user agent has been configured to
        //     not show popups (i.e., the user agent has a "popup blocker" enabled)
        if (active_window() && !active_window()->has_transient_activation() && traversable_navigable()->page().should_block_pop_ups()) {
            // FIXME: The user agent may inform the user that a popup has been blocked.
            dbgln("Pop-up blocked!");
        }

        // --> If sandboxingFlagSet has the sandboxed auxiliary navigation browsing context flag set
        else if (has_flag(sandboxing_flag_set, SandboxingFlagSet::SandboxedAuxiliaryNavigation)) {
            // FIXME: The user agent may report to a developer console that a popup has been blocked.
            dbgln("Pop-up blocked!");
        }

        // --> If the user agent has been configured such that in this instance it will create a new top-level traversable
        else if (auto new_web_view = request_new_web_view(); new_web_view.page) {
            // 1. Consume user activation of currentNavigable's active window.
            active_window()->consume_user_activation();

            // 2. Set windowType to "new and unrestricted".
            window_type = WindowType::NewAndUnrestricted;

            // 3. Let currentDocument be currentNavigable's active document.
            auto current_document = active_document();

            // 4. If currentDocument's opener policy's value is "same-origin" or "same-origin-plus-COEP",
            //    and currentDocument's origin is not same origin with currentDocument's relevant settings object's top-level origin, then:
            if ((current_document->opener_policy().value == OpenerPolicyValue::SameOrigin || current_document->opener_policy().value == OpenerPolicyValue::SameOriginPlusCOEP)
                && !current_document->origin().is_same_origin(relevant_settings_object(*current_document).top_level_origin.value())) {

                // 1. Set noopener to true.
                no_opener = TokenizedFeature::NoOpener::Yes;

                // 2. Set name to "_blank".
                name = u"_blank"sv;

                // 3. Set windowType to "new with no opener".
                window_type = WindowType::NewWithNoOpener;
            }
            // NOTE: In the presence of an opener policy,
            //       nested documents that are cross-origin with their top-level browsing context's active document always set noopener to true.

            // 5. Let targetName be the empty string.
            Utf16String target_name;

            // 6. If name is not an ASCII case-insensitive match for "_blank", then set targetName to name.
            if (!name.equals_ignoring_ascii_case(u"_blank"sv))
                target_name = Utf16String::from_utf16(name);

            auto create_new_traversable_closure = [page = new_web_view.page, window_handle = move(new_web_view.window_handle), target_name](GC::Ptr<BrowsingContext> opener) -> GC::Ref<LocalNavigable> {
                auto traversable = LocalTraversableNavigable::create_a_new_top_level_traversable(*page, opener, target_name);
                page->set_top_level_traversable(traversable);
                traversable->set_window_handle(Utf16String::from_ascii_without_validation(window_handle.bytes()));

                auto initial_history_entry = traversable->active_session_history_entry();
                VERIFY(initial_history_entry);
                page->client().page_did_create_top_level_traversable(
                    traversable->id(),
                    create_session_history_entry_descriptor(*initial_history_entry));
                return traversable;
            };
            auto create_new_traversable = GC::create_function(heap(), move(create_new_traversable_closure));

            // 7. If noopener is true, then set chosen to the result of creating a new top-level traversable given null and targetName.
            if (no_opener == TokenizedFeature::NoOpener::Yes) {
                chosen = create_new_traversable->function()(nullptr);
            }

            // 8. Otherwise:
            else {
                // 1. Set chosen to the result of creating a new top-level traversable given currentNavigable's active browsing context, targetName, and currentNavigable.
                // FIXME: "and currentNavigable", which is the openerNavigableForWebDriver parameter.
                chosen = create_new_traversable->function()(active_browsing_context());

                // 2. If sandboxingFlagSet's sandboxed navigation browsing context flag is set,
                //    then set chosen's active browsing context's one permitted sandboxed navigator to currentNavigable's active browsing context.
                if (has_flag(sandboxing_flag_set, SandboxingFlagSet::SandboxedNavigation))
                    chosen->active_browsing_context()->set_the_one_permitted_sandboxed_navigator(active_browsing_context().ptr());
            }

            // 9. If sandboxingFlagSet's sandbox propagates to auxiliary browsing contexts flag is set,
            //     then all the flags that are set in sandboxingFlagSet must be set in chosen's active browsing context's popup sandboxing flag set.
            if (has_flag(sandboxing_flag_set, SandboxingFlagSet::SandboxPropagatesToAuxiliaryBrowsingContexts))
                chosen->active_browsing_context()->set_popup_sandboxing_flag_set(chosen->active_browsing_context()->popup_sandboxing_flag_set() | sandboxing_flag_set);

            // 10. Set chosen's is created by web content to true.
            as<LocalTraversableNavigable>(*chosen).set_is_created_by_web_content(true);
        }

        // --> If the user agent has been configured such that in this instance it will choose currentNavigable
        else if (false) { // FIXME: When is this the case?
            // Set chosen to current.
            chosen = *this;
        }

        // --> If the user agent has been configured such that in this instance it will not find a navigable
        else if (false) { // FIXME: When is this the case?
            // Do nothing.
        }
    }

    // 9. Return chosen and windowType
    return { chosen.ptr(), window_type };
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#find-a-navigable-by-target-name
GC::Ptr<LocalNavigable> LocalNavigable::find_a_navigable_by_target_name(Utf16View name)
{
    // 1. Let currentDocument be currentNavigable's active document.
    auto& current_document = *active_document();

    // 2. Let sourceSnapshotParams be the result of snapshotting source snapshot params given currentDocument.
    auto source_snapshot_params = snapshot_source_snapshot_params(current_document);

    // 3. Let subtreesToSearch be an implementation-defined choice of one of the following:
    //    - « currentNavigable's traversable navigable, currentNavigable »
    //    - the inclusive ancestor navigables of currentDocument
    // FIXME: Decide which to use, or wait until the spec picks one.
    auto subtrees_to_search = current_document.inclusive_ancestor_navigables();

    // 4. For each subtreeToSearch of subtreesToSearch, in reverse order:
    for (auto const& subtree_to_search : subtrees_to_search.in_reverse()) {
        // 1. Let documentToSearch be subtreeToSearch's active document.
        auto& document_to_search = *as<LocalNavigable>(*subtree_to_search).active_document();

        // 2. For each navigable of the inclusive descendant navigables of documentToSearch:
        for (auto const& navigable : document_to_search.inclusive_descendant_navigables()) {
            // 1. If currentNavigable is not allowed by sandboxing to navigate navigable given sourceSnapshotParams, then optionally continue.
            if (!allowed_by_sandboxing_to_navigate(*navigable, source_snapshot_params))
                continue;

            // 2. If navigable's target name is name, then return navigable.
            auto const& target_name = navigable->target_name();
            if (target_name.utf16_view() == name)
                return *navigable;
        }
    }

    // 5. Let currentTopLevelBrowsingContext be currentNavigable's active browsing context's top-level browsing context.
    auto& current_top_level_browsing_context = *active_browsing_context()->top_level_browsing_context();

    // 6. Let group be currentTopLevelBrowsingContext's group.
    auto* group = current_top_level_browsing_context.group();

    // 7. For each topLevelBrowsingContext of group's browsing context set, in an implementation-defined order (the user agent should pick a consistent ordering, such as the most recently opened, most recently focused, or more closely related):
    for (auto const& top_level_browsing_context : group->browsing_context_set()) {
        // 1. If currentTopLevelBrowsingContext is topLevelBrowsingContext, then continue.
        if (&current_top_level_browsing_context == top_level_browsing_context.ptr())
            continue;

        // 2. Let documentToSearch be topLevelBrowsingContext's active document.
        auto* document_to_search = top_level_browsing_context->active_document();

        // 3. For each navigable of the inclusive descendant navigables of documentToSearch:
        for (auto const& navigable : document_to_search->inclusive_descendant_navigables()) {
            // 1. If currentNavigable's active browsing context is not familiar with navigable's active browsing context, then continue.
            if (!active_browsing_context()->is_familiar_with(*navigable->active_browsing_context()))
                continue;

            // 2. If currentNavigable is not allowed by sandboxing to navigate navigable given sourceSnapshotParams, then optionally continue.
            if (!allowed_by_sandboxing_to_navigate(*navigable, source_snapshot_params))
                continue;

            // 3. If navigable's target name is name, then return navigable.
            auto const& target_name = navigable->target_name();
            if (target_name.utf16_view() == name)
                return *navigable;
        }
    }

    // 8. Return null.
    return nullptr;
}

// https://html.spec.whatwg.org/multipage/browsers.html#determining-navigation-params-policy-container
static GC::Ref<PolicyContainer> determine_navigation_params_policy_container(URL::URL const& response_url,
    GC::Heap& heap,
    GC::Ptr<PolicyContainer> history_policy_container,
    GC::Ptr<PolicyContainer> initiator_policy_container,
    GC::Ptr<PolicyContainer> parent_policy_container,
    GC::Ptr<PolicyContainer> response_policy_container)
{
    // 1. If historyPolicyContainer is not null, then:
    if (history_policy_container) {
        // FIXME: 1. Assert: responseURL requires storing the policy container in history.

        // 2. Return a clone of historyPolicyContainer.
        return history_policy_container->clone(heap);
    }

    // 2. If responseURL is about:srcdoc, then:
    if (response_url == URL::about_srcdoc()) {
        // 1. Assert: parentPolicyContainer is not null.
        VERIFY(parent_policy_container);

        // 2. Return a clone of parentPolicyContainer.
        return parent_policy_container->clone(heap);
    }

    // 3. If responseURL is local and initiatorPolicyContainer is not null, then return a clone of initiatorPolicyContainer.
    if (Fetch::Infrastructure::is_local_url(response_url) && initiator_policy_container)
        return initiator_policy_container->clone(heap);

    // 4. If responsePolicyContainer is not null, then return responsePolicyContainer.
    // FIXME: File a spec issue to say "a clone of" here for consistency
    if (response_policy_container)
        return response_policy_container->clone(heap);

    // 5. Return a new policy container.
    return heap.allocate<PolicyContainer>(heap);
}

// https://html.spec.whatwg.org/multipage/browsers.html#obtain-coop
static OpenerPolicy obtain_an_opener_policy(GC::Ref<Fetch::Infrastructure::Response>, Fetch::Infrastructure::Request::ReservedClientType const& reserved_client)
{

    // 1. Let policy be a new opener policy.
    OpenerPolicy policy = {};

    // AD-HOC: We don't yet setup environments in all cases
    if (!reserved_client)
        return policy;

    auto& reserved_environment = *reserved_client;

    // 2. If reservedEnvironment is a non-secure context, then return policy.
    if (is_non_secure_context(reserved_environment))
        return policy;

    // FIXME: We don't yet have the technology to extract structured data from Fetch headers
    // FIXME: 3. Let parsedItem be the result of getting a structured field value given `Cross-Origin-Opener-Policy` and "item" from response's header list.
    // FIXME: 4. If parsedItem is not null, then:
    //     FIXME: nested steps...
    // FIXME: 5. Set parsedItem to the result of getting a structured field value given `Cross-Origin-Opener-Policy-Report-Only` and "item" from response's header list.
    // FIXME: 6. If parsedItem is not null, then:
    //     FIXME: nested steps...

    // 7. Return policy.
    return policy;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#hand-off-to-external-software
// FIXME: `resource` can also be a response: https://fetch.spec.whatwg.org/#concept-response
static void hand_off_to_external_software(URL::URL const& resource, GC::Ref<LocalNavigable> navigable, SandboxingFlagSet sandboxing_flags, bool has_transient_activation, URL::Origin const& initiator_origin)
{
    // 1. If all of the following are true:
    //    - navigable is not a top-level traversable;
    //    - sandboxFlags has its sandboxed custom protocols navigation browsing context flag set; and
    //    - sandboxFlags has its sandboxed top-level navigation with user activation browsing context flag set, or
    //      hasTransientActivation is false,
    //    then return without invoking the external software package.
    if (!navigable->is_top_level_traversable()
        && has_flag(sandboxing_flags, SandboxingFlagSet::SandboxedCustomProtocols)
        && (has_flag(sandboxing_flags, SandboxingFlagSet::SandboxedTopLevelNavigationWithUserActivation) || !has_transient_activation)) {
        return;
    }

    // 2. Perform the appropriate handoff of resource while attempting to mitigate the risk that this is an attempt to
    //    exploit the target software. For example, user agents could prompt the user to confirm that initiatorOrigin is
    //    to be allowed to invoke the external software in question. In particular, if hasTransientActivation is false,
    //    then the user agent should not invoke the external software package without prior user confirmation.
    navigable->page().client().page_did_request_external_url(resource, initiator_origin, has_transient_activation);
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#attempt-to-create-a-non-fetch-scheme-document
static GC::Ptr<DOM::Document> attempt_to_create_a_non_fetch_scheme_document(NonFetchSchemeNavigationParams const& params)
{
    // 1. Let url be navigationParams's URL.
    auto const& url = params.url;

    // 2. Let navigable be navigationParams's navigable.
    auto navigable = params.navigable;

    // 3. If url is to be handled using a mechanism that does not affect navigable, e.g., because url's scheme is
    //    handled externally, then:
    // AD-HOC: Checking if there is external software to hand-off to is done asynchronously, so we don't know here if it
    //         will succeed or not. Only a few cases reject it early. We find out that a URL went unhandled in
    //         ViewImplementation::handle_external_url(), so an equivalent of step 4 is implemented there.
    {
        // 1. Hand-off to external software given url, navigable, navigationParams's target snapshot sandboxing flags,
        //    navigationParams's source snapshot has transient activation, and navigationParams's initiator origin.
        hand_off_to_external_software(url, *navigable, params.target_snapshot_sandboxing_flags, params.source_snapshot_has_transient_activation, params.initiator_origin);

        // 2. Return null.
        return {};
    }

    // 4. Handle url by displaying some sort of inline content, e.g., an error message because the specified scheme is
    //    not one of the supported protocols, or an inline prompt to allow the user to select a registered handler for
    //    the given scheme. Return the result of displaying the inline content given navigable, navigationParams's id,
    //    navigationParams's navigation timing type, and navigationParams's user involvement.
    // AD-HOC: Not implemented here, see note on step 3 above.
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#create-navigation-params-from-a-srcdoc-resource
static GC::Ref<NavigationParams> create_navigation_params_from_a_srcdoc_resource(
    DocumentResource const& document_resource,
    Optional<URL::Origin> const& origin,
    Variant<SerializedPolicyContainer, DocumentState::Client> const& history_policy_container_variant,
    Optional<URL::URL> const& about_base_url,
    GC::Ptr<LocalNavigable> navigable,
    TargetSnapshotParams const& target_snapshot_params,
    UserNavigationInvolvement user_involvement,
    Optional<Utf16String> navigation_id)
{
    auto& vm = navigable->vm();
    VERIFY(navigable->active_window());
    auto& realm = navigable->active_window()->principal_realm();

    // 1. Let documentResource be entry's document state's resource.
    VERIFY(document_resource.has<Utf16String>());

    // 2. Let response be a new response with
    //    URL: about:srcdoc
    //    header list: (`Content-Type`, `text/html`)
    //    body: the UTF-8 encoding of documentResource, as a body
    auto response = Fetch::Infrastructure::Response::create(vm);
    response->url_list().append(URL::about_srcdoc());
    response->header_list()->append({ "Content-Type"sv, "text/html"sv });
    auto document_resource_utf8 = MUST(document_resource.get<Utf16String>().utf16_view().to_utf8());
    response->set_body(Fetch::Infrastructure::byte_sequence_as_body(realm, document_resource_utf8.bytes()));

    // 3. Let responseOrigin be the result of determining the origin given response's URL, targetSnapshotParams's sandboxing flags, and entry's document state's origin.
    auto response_origin = determine_the_origin(response->url(), target_snapshot_params.sandboxing_flags, origin);

    // 4. Let coop be a new opener policy.
    OpenerPolicy coop = {};

    // 5. Let coopEnforcementResult be a new opener policy enforcement result with
    //    url: response's URL
    //    origin: responseOrigin
    //    opener policy: coop
    OpenerPolicyEnforcementResult coop_enforcement_result {
        .url = *response->url(),
        .origin = response_origin,
        .opener_policy = coop
    };

    // 6. Let policyContainer be the result of determining navigation params policy container given response's URL,
    //    entry's document state's history policy container, null, navigable's container document's policy container, and null.
    GC::Ptr<PolicyContainer> history_policy_container = history_policy_container_variant.visit(
        [&](SerializedPolicyContainer const& s) -> GC::Ptr<PolicyContainer> { return create_a_policy_container_from_serialized_policy_container(s); },
        [](DocumentState::Client) -> GC::Ptr<PolicyContainer> { return {}; });
    GC::Ptr<PolicyContainer> policy_container;
    if (navigable->container()) {
        // NOTE: Specification assumes that only navigables corresponding to iframes can be navigated to about:srcdoc.
        //       We also use srcdoc to implement load_html() for top level navigables so we need to null check container
        //       because it might be null.
        policy_container = determine_navigation_params_policy_container(*response->url(), realm.heap(), history_policy_container, {}, navigable->container_document()->policy_container(), {});
    } else {
        policy_container = realm.heap().allocate<PolicyContainer>(realm.heap());
    }

    // 7. Return a new navigation params, with
    //    id: navigationId
    //    navigable: navigable
    //    request: null
    //    response: response
    //    fetch controller: null
    //    commit early hints: null
    //    COOP enforcement result: coopEnforcementResult
    //    reserved environment: null
    //    origin: responseOrigin
    //    policy container: policyContainer
    //    final sandboxing flag set: targetSnapshotParams's sandboxing flags
    //    iframe element referrer policy: targetSnapshotParams's iframe element referrer policy
    //    opener policy: coop
    //    FIXME: navigation timing type: navTimingType
    //    about base URL: entry's document state's about base URL
    //    user involvement: userInvolvement
    return vm.heap().allocate<NavigationParams>(
        move(navigation_id),
        navigable,
        nullptr,
        response,
        nullptr,
        nullptr,
        move(coop_enforcement_result),
        nullptr,
        move(response_origin),
        *policy_container,
        target_snapshot_params.sandboxing_flags,
        target_snapshot_params.iframe_element_referrer_policy,
        move(coop),
        about_base_url,
        user_involvement);
}

static void perform_navigation_params_fetch(JS::Realm& realm, GC::Ref<NavigationParamsFetchStateHolder> state_holder, GC::Ref<GC::Function<void(GC::Ref<InternalNavigationResult>)>> top_level_completion_steps, GC::Ref<GC::Function<void()>> fetch_completion_steps)
{
    // 21. While true:
    // NOTE: To make this async, a loop is performed by calling "perform_navigation_params_fetch" again from within "perform_navigation_params_fetch",
    //       performs breaks by calling the passed in fetch completion steps and then returning and performs returns by
    //       calling the top level completion steps and then returning.

    // 1. If request's reserved client is not null and currentURL's origin is not the same as request's reserved client's creation URL's origin, then:
    if (state_holder->request->reserved_client() && !state_holder->current_url.origin().is_same_origin(state_holder->request->reserved_client()->creation_url.origin())) {
        // 1. Run the environment discarding steps for request's reserved client.
        state_holder->request->reserved_client()->discard_environment();

        // 2. Set request's reserved client to null.
        state_holder->request->set_reserved_client(nullptr);

        // 3. Set commitEarlyHints to null.
        state_holder->commit_early_hints = nullptr;
    }

    // 2. If request's reserved client is null, then:
    if (!state_holder->request->reserved_client()) {
        // 1. Let topLevelCreationURL be currentURL.
        Optional<URL::URL> top_level_creation_url = state_holder->current_url;

        // 2. Let topLevelOrigin be null.
        Optional<URL::Origin> top_level_origin;

        // 3. If navigable is not a top-level traversable, then:
        if (!state_holder->navigable->is_top_level_traversable()) {
            // 1. Let parentEnvironment be navigable's parent's active document's relevant settings object.
            auto parent = state_holder->navigable->parent();
            auto* local_parent = parent ? &as<LocalNavigable>(*parent) : nullptr;
            auto parent_document = local_parent ? local_parent->active_document() : nullptr;
            if (!local_parent || local_parent->has_been_destroyed() || !parent_document || parent_document->has_been_destroyed()) {
                // AD-HOC: A queued child navigation can resume after its parent document has been destroyed. The
                //         specification assumes the parent environment is still available here, but browser engines
                //         abandon this stale detached frame navigation instead of continuing it against a discarded
                //         parent.
                state_holder->response = Fetch::Infrastructure::Response::network_error(realm.vm(), "Parent document is no longer active"_string);
                fetch_completion_steps->function()();
                return;
            }
            auto& parent_environment = parent_document->relevant_settings_object();

            // 2. Set topLevelCreationURL to parentEnvironment's top-level creation URL.
            top_level_creation_url = parent_environment.top_level_creation_url;

            // 3. Set topLevelOrigin to parentEnvironment's top-level origin.
            top_level_origin = parent_environment.top_level_origin;
        }

        // 4. Set request's reserved client to a new environment whose id is a unique opaque string,
        //    target browsing context is navigable's active browsing context,
        //    creation URL is currentURL,
        //    top-level creation URL is topLevelCreationURL,
        //    and top-level origin is topLevelOrigin.
        // FIXME: Make this a proper unique opaque string.
        static int next_id = 1;
        auto id_string = Utf16String::formatted("create-by-fetching-{}", next_id++);
        state_holder->request->set_reserved_client(realm.create<Environment>(id_string, state_holder->current_url, top_level_creation_url, top_level_origin, state_holder->navigable->active_browsing_context()));
    }

    // 3. If the result of should navigation request of type be blocked by Content Security Policy? given request and cspNavigationType is "Blocked", then set response to a network error and break. [CSP]
    if (ContentSecurityPolicy::should_navigation_request_of_type_be_blocked_by_content_security_policy(state_holder->request, state_holder->csp_navigation_type) == ContentSecurityPolicy::Directives::Directive::Result::Blocked) {
        state_holder->response = Fetch::Infrastructure::Response::network_error(realm.vm(), "Blocked by Content Security Policy"_string);
        fetch_completion_steps->function()();
        return;
    }

    // 4. Set response to null.
    state_holder->response = nullptr;

    // 5. If fetchController is null, then set fetchController to the result of fetching request,
    //    with processEarlyHintsResponse set to processEarlyHintsResponse as defined below, processResponse
    //    set to processResponse as defined below, and useParallelQueue set to true.
    if (!state_holder->fetch_controller) {
        // FIXME: Let processEarlyHintsResponse be the following algorithm given a response earlyResponse:

        // Let processResponse be the following algorithm given a response fetchedResponse:
        auto process_response = [state_holder](GC::Ref<Fetch::Infrastructure::Response> fetch_response) {
            // 1. Set response to fetchedResponse.
            state_holder->response = fetch_response;
            VERIFY(state_holder->continuation_steps);
            state_holder->continuation_steps->function()(NavigationParamsFetchStateHolder::ContinuationReason::GotResponse);
        };

        state_holder->fetch_controller = Fetch::Fetching::fetch(
            realm,
            state_holder->request,
            Fetch::Infrastructure::FetchAlgorithms::create(realm.vm(),
                {
                    .process_request_body_chunk_length = {},
                    .process_request_end_of_body = {},
                    .process_early_hints_response = {},
                    .process_response = move(process_response),
                    .process_response_end_of_body = {},
                    .process_response_consume_body = {},
                }),
            Fetch::Fetching::UseParallelQueue::Yes,
            Fetch::Fetching::CreateResponseBodyTransferLease::Yes);
    }
    // 6. Otherwise, process the next manual redirect for fetchController.
    else {
        state_holder->fetch_controller->process_next_manual_redirect();
    }

    // 7. Wait until either response is non-null, or navigable's ongoing navigation changes to no longer equal navigationId.
    GC::Ptr<NavigationObserver> ongoing_navigation_changed_observer;
    if (state_holder->navigation_id.has_value()) {
        ongoing_navigation_changed_observer = NavigationObserver::create(*state_holder->navigable);
        ongoing_navigation_changed_observer->set_ongoing_navigation_changed([state_holder] {
            VERIFY(state_holder->continuation_steps);
            state_holder->continuation_steps->function()(NavigationParamsFetchStateHolder::ContinuationReason::OngoingNavigationChanged);
        });
    }

    state_holder->continuation_steps = GC::create_function(realm.heap(), [&realm, state_holder, ongoing_navigation_changed_observer, top_level_completion_steps, fetch_completion_steps](NavigationParamsFetchStateHolder::ContinuationReason continuation_reason) {
        // If the latter condition occurs, then abort fetchController, and return. Otherwise, proceed onward.
        if (state_holder->navigation_id.has_value()) {
            VERIFY(ongoing_navigation_changed_observer);
            ongoing_navigation_changed_observer->set_ongoing_navigation_changed({});

            if (continuation_reason == NavigationParamsFetchStateHolder::ContinuationReason::OngoingNavigationChanged) {
                if (state_holder->navigable->ongoing_navigation() != *state_holder->navigation_id) {
                    state_holder->fetch_controller->abort(realm, {});
                    auto result = realm.heap().allocate<InternalNavigationResult>();
                    result->navigation_params = LocalNavigable::NullOrError {};
                    top_level_completion_steps->function()(*result);
                    return;
                }
            }
        }

        // 8. If request's body is null, then set entry's document state's resource to null.
        if (state_holder->request->body().has<Empty>()) {
            state_holder->resource_cleared = true;
            state_holder->resource = Empty {};
        }

        // 9. Set responsePolicyContainer to the result of creating a policy container from a fetch response given response and request's reserved client.
        state_holder->response_policy_container = create_a_policy_container_from_a_fetch_response(*state_holder->response, nullptr);

        // 10. Set finalSandboxFlags to the union of targetSnapshotParams's sandboxing flags and responsePolicyContainer's CSP list's CSP-derived sandboxing flags.
        state_holder->final_sandbox_flags = state_holder->target_snapshot_params.sandboxing_flags | state_holder->response_policy_container->csp_list->csp_derived_sandboxing_flags();

        // 11. Set responseOrigin to the result of determining the origin given response's URL, finalSandboxFlags, and entry's document state's initiator origin.
        state_holder->response_origin = determine_the_origin(state_holder->response->url(), state_holder->final_sandbox_flags, state_holder->initiator_origin);

        // 12. If navigable is a top-level traversable, then:
        if (state_holder->navigable->is_top_level_traversable()) {
            // 1. Set responseCOOP to the result of obtaining an opener policy given response and request's reserved client.
            state_holder->response_coop = obtain_an_opener_policy(*state_holder->response, state_holder->request->reserved_client());

            // FIXME: 2. Set coopEnforcementResult to the result of enforcing the response's opener policy given navigable's active browsing context,
            //    response's URL, responseOrigin, responseCOOP, coopEnforcementResult and request's referrer.

            // FIXME: 3. If finalSandboxFlags is not empty and responseCOOP's value is not "unsafe-none", then set response to an appropriate network error and break.
            // NOTE: This results in a network error as one cannot simultaneously provide a clean slate to a response
            //       using opener policy and sandbox the result of navigating to that response.
        }

        // 13. FIXME: If response is not a network error, navigable is a child navigable, and the result of performing a cross-origin resource policy check
        //    with navigable's container document's origin, navigable's container document's relevant settings object, request's destination, response,
        //    and true is blocked, then set response to a network error and break.
        // NOTE: Here we're running the cross-origin resource policy check against the parent navigable rather than navigable itself
        //       This is because we care about the same-originness of the embedded content against the parent context, not the navigation source.

        // 14. Set locationURL to response's location URL given currentURL's fragment.
        state_holder->location_url = state_holder->response->location_url(state_holder->current_url.fragment());

        // 15. If locationURL is failure or null, then break.
        if (state_holder->location_url.is_error() || !state_holder->location_url.value().has_value()) {
            fetch_completion_steps->function()();
            return;
        }

        // 16. Assert: locationURL is a URL.
        // 17. Set entry's classic history API state to StructuredSerializeForStorage(null).
        state_holder->redirect_classic_history_api_state = MUST(structured_serialize_for_storage(realm.vm(), JS::js_null()));

        // 18. Let oldDocState be entry's document state.

        // 19. Set entry's document state to a new document state, with
        // history policy container: a clone of the oldDocState's history policy container if it is non-null; null otherwise
        // request referrer: oldDocState's request referrer
        // request referrer policy: oldDocState's request referrer policy
        // origin: oldDocState's origin
        // resource: oldDocState's resource
        // ever populated: oldDocState's ever populated
        // navigable target name: oldDocState's navigable target name
        auto new_doc_state = DocumentState::create(state_holder->navigable->page().client().allocate_cross_process_id());
        new_doc_state->set_history_policy_container(state_holder->history_policy_container);
        new_doc_state->set_request_referrer(state_holder->request_referrer);
        new_doc_state->set_request_referrer_policy(state_holder->request_referrer_policy);
        new_doc_state->set_origin(state_holder->origin);
        new_doc_state->set_resource(state_holder->resource);
        new_doc_state->set_ever_populated(state_holder->ever_populated);
        new_doc_state->set_navigable_target_name(state_holder->navigable_target_name);
        state_holder->replacement_document_state = new_doc_state;
        state_holder->initiator_origin = {};
        state_holder->about_base_url = {};

        // 20. If locationURL's scheme is not an HTTP(S) scheme, then:
        if (!Fetch::Infrastructure::is_http_or_https_scheme(state_holder->location_url.value()->scheme())) {
            // 1. Set entry's document state's resource to null.
            state_holder->replacement_document_state->set_resource(Empty {});

            // 2. Break.
            fetch_completion_steps->function()();
            return;
        }

        // 21. Set currentURL to locationURL.
        state_holder->current_url = state_holder->location_url.value().value();

        // 22. Set entry's URL to currentURL.
        state_holder->redirected_url = state_holder->current_url;

        perform_navigation_params_fetch(realm, state_holder, top_level_completion_steps, fetch_completion_steps);
    });
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#create-navigation-params-by-fetching
static void create_navigation_params_by_fetching(
    URL::URL url,
    DocumentResource document_resource,
    Fetch::Infrastructure::Request::ReferrerType request_referrer,
    ReferrerPolicy::ReferrerPolicy request_referrer_policy,
    Optional<URL::Origin> initiator_origin,
    Variant<SerializedPolicyContainer, DocumentState::Client> history_policy_container,
    Optional<URL::URL> about_base_url,
    Optional<URL::Origin> origin,
    Utf16String navigable_target_name,
    bool reload_pending,
    bool ever_populated,
    GC::Ptr<LocalNavigable> navigable,
    GC::Ref<SourceSnapshotParams> source_snapshot_params,
    TargetSnapshotParams const& target_snapshot_params,
    ContentSecurityPolicy::Directives::Directive::NavigationType csp_navigation_type,
    UserNavigationInvolvement user_involvement,
    Optional<Utf16String> navigation_id,
    GC::Ref<GC::Function<void(GC::Ref<InternalNavigationResult>)>> completion_steps)
{
    auto& vm = navigable->vm();
    VERIFY(navigable->active_window());
    auto& realm = navigable->active_window()->principal_realm();
    auto& active_document = *navigable->active_document();

    // FIXME: 1. Assert: this is running in parallel.

    // 2. Let documentResource be entry's document state's resource.
    // NOTE: documentResource is passed as a parameter.

    // 3. Let request be a new request, with
    //    url: entry's URL
    //    client: sourceSnapshotParams's fetch client
    //    destination: "document"
    //    credentials mode: "include"
    //    use-URL-credentials flag: set
    //    redirect mode: "manual"
    //    replaces client id: navigable's active document's relevant settings object's id
    //    mode: "navigate"
    //    referrer: entry's document state's request referrer
    //    referrer policy: entry's document state's request referrer policy
    //    policy container: sourceSnapshotParams's source policy container
    auto request = Fetch::Infrastructure::Request::create(vm);
    request->set_url(url);
    request->set_client(source_snapshot_params->fetch_client);
    request->set_destination(Fetch::Infrastructure::Request::Destination::Document);
    request->set_credentials_mode(Fetch::Infrastructure::Request::CredentialsMode::Include);
    request->set_use_url_credentials(true);
    request->set_redirect_mode(Fetch::Infrastructure::Request::RedirectMode::Manual);
    request->set_replaces_client_id(active_document.relevant_settings_object().id);
    request->set_mode(Fetch::Infrastructure::Request::Mode::Navigate);
    request->set_referrer(request_referrer);
    request->set_referrer_policy(request_referrer_policy);
    request->set_policy_container(source_snapshot_params->source_policy_container);

    // 4. If navigable is a top-level traversable, then set request's top-level navigation initiator origin to entry's
    //    document state's initiator origin.
    if (navigable->is_top_level_traversable())
        request->set_top_level_navigation_initiator_origin(initiator_origin);

    // 5. If request's client is null:
    if (request->client() == nullptr) {
        // Note: This only occurs in the case of a browser UI-initiated navigation.

        // 1. Set request's origin to a new opaque origin.
        request->set_origin(URL::Origin::create_opaque());

        // 2. Set request's service-workers mode to "all".
        request->set_service_workers_mode(Fetch::Infrastructure::Request::ServiceWorkersMode::All);

        // 3. Set request's referrer to "no-referrer".
        request->set_referrer(Fetch::Infrastructure::Request::Referrer::NoReferrer);
    }

    // 6. If documentResource is a POST resource:
    if (auto* post_resource = document_resource.get_pointer<POSTResource>()) {
        // 1. Set request's method to `POST`.
        request->set_method("POST"sv);

        // 2. Set request's body to documentResource's request body.
        request->set_body(document_resource.get<POSTResource>().request_body.value());

        // 3. Set `Content-Type` to documentResource's request content-type in request's header list.
        auto request_content_type = [&]() {
            switch (post_resource->request_content_type) {
            case POSTResource::RequestContentType::ApplicationXWWWFormUrlencoded:
                return "application/x-www-form-urlencoded"sv;
            case POSTResource::RequestContentType::MultipartFormData:
                return "multipart/form-data"sv;
            case POSTResource::RequestContentType::TextPlain:
                return "text/plain"sv;
            default:
                VERIFY_NOT_REACHED();
            }
        }();

        StringBuilder request_content_type_buffer;
        if (!post_resource->request_content_type_directives.is_empty()) {
            request_content_type_buffer.append(request_content_type);

            for (auto const& directive : post_resource->request_content_type_directives)
                request_content_type_buffer.appendff("; {}={}", directive.type, directive.value);

            request_content_type = request_content_type_buffer.string_view();
        }

        auto header = HTTP::Header::isomorphic_encode("Content-Type"sv, request_content_type);
        request->header_list()->append(move(header));
    }

    // 7. If entry's document state's reload pending is true, then set request's reload-navigation flag.
    if (reload_pending) {
        request->set_reload_navigation(true);

        // AD-HOC: The specs don't define HTTP cache behavior for reloads. But every major engine forces at least re-
        //         validation of the reloaded document rather than serving it straight from cache. Use the "no-cache"
        //         cache mode, which per Fetch "creates a conditional request if there is a response in the HTTP cache
        //         and a normal request otherwise. It then updates the HTTP cache with the response." This matches
        //         Chromium (FetchCacheMode::kValidateCache) and Firefox (nsIRequest::VALIDATE_ALWAYS); WebKit goes
        //         further, and bypasses the cache entirely (ReloadIgnoringCacheData).
        request->set_cache_mode(HTTP::CacheMode::NoCache);
    }

    // 8. Otherwise, if entry's document state's ever populated is true, then set request's history-navigation flag.
    else if (ever_populated)
        request->set_history_navigation(true);

    // 9. If sourceSnapshotParams's has transient activation is true, then set request's user-activation to true.
    if (source_snapshot_params->has_transient_activation)
        request->set_user_activation(true);

    // 10. If navigable's container is non-null:
    if (navigable->container() != nullptr) {
        // 1. If the navigable's container has a browsing context scope origin, then set request's origin to that browsing context scope origin.
        // FIXME: From "browsing context scope origin": This definition is broken and needs investigation to see what it was intended to express: see issue #4703.
        //        The referenced issue suggests that it is a no-op to retrieve the browsing context scope origin.

        // 2. Set request's destination to navigable's container's local name.
        // FIXME: Are there other container types? If so, we need a helper here
        Web::Fetch::Infrastructure::Request::Destination destination = is<HTMLIFrameElement>(*navigable->container()) ? Web::Fetch::Infrastructure::Request::Destination::IFrame
                                                                                                                      : Web::Fetch::Infrastructure::Request::Destination::Object;
        request->set_destination(destination);

        // 3. If sourceSnapshotParams's fetch client is navigable's container document's relevant settings object,
        //    then set request's initiator type to navigable's container's local name.
        // NOTE: This ensure that only container-initiated navigations are reported to resource timing.
        if (source_snapshot_params->fetch_client.ptr() == &navigable->container_document()->relevant_settings_object()) {
            // FIXME: Are there other container types? If so, we need a helper here
            Web::Fetch::Infrastructure::Request::InitiatorType initiator_type = is<HTMLIFrameElement>(*navigable->container()) ? Web::Fetch::Infrastructure::Request::InitiatorType::IFrame
                                                                                                                               : Web::Fetch::Infrastructure::Request::InitiatorType::Object;
            request->set_initiator_type(initiator_type);
        }
    }

    // NOTE: We use a heap-allocated cell to hold all the following state because the callbacks below will use them
    //       after this stack is freed.
    // 11. Let response be null.
    // 12. Let responseOrigin be null.
    // 13. Let fetchController be null.

    // 14. Let coopEnforcementResult be a new opener policy enforcement result, with
    // - url: navigable's active document's URL
    // - origin: navigable's active document's origin
    // - opener policy: navigable's active document's opener policy
    // - current context is navigation source: true if navigable's active document's origin is same origin with
    //                                         entry's document state's initiator origin otherwise false
    OpenerPolicyEnforcementResult coop_enforcement_result = {
        .url = active_document.url(),
        .origin = active_document.origin(),
        .opener_policy = active_document.opener_policy(),
        .current_context_is_navigation_source = initiator_origin.has_value() && active_document.origin().is_same_origin(*initiator_origin)
    };

    // 15. Let finalSandboxFlags be an empty sandboxing flag set.
    // 16. Let responsePolicyContainer be null.
    // 17. Let responseCOOP be a new opener policy.
    // 18. Let locationURL be null.
    // 19. Let currentURL be request's current URL.
    // 20. Let commitEarlyHints be null.
    // AD-HOC: Store required variables on the state holder to keep them alive whilst waiting on the fetch to complete.
    auto state_holder = realm.heap().allocate<NavigationParamsFetchStateHolder>(move(coop_enforcement_result), request->current_url(), request,
        move(initiator_origin), move(history_policy_container), move(about_base_url), source_snapshot_params,
        request_referrer, request_referrer_policy, move(origin), move(document_resource), ever_populated, move(navigable_target_name));
    state_holder->navigable = navigable;
    state_holder->csp_navigation_type = csp_navigation_type;
    state_holder->target_snapshot_params = target_snapshot_params;
    state_holder->navigation_id = move(navigation_id);

    perform_navigation_params_fetch(realm, state_holder, completion_steps, GC::create_function(realm.heap(), [&realm, state_holder, user_involvement, completion_steps] {
        auto result = realm.heap().allocate<InternalNavigationResult>();
        result->redirected_url = move(state_holder->redirected_url);
        result->classic_history_api_state = move(state_holder->redirect_classic_history_api_state);
        result->replacement_document_state = state_holder->replacement_document_state;
        result->resource_cleared = state_holder->resource_cleared;

        // 22. If locationURL is a URL whose scheme is not a fetch scheme, then return a new non-fetch scheme navigation params, with
        if (!state_holder->location_url.is_error() && state_holder->location_url.value().has_value() && !Fetch::Infrastructure::is_fetch_scheme(state_holder->location_url.value().value().scheme())) {
            // - id: navigationId
            // - navigable: navigable
            // - URL: locationURL
            // - target snapshot sandboxing flags: targetSnapshotParams's sandboxing flags
            // - source snapshot has transient activation: sourceSnapshotParams's has transient activation
            // - initiator origin: responseOrigin
            // FIXME: - navigation timing type: navTimingType
            // - user involvement: userInvolvement
            result->navigation_params = realm.heap().allocate<NonFetchSchemeNavigationParams>(
                state_holder->navigation_id,
                state_holder->navigable,
                state_holder->location_url.value().value(),
                state_holder->target_snapshot_params.sandboxing_flags,
                state_holder->source_snapshot_params->has_transient_activation,
                *state_holder->response_origin,
                user_involvement);
            completion_steps->function()(*result);
            return;
        }

        // 23. If any of the following are true:
        //       - response is a network error;
        //       - locationURL is failure; or
        //       - locationURL is a URL whose scheme is a fetch scheme
        //     then return null.
        if (state_holder->response->is_network_error()) {
            // AD-HOC: We pass the error message if we have one in NullWithError
            result->navigation_params = state_holder->response->network_error_message().map([](auto const& error_message) {
                return Utf16String::from_utf8(error_message);
            });
            completion_steps->function()(*result);
            return;
        }

        if (state_holder->location_url.is_error() || (state_holder->location_url.value().has_value() && Fetch::Infrastructure::is_fetch_scheme(state_holder->location_url.value().value().scheme()))) {
            result->navigation_params = LocalNavigable::NullOrError {};
            completion_steps->function()(*result);
            return;
        }

        // 24. Assert: locationURL is null and response is not a network error.
        VERIFY(!state_holder->location_url.value().has_value());
        VERIFY(!state_holder->response->is_network_error());

        // 25. Let resultPolicyContainer be the result of determining navigation params policy container given response's URL,
        //     entry's document state's history policy container, sourceSnapshotParams's source policy container, null, and responsePolicyContainer.
        GC::Ptr<PolicyContainer> history_policy_container = state_holder->history_policy_container.visit(
            [&](SerializedPolicyContainer const& s) -> GC::Ptr<PolicyContainer> { return create_a_policy_container_from_serialized_policy_container(s); },
            [](DocumentState::Client) -> GC::Ptr<PolicyContainer> { return {}; });
        auto result_policy_container = determine_navigation_params_policy_container(*state_holder->response->url(), realm.heap(), history_policy_container, state_holder->source_snapshot_params->source_policy_container, {}, state_holder->response_policy_container);

        // 26. If navigable's container is an iframe, and response's timing allow passed flag is set,
        //     then set navigable's container's pending resource-timing start time to null.
        if (state_holder->navigable->container() && state_holder->response->timing_allow_passed()) {
            if (auto* iframe_element = as_if<HTML::HTMLIFrameElement>(*state_holder->navigable->container()))
                iframe_element->set_pending_resource_start_time({});
        }

        // 27. Return a new navigation params, with
        //     id: navigationId
        //     navigable: navigable
        //     request: request
        //     response: response
        //     fetch controller: fetchController
        //     commit early hints: commitEarlyHints
        //     opener policy: responseCOOP
        //     reserved environment: request's reserved client
        //     origin: responseOrigin
        //     policy container: resultPolicyContainer
        //     final sandboxing flag set: finalSandboxFlags
        //     COOP enforcement result: coopEnforcementResult
        //     FIXME: navigation timing type: navTimingType
        //     about base URL: entry's document state's about base URL
        //     user involvement: userInvolvement
        // FIXME: Value for iframe element referrer policy is missing in the spec. https://github.com/whatwg/html/issues/12567
        //        So, we default to the empty string.
        result->navigation_params = realm.heap().allocate<NavigationParams>(
            state_holder->navigation_id,
            state_holder->navigable,
            state_holder->request,
            *state_holder->response,
            state_holder->fetch_controller,
            state_holder->commit_early_hints,
            state_holder->coop_enforcement_result,
            state_holder->request->reserved_client(),
            *state_holder->response_origin,
            result_policy_container,
            state_holder->final_sandbox_flags,
            ReferrerPolicy::ReferrerPolicy::EmptyString,
            state_holder->response_coop,
            state_holder->about_base_url,
            user_involvement);
        completion_steps->function()(*result);
    }));
}

using NavigationParamsCreationCompletion = GC::Function<void(GC::Ref<InternalNavigationResult>)>;

static void create_navigation_params_for_population(
    LocalNavigable& navigable,
    URL::URL url,
    DocumentResource document_resource,
    Fetch::Infrastructure::Request::ReferrerType request_referrer,
    ReferrerPolicy::ReferrerPolicy request_referrer_policy,
    Optional<URL::Origin> initiator_origin,
    Optional<URL::Origin> origin,
    Variant<SerializedPolicyContainer, DocumentState::Client> history_policy_container,
    Optional<URL::URL> about_base_url,
    Utf16String navigable_target_name,
    bool reload_pending,
    bool ever_populated,
    GC::Ref<SourceSnapshotParams> source_snapshot_params,
    TargetSnapshotParams const& target_snapshot_params,
    UserNavigationInvolvement user_involvement,
    Optional<Utf16String> navigation_id,
    LocalNavigable::NavigationParamsVariant navigation_params,
    ContentSecurityPolicy::Directives::Directive::NavigationType csp_navigation_type,
    bool allow_POST,
    GC::Ref<NavigationParamsCreationCompletion> completion_steps)
{
    // Helper to wrap a NavigationParamsVariant in an InternalNavigationResult with no redirect mutations.
    auto wrap_navigation_params = [&](LocalNavigable::NavigationParamsVariant navigation_params) {
        auto result = navigable.heap().allocate<InternalNavigationResult>();
        result->navigation_params = move(navigation_params);
        completion_steps->function()(*result);
    };

    // 4. If navigationParams is null, then:
    if (navigation_params.has<LocalNavigable::NullOrError>()) {
        // 1. If documentResource is a string, then set navigationParams to the result of creating navigation params
        //    from a srcdoc resource given entry, navigable, targetSnapshotParams, userInvolvement, navigationId, and
        //    navTimingType.
        if (document_resource.has<Utf16String>()) {
            wrap_navigation_params(create_navigation_params_from_a_srcdoc_resource(
                document_resource,
                origin,
                history_policy_container,
                about_base_url,
                &navigable, target_snapshot_params, user_involvement, navigation_id));
        }
        // 2. Otherwise, if all of the following are true:
        //    - entry's URL's scheme is a fetch scheme; and
        //    - documentResource is null, or allowPOST is true and documentResource's request body is not failure,
        //      (FIXME: check if request body is not failure)
        //    then set navigationParams to the result of creating navigation params by fetching given entry, navigable,
        //    sourceSnapshotParams, targetSnapshotParams, cspNavigationType, userInvolvement, navigationId, and
        //    navTimingType.
        else if (Fetch::Infrastructure::is_fetch_scheme(url.scheme()) && (document_resource.has<Empty>() || allow_POST)) {
            create_navigation_params_by_fetching(
                url,
                document_resource,
                request_referrer,
                request_referrer_policy,
                initiator_origin,
                history_policy_container,
                about_base_url,
                origin,
                navigable_target_name,
                reload_pending,
                ever_populated,
                &navigable,
                source_snapshot_params,
                target_snapshot_params,
                csp_navigation_type,
                user_involvement,
                navigation_id,
                completion_steps);
        }
        // 3. Otherwise, if entry's URL's scheme is not a fetch scheme, then set navigationParams to a new non-fetch
        //    scheme navigation params, with:
        else if (!Fetch::Infrastructure::is_fetch_scheme(url.scheme())) {
            // - id: navigationId
            // - navigable: navigable
            // - URL: entry's URL
            // - target snapshot sandboxing flags: targetSnapshotParams's sandboxing flags
            // - source snapshot has transient activation: sourceSnapshotParams's has transient activation
            // - initiator origin: entry's document state's initiator origin
            // FIXME: - navigation timing type: navTimingType
            // - user involvement: userInvolvement
            wrap_navigation_params(navigable.vm().heap().allocate<NonFetchSchemeNavigationParams>(
                navigation_id,
                &navigable,
                url,
                target_snapshot_params.sandboxing_flags,
                source_snapshot_params->has_transient_activation,
                *initiator_origin,
                user_involvement));
        }
    } else {
        wrap_navigation_params(move(navigation_params));
    }
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#populating-a-session-history-entry
void LocalNavigable::populate_session_history_entry_document(
    URL::URL url,
    DocumentResource document_resource,
    Fetch::Infrastructure::Request::ReferrerType request_referrer,
    ReferrerPolicy::ReferrerPolicy request_referrer_policy,
    Optional<URL::Origin> initiator_origin,
    Optional<URL::Origin> origin,
    Variant<SerializedPolicyContainer, DocumentState::Client> history_policy_container,
    Optional<URL::URL> about_base_url,
    Utf16String navigable_target_name,
    bool reload_pending,
    bool ever_populated,
    GC::Ref<SourceSnapshotParams> source_snapshot_params,
    TargetSnapshotParams const& target_snapshot_params,
    UserNavigationInvolvement user_involvement,
    Optional<Utf16String> navigation_id,
    NavigationParamsVariant navigation_params,
    ContentSecurityPolicy::Directives::Directive::NavigationType csp_navigation_type,
    bool allow_POST,
    GC::Ptr<GC::Function<void(GC::Ptr<PopulateSessionHistoryEntryDocumentOutput>)>> completion_steps)
{
    // AD-HOC: Not in the spec but subsequent steps will fail if the navigable doesn't have an active window.
    if (!active_window()) {
        stop_or_resume_response_body_delivery(navigation_params);
        return;
    }

    auto received_navigation_params = GC::create_function(heap(), [this, url, navigation_id, user_involvement, completion_steps, csp_navigation_type, source_snapshot_params](GC::Ref<InternalNavigationResult> result) {
        // AD-HOC: Not in the spec but subsequent steps will fail if the navigable doesn't have an active window.
        if (!active_window()) {
            stop_or_resume_response_body_delivery(result->navigation_params);
            return;
        }

        auto output = heap().allocate<PopulateSessionHistoryEntryDocumentOutput>();
        // NB: result's redirect fields are moved into output here; population must read them from output rather than
        //     the moved-from result.
        output->redirected_url = move(result->redirected_url);
        output->classic_history_api_state = move(result->classic_history_api_state);
        output->replacement_document_state = result->replacement_document_state;
        output->resource_cleared = result->resource_cleared;

        queue_navigation_and_traversal_task_for_session_history_entry_population(
            url,
            source_snapshot_params->allows_downloading,
            source_snapshot_params->fetch_client ? Optional<URL::Origin> { source_snapshot_params->fetch_client->origin() } : Optional<URL::Origin> {},
            user_involvement,
            navigation_id,
            move(result->navigation_params),
            csp_navigation_type,
            output,
            completion_steps);
    });

    create_navigation_params_for_population(
        *this,
        move(url),
        move(document_resource),
        move(request_referrer),
        request_referrer_policy,
        move(initiator_origin),
        move(origin),
        move(history_policy_container),
        move(about_base_url),
        move(navigable_target_name),
        reload_pending,
        ever_populated,
        source_snapshot_params,
        target_snapshot_params,
        user_involvement,
        move(navigation_id),
        move(navigation_params),
        csp_navigation_type,
        allow_POST,
        received_navigation_params);
}

void LocalNavigable::queue_navigation_and_traversal_task_for_session_history_entry_population(
    URL::URL url,
    bool source_allows_downloading,
    Optional<URL::Origin> source_interface_origin,
    UserNavigationInvolvement user_involvement,
    Optional<Utf16String> navigation_id,
    NavigationParamsVariant navigation_params,
    ContentSecurityPolicy::Directives::Directive::NavigationType csp_navigation_type,
    GC::Ref<PopulateSessionHistoryEntryDocumentOutput> output,
    GC::Ptr<GC::Function<void(GC::Ptr<PopulateSessionHistoryEntryDocumentOutput>)>> completion_steps)
{
    if (!active_window()) {
        stop_or_resume_response_body_delivery(navigation_params);
        return;
    }

    // 5. Queue a global task on the navigation and traversal task source, given navigable's active window, to run these steps:
    queue_global_task(Task::Source::NavigationAndTraversal, HTML::relevant_global_object(*active_window()), GC::create_function(heap(), [this, url, source_allows_downloading, source_interface_origin, user_involvement, navigation_id, navigation_params, csp_navigation_type, output, completion_steps]() mutable {
        // 1. If navigable's ongoing navigation no longer equals navigationId, then run completionSteps and abort these steps.
        if (navigation_id.has_value() && ongoing_navigation() != navigation_id) {
            if (completion_steps)
                completion_steps->function()(nullptr);
            return;
        }

        // 2. Let saveExtraDocumentState be true.
        output->save_extra_document_state = true;

        // 3. If navigationParams is a non-fetch scheme navigation params, then:
        if (navigation_params.has<GC::Ref<NonFetchSchemeNavigationParams>>()) {
            // 1. Set entry's document state's document to the result of running attempt to create a non-fetch scheme
            //    document given navigationParams.
            //    NOTE: This can result in setting entry's document state's document to null, e.g., when handing-off to
            //    external software.
            output->document = attempt_to_create_a_non_fetch_scheme_document(navigation_params.get<GC::Ref<NonFetchSchemeNavigationParams>>());

            // 2. Set saveExtraDocumentState to false.
            output->save_extra_document_state = false;
        }

        // 4. Otherwise, if any of the following are true:
        //  - navigationParams is null;
        //  - the result of should navigation response to navigation request of type in target be blocked by Content Security Policy? given navigationParams's request, navigationParams's response, navigationParams's policy container's CSP list, cspNavigationType, and navigable is "Blocked";
        //  - FIXME: navigationParams's reserved environment is non-null and the result of checking a navigation response's adherence to its embedder policy given navigationParams's response, navigable, and navigationParams's policy container's embedder policy is false; or
        //  - the result of checking a navigation response's adherence to `X-Frame-Options` given navigationParams's response, navigable, navigationParams's policy container's CSP list, and navigationParams's origin is false,
        //    then:
        else if (navigation_params.visit(
                     [](NullOrError) { return true; },
                     [this, csp_navigation_type](GC::Ref<NavigationParams> navigation_params) {
                         auto csp_result = ContentSecurityPolicy::should_navigation_response_to_navigation_request_of_type_in_target_be_blocked_by_content_security_policy(navigation_params->request, *navigation_params->response, navigation_params->policy_container->csp_list, csp_navigation_type, *this);
                         if (csp_result == ContentSecurityPolicy::Directives::Directive::Result::Blocked)
                             return true;

                         // FIXME: Pass in navigationParams's policy container's CSP list
                         return !check_a_navigation_responses_adherence_to_x_frame_options(navigation_params->response, this, navigation_params->policy_container->csp_list, navigation_params->origin);
                     },
                     [](GC::Ref<NonFetchSchemeNavigationParams>) { return false; })) {
            // 1. Set entry's document state's document to the result of creating a document for inline content that doesn't have a DOM, given navigable, null, navTimingType, and userInvolvement. The inline content should indicate to the user the sort of error that occurred.
            auto error_message = navigation_params.has<NullOrError>() ? navigation_params.get<NullOrError>().value_or("Unknown error"_utf16) : "The request was denied."_utf16;
            auto error_message_utf8 = error_message.to_utf8();

            // AD-HOC: Name the URL that actually failed to load: The last URL the navigation was redirected to, if
            //         any — rather than the URL it started at.
            auto error_url = output->redirected_url.value_or(url);
            auto error_html = load_error_page(error_url, error_message_utf8).release_value_but_fixme_should_propagate_errors();
            output->document = create_document_for_inline_content(this, navigation_id, user_involvement, [this, error_html](auto& document) {
                auto scripting_mode = document.is_scripting_enabled() ? HTML::ParserScriptingMode::Normal : HTML::ParserScriptingMode::Disabled;
                auto parser = HTMLParser::create_from_byte_string(document, error_html, scripting_mode, "utf-8"sv);
                document.set_url(URL::about_error());
                parser->run();

                // FIXME: This should go in create_document_for_inline_content() instead.
                // FIXME: Directly calling parser->the_end results in a deadlock, because it waits for the warning image to load.
                //        However the response is never processed when parser->the_end is called.
                //        Queuing a global task is a workaround for now.
                queue_a_task(Task::Source::Unspecified, HTML::main_thread_event_loop(), document, GC::create_function(heap(), [&document, parser] {
                    HTMLParser::the_end(document, parser);
                }));
            });

            // 2. Make document unsalvageable given entry's document state's document and "navigation-failure".
            if (output->document)
                output->document->make_unsalvageable("navigation-failure"_utf16);

            // 3. Set saveExtraDocumentState to false.
            output->save_extra_document_state = false;

            // 4. If navigationParams is not null, then:
            if (!navigation_params.has<NullOrError>()) {
                // 1. Run the environment discarding steps for navigationParams's reserved environment.
                navigation_params.visit(
                    [](GC::Ref<NavigationParams> const& it) {
                        if (it->fetch_controller)
                            it->fetch_controller->stop_fetch();
                        else
                            it->response->resume_body_delivery();
                        it->reserved_environment->discard_environment();
                    },
                    [](auto const&) {});

                // FIXME: 2. Invoke WebDriver BiDi navigation failed with navigable and a new WebDriver BiDi navigation status whose id is navigationId, status is "canceled", and url is navigationParams's response's URL.
            }
        }

        // 5. Otherwise, if navigationParams's response has a `Content-Disposition` header specifying the attachment
        //    disposition type, then:
        else if (auto nav_params = navigation_params.get<GC::Ref<NavigationParams>>();
            parse_content_disposition(*nav_params->response->header_list()).is_attachment) {
            output->download_handled = handle_navigation_response_as_download(nav_params, source_allows_downloading, source_interface_origin);
            output->save_extra_document_state = false;
        }

        // 6. Otherwise, if navigationParams's response's status is not 204 and is not 205, then set entry's document state's document to the result of
        //    loading a document given navigationParams, sourceSnapshotParams, and entry's document state's initiator origin.
        else if (auto const& response = navigation_params.get<GC::Ref<NavigationParams>>()->response; response->status() != 204 && response->status() != 205) {
            auto nav_params = navigation_params.get<GC::Ref<NavigationParams>>();
            auto body = nav_params->response->body();

            // Get sniff bytes for MIME type detection. For streaming responses where bytes
            // haven't arrived yet, we must wait asynchronously.
            auto sniff_bytes = body ? body->sniff_bytes_if_available() : Optional<ReadonlyBytes> { ReadonlyBytes {} };
            if (!sniff_bytes.has_value()) {
                // Async path: bytes not yet available, wait for them
                nav_params->response->resume_body_delivery_up_to(Fetch::Infrastructure::MAX_SNIFF_BYTES);
                body->wait_for_sniff_bytes(GC::create_function(heap(),
                    [output, nav_params, navigation_params, completion_steps, source_allows_downloading, source_interface_origin](ReadonlyBytes sniff_bytes) {
                        // AD-HOC: The document may have been destroyed between when the fetch started and when the
                        //         bytes arrived.
                        if (nav_params->navigable->active_browsing_context()) {
                            output->document = load_document(nav_params, sniff_bytes);
                            if (!output->document) {
                                output->download_handled = handle_navigation_response_as_download(nav_params, source_allows_downloading, source_interface_origin, sniff_bytes);
                                output->save_extra_document_state = false;
                            } else {
                                nav_params->response->resume_body_delivery();
                            }
                        } else {
                            stop_or_resume_response_body_delivery(navigation_params);
                        }
                        output->navigation_params = navigation_params;
                        if (completion_steps)
                            completion_steps->function()(output);
                    }));
                return;
            }

            // Sync path: bytes available immediately
            output->document = load_document(nav_params, sniff_bytes.value());
            if (!output->document) {
                output->download_handled = handle_navigation_response_as_download(nav_params, source_allows_downloading, source_interface_origin, sniff_bytes.value());
                output->save_extra_document_state = false;
            } else {
                nav_params->response->resume_body_delivery();
            }
        } else {
            auto nav_params = navigation_params.get<GC::Ref<NavigationParams>>();
            nav_params->response->release_request_transfer_lease();
        }

        output->navigation_params = navigation_params;
        if (completion_steps)
            completion_steps->function()(output);
    }));
}

void LocalNavigable::create_navigation_params_for_navigation(NavigationPopulationRequest request, GC::Ref<SourceSnapshotParams> source_snapshot_params, NavigationParamsVariant navigation_params)
{
    auto navigation_id = request.navigation_id;

    if (!active_window()) {
        stop_or_resume_response_body_delivery(navigation_params);
        return;
    }

    // 3. Queue a global task on the navigation and traversal task source given navigable's active window to abort a document and its descendants given navigable's active document.
    queue_global_task(Task::Source::NavigationAndTraversal, HTML::relevant_global_object(*active_window()), GC::create_function(heap(), [this] {
        active_document()->abort_a_document_and_its_descendants();
    }));

    auto received_navigation_params = GC::create_function(heap(), [this, request, navigation_id](GC::Ref<InternalNavigationResult> result) mutable {
        if (!active_window() || ongoing_navigation() != navigation_id) {
            stop_or_resume_response_body_delivery(result->navigation_params);
            return;
        }

        auto& realm = active_window()->principal_realm();
        create_navigation_params_descriptor(realm, result->navigation_params, GC::create_function(heap(), [this, request = move(request), navigation_id, result](NavigationParamsVariantDescriptor navigation_params) mutable {
            if (!active_window() || ongoing_navigation() != navigation_id) {
                stop_or_resume_response_body_delivery(result->navigation_params);
                return;
            }

            Optional<SessionHistoryDocumentStateDescriptor> replacement_document_state;
            if (result->replacement_document_state)
                replacement_document_state = create_session_history_document_state_descriptor(*result->replacement_document_state);

            auto population_result = NavigationPopulationResult {
                .navigation_params = move(navigation_params),
                .redirected_url = move(result->redirected_url),
                .classic_history_api_state = move(result->classic_history_api_state),
                .replacement_document_state = move(replacement_document_state),
                .resource_cleared = result->resource_cleared,
            };
            page().client().navigation_params_creation_finished(*this, move(request), move(population_result));
        }));
    });

    auto const& document_state = request.history_entry.document_state;
    create_navigation_params_for_population(
        *this,
        request.history_entry.url,
        document_state.resource,
        document_state.request_referrer,
        document_state.request_referrer_policy,
        document_state.initiator_origin,
        document_state.origin,
        document_state.history_policy_container,
        document_state.about_base_url,
        document_state.navigable_target_name,
        document_state.reload_pending,
        document_state.ever_populated,
        source_snapshot_params,
        request.target_snapshot_params,
        request.user_involvement,
        request.navigation_id,
        move(navigation_params),
        request.csp_navigation_type,
        true,
        received_navigation_params);
}

WebIDL::ExceptionOr<void> LocalNavigable::navigate(NavigateParams params)
{
    // AD-HOC: Not in the spec but subsequent steps will fail if the navigable doesn't have an active window.
    if (!active_window())
        return {};

    auto source_document = params.source_document;
    auto exceptions_enabled = params.exceptions_enabled;
    auto user_involvement = params.user_involvement;
    auto url = params.url;

    auto& active_document = *this->active_document();
    auto& realm = HTML::relevant_realm(active_document);

    // 1. Let cspNavigationType be "form-submission" if formDataEntryList is non-null; otherwise "other".
    auto csp_navigation_type = params.form_data_entry_list.has_value() ? ContentSecurityPolicy::Directives::Directive::NavigationType::FormSubmission : ContentSecurityPolicy::Directives::Directive::NavigationType::Other;

    // 2. Let sourceSnapshotParams be the result of snapshotting source snapshot params given sourceDocument.
    auto source_snapshot_params = snapshot_source_snapshot_params(source_document);

    // 3. Let initiatorOriginSnapshot be a new opaque origin.
    auto initiator_origin_snapshot = URL::Origin::create_opaque();

    // 4. Let initiatorBaseURLSnapshot be about:blank.
    auto initiator_base_url_snapshot = URL::about_blank();

    // 5. If sourceDocument is null:
    if (!source_document) {
        // 1. Assert: userInvolvement is "browser UI".
        VERIFY(user_involvement == UserNavigationInvolvement::BrowserUI);

        // 2. If url's scheme is "javascript", then set initiatorOriginSnapshot to navigable's active document's origin.
        if (url.scheme() == "javascript"sv)
            initiator_origin_snapshot = active_document.origin();
    }
    // 6. Otherwise:
    else {
        // 1. Assert: userInvolvement is not "browser UI".
        VERIFY(user_involvement != UserNavigationInvolvement::BrowserUI);

        // 2. If sourceDocument's node navigable is not allowed by sandboxing to navigate navigable given sourceSnapshotParams:
        if (!source_document->navigable()->allowed_by_sandboxing_to_navigate(*this, source_snapshot_params)) {
            // 1. If exceptionsEnabled is true, then throw a "SecurityError" DOMException.
            if (exceptions_enabled)
                return WebIDL::SecurityError::create(realm, "Source document's node navigable is not allowed to navigate"_utf16);

            // 2 Return.
            return {};
        }

        // 3. Set initiatorOriginSnapshot to sourceDocument's origin.
        initiator_origin_snapshot = source_document->origin();

        // 4. Set initiatorBaseURLSnapshot to sourceDocument's document base URL.
        initiator_base_url_snapshot = source_document->base_url();
    }

    PreparedNavigation navigation {
        .params = move(params),
        .csp_navigation_type = csp_navigation_type,
        .source_snapshot_params = source_snapshot_params,
        .initiator_origin_snapshot = move(initiator_origin_snapshot),
        .initiator_base_url_snapshot = move(initiator_base_url_snapshot),
    };

    // AD-HOC: A child navigable's session history entry exists canonically only once the UI process has admitted
    //         the creation operation, so navigations that arrive before that acknowledgment queue until it lands.
    //         Top-level traversables are marked ready at creation and never queue here. Keep the values snapshotted
    //         by steps 1-6 so the eventual continuation starts at step 7.
    if (!m_has_session_history_entry_and_ready_for_navigation) {
        queue_pending_navigation(move(navigation), PendingNavigationBehavior::Append);
        return {};
    }

    begin_navigation(move(navigation));
    return {};
}

// To navigate a navigable navigable to a URL url using an optional Document-or-null sourceDocument (default null),
// with an optional POST resource, string, or null documentResource (default null),
// an optional response-or-null response (default null), an optional boolean exceptionsEnabled (default false),
// an optional NavigationHistoryBehavior historyHandling (default "auto"),
// an optional serialized state-or-null navigationAPIState (default null),
// an optional entry list or null formDataEntryList (default null),
// an optional referrer policy referrerPolicy (default the empty string),
// an optional user navigation involvement userInvolvement (default "none"),
// an optional Element sourceElement (default null),
// and an optional boolean initialInsertion (default false):

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#navigate

void LocalNavigable::continue_navigation_after_population_dispatch(PreparedNavigation navigation, NavigationPopulationRequest population_request)
{
    auto& params = navigation.params;
    auto source_snapshot_params = navigation.source_snapshot_params;
    auto navigation_id = population_request.navigation_id;

    if (has_been_destroyed() || !active_window()) {
        set_delaying_load_events(false);
        return;
    }
    if (ongoing_navigation() != navigation_id) {
        set_delaying_load_events(false);
        return;
    }

    if (!is_top_level_traversable()) {
        if (auto parent = this->parent(); parent && has_compositor_context()) {
            auto& local_parent = as<LocalNavigable>(*parent);
            if (local_parent.has_compositor_context())
                compositor_context().set_parent_context(local_parent.compositor_context().id());
        }
    }

    // 7. Let navigationParams be null.
    NavigationParamsVariant navigation_params = LocalNavigable::NullOrError {};

    // 8. If response is non-null:
    if (params.response) {
        auto response_url = params.response->url();
        VERIFY(response_url.has_value());

        // 1. Let sourcePolicyContainer be a clone of the sourceDocument's policy container, if
        //    sourceDocument is not null; otherwise null.
        GC::Ptr<PolicyContainer> source_policy_container;
        if (params.source_document)
            source_policy_container = source_snapshot_params->source_policy_container;

        // 2. Let policyContainer be the result of determining navigation params policy container given
        //    response's URL, null, sourcePolicyContainer, navigable's container document's policy container,
        //    and null.
        GC::Ptr<PolicyContainer> parent_policy_container;
        if (auto container_document = this->container_document())
            parent_policy_container = container_document->policy_container();
        else if (*response_url == URL::about_srcdoc()) {
            // NOTE: Specification assumes that only navigables corresponding to iframes can be navigated to about:srcdoc.
            //       We also use srcdoc to implement load_html() for top level navigables so we need a policy container
            //       because the navigable might not have a container.
            parent_policy_container = heap().allocate<PolicyContainer>(heap());
        }
        auto policy_container = determine_navigation_params_policy_container(*response_url, heap(), {}, source_policy_container, parent_policy_container, {});

        // 3. Let finalSandboxFlags be the union of targetSnapshotParams's sandboxing flags and
        //    policyContainer's CSP list's CSP-derived sandboxing flags.
        auto final_sandbox_flags = population_request.target_snapshot_params.sandboxing_flags | policy_container->csp_list->csp_derived_sandboxing_flags();

        // 4. Let responseOrigin be the result of determining the origin given response's URL,
        //    finalSandboxFlags, and documentState's initiator origin.
        auto response_origin = determine_the_origin(response_url, final_sandbox_flags, population_request.history_entry.document_state.initiator_origin);

        // 5. Let coop be a new opener policy.
        OpenerPolicy response_coop = {};

        // 6. Let coopEnforcementResult be a new opener policy enforcement result with
        //    url: response's URL
        //    origin: responseOrigin
        //    opener policy: coop
        OpenerPolicyEnforcementResult coop_enforcement_result {
            .url = *response_url,
            .origin = response_origin,
            .opener_policy = response_coop,
        };

        // 7. Set navigationParams to a new navigation params, with
        //    id: navigationId
        //    navigable: navigable
        //    request: null
        //    response: response
        //    fetch controller: null
        //    commit early hints: null
        //    COOP enforcement result: coopEnforcementResult
        //    reserved environment: null
        //    origin: responseOrigin
        //    policy container: policyContainer
        //    final sandboxing flag set: finalSandboxFlags
        //    iframe element referrer policy: targetSnapshotParams's iframe element referrer policy
        //    opener policy: coop
        //    FIXME: navigation timing type: "navigate"
        //    about base URL: documentState's about base URL
        //    user involvement: userInvolvement
        navigation_params = heap().allocate<NavigationParams>(
            navigation_id,
            this,
            nullptr,
            params.response,
            nullptr,
            nullptr,
            move(coop_enforcement_result),
            nullptr,
            move(response_origin),
            policy_container,
            final_sandbox_flags,
            population_request.target_snapshot_params.iframe_element_referrer_policy,
            response_coop,
            population_request.history_entry.document_state.about_base_url,
            params.user_involvement);
    }

    // 9. Attempt to populate the history entry's document for historyEntry, given navigable, "navigate",
    //    sourceSnapshotParams, targetSnapshotParams, userInvolvement, navigationId, navigationParams,
    //    cspNavigationType, with allowPOST set to true and completionSteps set to the following step:
    create_navigation_params_for_navigation(move(population_request), source_snapshot_params, move(navigation_params));
}

// Continue the navigate algorithm at step 7 with the values snapshotted by steps 1-6 in navigate().
void LocalNavigable::begin_navigation(PreparedNavigation navigation)
{
    // AD-HOC: Not in the spec but we should not navigate a navigable that has been destroyed.
    //         This can happen when a session history traversal step for creating a child navigable
    //         runs after the navigable has been destroyed (e.g. an iframe is removed before its
    //         post-connection steps finish processing). Without this check, we would call
    //         set_delaying_load_events(true) below, creating a DocumentLoadEventDelayer on the
    //         parent document that is never cleared.
    if (has_been_destroyed())
        return;

    // AD-HOC: Not in the spec but subsequent steps will fail if the navigable doesn't have an active window.
    if (!active_window())
        return;

    auto& params = navigation.params;
    auto url = params.url;
    auto source_document = params.source_document;
    auto document_resource = params.document_resource;
    auto response = params.response;
    auto history_handling = params.history_handling;
    auto navigation_api_state = params.navigation_api_state;
    auto referrer_policy = params.referrer_policy;
    auto user_involvement = params.user_involvement;
    auto source_element = params.source_element;
    auto initial_insertion = params.initial_insertion;
    auto& active_document = *this->active_document();
    auto& vm = this->vm();
    auto csp_navigation_type = navigation.csp_navigation_type;
    auto source_snapshot_params = navigation.source_snapshot_params;
    auto initiator_origin_snapshot = navigation.initiator_origin_snapshot;
    auto initiator_base_url_snapshot = navigation.initiator_base_url_snapshot;

    // 7. Let navigationId be the result of generating a random UUID.
    // NB: Generating the ID is the responsibility of whichever process requested the navigation. A load
    //     requested by the UI process carries the ID the UI generated when it recorded the navigation.
    auto navigation_id = params.navigation_id.value_or_lazy_evaluated([] {
        auto uuid = Crypto::generate_random_uuid();
        return Utf16String::from_ascii_without_validation(uuid.bytes());
    });

    // 8. If the surrounding agent is equal to navigable's active document's relevant agent, then continue these steps.
    //    Otherwise, queue a global task on the navigation and traversal task source given navigable's active window to continue these steps.
    // NB: A navigation requested by the UI process was queued by the IPC message that delivered it here,
    //     while a navigation begun within this process continues in its own agent.

    // 9. If navigable's active document's unload counter is greater than 0,
    //    then invoke WebDriver BiDi navigation failed with navigable and a WebDriver BiDi navigation status whose id
    //    is navigationId, status is "canceled", and url is url, and return.
    if (active_document.unload_counter() > 0) {
        // FIXME: invoke WebDriver BiDi navigation failed with navigable and a WebDriver BiDi navigation status whose id
        //        is navigationId, status is "canceled", and url is url
        return;
    }

    // 10. Let container be navigable's container.
    auto& container = m_container;

    // 11. If container is an iframe element and will lazy load element steps given container returns true,
    //     then stop intersection-observing a lazy loading element container and set container's lazy load resumption steps to null.
    if (container && container->is_html_iframe_element()) {
        auto& iframe_element = static_cast<HTMLIFrameElement&>(*container);
        if (iframe_element.will_lazy_load_element()) {
            iframe_element.document().stop_intersection_observing_a_lazy_loading_element(iframe_element);
            iframe_element.set_lazy_load_resumption_steps(nullptr);
        }
    }

    // 12. If historyHandling is "auto", then:
    if (history_handling == Bindings::NavigationHistoryBehavior::Auto) {
        // 1. If url equals navigable's active document's URL, and initiatorOriginSnapshot is same origin with
        //    navigable's active document's origin, then set historyHandling to "replace".
        // AD-HOC: Also replace same-URL navigations when sourceDocument is null.
        //         See https://github.com/whatwg/html/issues/12803.
        if (url == active_document.url()
            && (!source_document || initiator_origin_snapshot.is_same_origin(active_document.origin()))) {
            history_handling = Bindings::NavigationHistoryBehavior::Replace;
        }

        // 2. Otherwise, set historyHandling to "push".
        else {
            history_handling = Bindings::NavigationHistoryBehavior::Push;
        }
    }

    // 13. If the navigation must be a replace given url and navigable's active document, then set historyHandling to
    //     "replace".
    if (navigation_must_be_a_replace(url, active_document))
        history_handling = Bindings::NavigationHistoryBehavior::Replace;

    // 14. If all of the following are true:
    //     - documentResource is null;
    //     - response is null;
    //     - url equals navigable's active session history entry's URL with exclude fragments set to true; and
    //     - url's fragment is non-null,
    //     then:
    if (document_resource.has<Empty>()
        && !response
        && url.equals(active_session_history_entry()->url(), URL::ExcludeFragment::Yes)
        && url.fragment().has_value()) {
        // 1. Navigate to a fragment given navigable, url, historyHandling, userInvolvement, sourceElement, navigationAPIState, and navigationId.
        navigate_to_a_fragment(url, to_history_handling_behavior(history_handling), user_involvement, source_element, navigation_api_state, navigation_id);

        // 2. Return.
        return;
    }

    // 15. If navigable's parent is non-null, then set navigable's is delaying load events to true.
    if (parent() != nullptr)
        set_delaying_load_events(true);

    // 16. Let targetSnapshotParams be the result of snapshotting target snapshot params given navigable.
    auto target_snapshot_params = snapshot_target_snapshot_params(*this);

    // FIXME: 17. Invoke WebDriver BiDi navigation started with navigable and a new WebDriver BiDi navigation status whose id
    //     is navigationId, status is "pending", and url is url.

    // 18. If navigable's ongoing navigation is "traversal", then:
    if (ongoing_navigation().has<Traversal>()) {
        // FIXME: 1. Invoke WebDriver BiDi navigation failed with navigable and a new WebDriver BiDi navigation status whose id
        //    is navigationId, status is "canceled", and url is url.

        // AD-HOC: The HTML Standard cancels a navigation that starts while a traversal is ongoing. We defer it
        //         instead so UI-initiated navigations that race the tail end of a previous load are not dropped.
        //         Match Chromium, WebKit, and Gecko's observable behavior by letting the newest navigation win.
        //         See https://github.com/whatwg/html/issues/12581.
        queue_pending_navigation(move(navigation), PendingNavigationBehavior::Replace);

        // 2. Return.
        return;
    }

    // 19. Set the ongoing navigation for navigable to navigationId.
    set_ongoing_navigation(navigation_id);

    // 20. If url's scheme is "javascript", then:
    if (url.scheme() == "javascript"sv) {
        if (is_top_level_traversable())
            active_browsing_context()->page().client().request_navigation_start(*this, active_document.url(), NavigationTarget::TopLevel, url, navigation_id, {});

        // 1. Queue a global task on the navigation and traversal task source given navigable's active window to navigate to a javascript: URL given navigable, url, historyHandling, sourceSnapshotParams, initiatorOriginSnapshot, userInvolvement, cspNavigationType, initialInsertion, and navigationId.
        VERIFY(active_window());
        queue_global_task(Task::Source::NavigationAndTraversal, HTML::relevant_global_object(*active_window()), GC::create_function(heap(), [this, url, history_handling, source_snapshot_params, initiator_origin_snapshot, user_involvement, csp_navigation_type, initial_insertion, navigation_id] {
            navigate_to_a_javascript_url(url, to_history_handling_behavior(history_handling), source_snapshot_params, initiator_origin_snapshot, user_involvement, csp_navigation_type, initial_insertion, navigation_id);
        }));

        // 2. Return.
        return;
    }

    // 21. If all of the following are true:
    //     - userInvolvement is not "browser UI";
    //     - navigable's active document's origin is same origin-domain with sourceDocument's origin;
    //     - navigable's active document's is initial about:blank is false; and
    //     - url's scheme is a fetch scheme
    //     then:
    if (source_document && active_document.origin().is_same_origin_domain(source_document->origin()) && !active_document.is_initial_about_blank() && Fetch::Infrastructure::is_fetch_scheme(url.scheme())) {
        // 1. Let navigation be navigable's active window's navigation API.
        VERIFY(active_window());
        auto navigation = active_window()->navigation();

        // 2. Let entryListForFiring be formDataEntryList if documentResource is a POST resource; otherwise, null.
        auto entry_list_for_firing = [&]() -> Optional<GC::ConservativeVector<XHR::FormDataEntry>> {
            if (document_resource.has<POSTResource>())
                return GC::ConservativeVector { params.form_data_entry_list.value() };
            return {};
        }();

        // 3. Let navigationAPIStateForFiring be navigationAPIState if navigationAPIState is not null;
        //    otherwise, StructuredSerializeForStorage(undefined).
        auto navigation_api_state_for_firing = navigation_api_state.value_or(MUST(structured_serialize_for_storage(vm, JS::js_undefined())));

        // 4. Let continue be the result of firing a push/replace/reload navigate event at navigation
        //    with navigationType set to historyHandling, isSameDocument set to false, userInvolvement set to userInvolvement,
        //    sourceElement set to sourceElement, formDataEntryList set to entryListForFiring, destinationURL set to url,
        //    and navigationAPIState set to navigationAPIStateForFiring.
        auto navigation_type = [](Bindings::NavigationHistoryBehavior history_handling) {
            switch (history_handling) {
            case Bindings::NavigationHistoryBehavior::Push:
                return Bindings::NavigationType::Push;
            case Bindings::NavigationHistoryBehavior::Replace:
                return Bindings::NavigationType::Replace;
            case Bindings::NavigationHistoryBehavior::Auto:
            default:
                VERIFY_NOT_REACHED();
            }
        }(history_handling);
        auto continue_ = navigation->fire_a_push_replace_reload_navigate_event(navigation_type, url, false, user_involvement, source_element, entry_list_for_firing, navigation_api_state_for_firing);

        // 5. If continue is false, then return.
        if (!continue_) {
            // AD-HOC: This navigation is over: the navigate event either canceled it, or intercepted it and already
            //         committed it as a same-document navigation. In the spec, an intercepted navigation's queued
            //         same-document finalize sets the navigable's ongoing navigation to null moments later; our
            //         synchronous same-document commit replaces that queued step but deliberately preserves foreign
            //         navigation IDs, so clear our own ID here. Leaving it stamped makes later same-document
            //         traversals treat themselves as superseded and lets WebDriver wait forever for this navigation
            //         to finish. Preserve the Navigation API state: an intercepted navigate event stays ongoing
            //         until its handlers settle.
            if (ongoing_navigation() == navigation_id)
                set_ongoing_navigation(Empty {}, NavigationAPIAbortBehavior::Preserve);
            return;
        }
    }

    // FIXME: 22. If sourceDocument is navigable's container document, then reserve deferred fetch quota for navigable's container given url's origin.

    // 23. In parallel, run these steps:
    auto source_snapshot = create_navigation_source_snapshot(source_snapshot_params);

    // Session history entry state is structured-serialized in WebContent, where the JavaScript VM lives. The UI
    // process owns the pending entry and document state created from these initial values.
    auto entry_defaults = SessionHistoryEntry::create();
    auto start_request = NavigationStartRequest {
        .navigable_id = id(),
        .url = url,
        .document_resource = document_resource,
        .request_referrer = Fetch::Infrastructure::Request::Referrer::Client,
        .request_referrer_policy = referrer_policy,
        .initiator_origin = initiator_origin_snapshot,
        .initiator_base_url = initiator_base_url_snapshot,
        .navigable_target_name = target_name(),
        .source_snapshot_params = move(source_snapshot),
        .target_snapshot_params = target_snapshot_params,
        .csp_navigation_type = csp_navigation_type,
        .history_handling = history_handling,
        .user_involvement = user_involvement,
        .navigation_id = navigation_id,
        .classic_history_api_state = entry_defaults->classic_history_api_state(),
        .navigation_api_state = entry_defaults->navigation_api_state(),
        .navigation_api_key = entry_defaults->navigation_api_key(),
        .navigation_api_id = entry_defaults->navigation_api_id(),
    };
    auto continue_steps = GC::create_function(heap(), [this](Optional<PreparedNavigation> pending_navigation, Optional<NavigationPopulationRequest> population_request) {
        VERIFY(pending_navigation.has_value());
        VERIFY(population_request.has_value());
        continue_navigation_after_population_dispatch(pending_navigation.release_value(), population_request.release_value());
    });
    park_navigation_for_population(navigation_id, move(navigation), continue_steps);

    auto target = is_top_level_traversable() ? NavigationTarget::TopLevel : NavigationTarget::IFrame;
    active_browsing_context()->page().client().request_navigation_start(*this, active_document.url(), target, url, navigation_id, move(start_request));
    return;
}

void LocalNavigable::run_navigation_unload_check(Utf16String const& navigation_id, GC::Ref<GC::Function<void(bool)>> completion_steps)
{
    if (has_been_destroyed() || !active_window()) {
        completion_steps->function()(false);
        return;
    }

    auto pending_index = m_pending_navigations.find_first_index_if([&](auto const& pending) {
        return pending.population_navigation_id == navigation_id;
    });
    if (!pending_index.has_value()) {
        completion_steps->function()(false);
        return;
    }

    // 1. Let unloadPromptCanceled be the result of checking if unloading is user-canceled for navigable's active document's inclusive descendant navigables.
    traversable_navigable()->check_if_unloading_is_canceled(active_document()->inclusive_descendant_navigables(),
        GC::create_function(heap(), [this, navigation_id, completion_steps](LocalTraversableNavigable::CheckIfUnloadingIsCanceledResult unload_prompt_canceled) {
            if (has_been_destroyed() || !active_window()) {
                completion_steps->function()(false);
                return;
            }

            // 2. If unloadPromptCanceled is not "continue", or navigable's ongoing navigation is no longer navigationId:
            // NB: The UI process learns of the canceled check from the population-failure report and ends the
            //     recorded load itself.
            if (unload_prompt_canceled != LocalTraversableNavigable::CheckIfUnloadingIsCanceledResult::Continue) {
                set_delaying_load_events(false);
                completion_steps->function()(false);
                return;
            }

            if (ongoing_navigation() != navigation_id) {
                set_delaying_load_events(false);
                completion_steps->function()(false);
                return;
            }

            completion_steps->function()(true);
        }));
}

bool LocalNavigable::resume_navigation_params_creation(Utf16String const& navigation_id, Optional<NavigationPopulationRequest> request)
{
    auto pending = take_navigation_parked_for_population(navigation_id);
    if (!pending.has_value())
        return false;

    if (!request.has_value()) {
        set_delaying_load_events(false);
        return true;
    }

    VERIFY(pending->continue_steps);
    pending->continue_steps->function()(move(pending->navigation), move(request));
    return true;
}

void LocalNavigable::request_population_for_reconstructed_history_entry(NavigationPopulationRequest request)
{
    set_ongoing_navigation(request.navigation_id);
    auto source_snapshot_params = snapshot_source_snapshot_params(nullptr);
    auto navigation_id = request.navigation_id;
    auto continue_steps = GC::create_function(heap(), [this, source_snapshot_params](Optional<PreparedNavigation> pending_navigation, Optional<NavigationPopulationRequest> population_request) mutable {
        VERIFY(!pending_navigation.has_value());
        VERIFY(population_request.has_value());
        create_navigation_params_for_navigation(population_request.release_value(), source_snapshot_params, NullOrError {});
    });

    park_navigation_for_population(navigation_id, {}, continue_steps);
    page().client().request_navigation_population(*this, active_document()->url(), NavigationTarget::IFrame, move(request));
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#navigate-fragid
void LocalNavigable::navigate_to_a_fragment(URL::URL const& url, HistoryHandlingBehavior history_handling, UserNavigationInvolvement user_involvement, GC::Ptr<DOM::Element> source_element, Optional<StorageSerializationRecord> navigation_api_state, Utf16String navigation_id)
{
    // 1. Let navigation be navigable's active window's navigation API.
    VERIFY(active_window());
    auto navigation = active_window()->navigation();

    // 2. Let destinationNavigationAPIState be navigable's active session history entry's navigation API state.
    // 3. If navigationAPIState is not null, then set destinationNavigationAPIState to navigationAPIState.
    auto destination_navigation_api_state = navigation_api_state.has_value() ? *navigation_api_state : active_session_history_entry()->navigation_api_state();

    // 4. Let continue be the result of firing a push/replace/reload navigate event at navigation with navigationType
    //    set to historyHandling, isSameDocument set to true, userInvolvement set to userInvolvement, sourceElement set
    //    to sourceElement, destinationURL set to url, and navigationAPIState set to destinationNavigationAPIState.
    auto navigation_type = history_handling == HistoryHandlingBehavior::Push ? Bindings::NavigationType::Push : Bindings::NavigationType::Replace;
    bool const continue_ = navigation->fire_a_push_replace_reload_navigate_event(navigation_type, url, true, user_involvement, source_element, {}, destination_navigation_api_state);

    // 5. If continue is false, then return.
    if (!continue_)
        return;

    save_persisted_state_to_active_session_history_entry();
    auto active_entry = active_session_history_entry();
    auto previous_entry_persisted_state = create_session_history_entry_persisted_state(*active_entry);

    // 6. Let historyEntry be a new session history entry, with
    //      URL: url
    //      document state: navigable's active session history entry's document state
    //      navigation API state: destinationNavigationAPIState
    //      scroll restoration mode: navigable's active session history entry's scroll restoration mode
    auto history_entry = SessionHistoryEntry::create();
    history_entry->set_url(url);
    history_entry->set_document_state(active_entry->document_state());
    history_entry->set_navigation_api_state(destination_navigation_api_state);
    history_entry->set_scroll_restoration_mode(active_entry->scroll_restoration_mode());
    history_entry->set_scroll_position_data(active_entry->scroll_position_data());

    // 7. Let entryToReplace be navigable's active session history entry if historyHandling is "replace", otherwise null.
    auto entry_to_replace = history_handling == HistoryHandlingBehavior::Replace ? active_entry : nullptr;

    // 8. Let history be navigable's active document's history object.
    auto history = active_document()->history();

    // 9. Let scriptHistoryIndex be history's index.
    auto script_history_index = history->m_index;

    // 10. Let scriptHistoryLength be history's length.
    auto script_history_length = history->m_length;

    // 11. If historyHandling is "push", then:
    if (history_handling == HistoryHandlingBehavior::Push) {
        // 1. Set history's state to null.
        history->set_state(JS::js_null());

        // 2. Increment scriptHistoryIndex.
        script_history_index++;

        // 3. Set scriptHistoryLength to scriptHistoryIndex + 1.
        script_history_length = script_history_index + 1;
    }

    // 12. Set navigable's active session history entry to historyEntry.
    m_active_session_history_entry = history_entry;

    // 13. Update document for history step application given navigable's active document, historyEntry, true, scriptHistoryIndex, and scriptHistoryLength.
    // AD HOC: Skip updating the navigation api entries twice here
    active_document()->update_for_history_step_application(*history_entry, true, script_history_length, script_history_index, navigation_type, {}, {}, false);

    // 14. Update the navigation API entries for a same-document navigation given navigation, historyEntry, and historyHandling.
    navigation->update_the_navigation_api_entries_for_a_same_document_navigation(history_entry, navigation_type);

    // 15. Scroll to the fragment given navigable's active document.
    // FIXME: Specification doesn't say when document url needs to update during fragment navigation
    active_document()->set_url(url);
    active_document()->scroll_to_the_fragment();

    // 16. Let traversable be navigable's traversable navigable.
    auto traversable = traversable_navigable();

    // 17. Append the following session history synchronous navigation steps involving navigable to traversable:
    // 1. Finalize a same-document navigation given traversable, navigable, historyEntry, entryToReplace,
    //    historyHandling, and userInvolvement.
    traversable->finalize_same_document_navigation(*this, history_entry, entry_to_replace, history_handling, user_involvement, move(previous_entry_persisted_state));

    // FIXME: Invoke WebDriver BiDi fragment navigated with navigable and a new WebDriver BiDi navigation status whose
    //        id is navigationId, url is url, and status is "complete".
    (void)navigation_id;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#evaluate-a-javascript:-url
GC::Ptr<DOM::Document> LocalNavigable::evaluate_javascript_url(URL::URL const& url, URL::Origin const& new_document_origin, UserNavigationInvolvement user_involvement, Utf16String navigation_id)
{
    auto& vm = this->vm();
    VERIFY(active_window());
    auto& realm = active_window()->principal_realm();

    // 1. Let urlString be the result of running the URL serializer on url.
    auto url_string = url.serialize();

    // 2. Let encodedScriptSource be the result of removing the leading "javascript:" from urlString.
    auto encoded_script_source = url_string.bytes_as_string_view().substring_view(11);

    // 3. Let scriptSource be the UTF-8 decoding of the percent-decoding of encodedScriptSource.
    auto percent_decoded_script_source = URL::percent_decode(encoded_script_source);
    auto script_source = Utf16String::from_utf8(percent_decoded_script_source.view());

    // 4. Let settings be targetNavigable's active document's relevant settings object.
    auto& settings = active_document()->relevant_settings_object();

    // 5. Let baseURL be settings's API base URL.
    auto base_url = settings.api_base_url();

    // 6. Let script be the result of creating a classic script given scriptSource, settings, baseURL, and the default classic script fetch options.
    auto script = HTML::ClassicScript::create("(javascript url)", script_source, settings, base_url);

    // 7. Let evaluationStatus be the result of running the classic script script.
    auto evaluation_status = script->run();

    // 8. Let result be null.
    Optional<Utf16View> result;

    // 9. If evaluationStatus is a normal completion, and evaluationStatus.[[Value]] is a String, then set result to evaluationStatus.[[Value]].
    if (evaluation_status.type() == JS::Completion::Type::Normal && evaluation_status.value().is_string()) {
        result = evaluation_status.value().as_string().utf16_string_view();
    } else {
        // 10. Otherwise, return null.
        return nullptr;
    }

    // 11. Let response be a new response with
    //     URL: targetNavigable's active document's URL
    //     header list: «(`Content-Type`, `text/html;charset=utf-8`)»
    //     body: the UTF-8 encoding of result, as a body
    auto result_utf8 = MUST(result->to_utf8());
    auto response = Fetch::Infrastructure::Response::create(vm);
    response->url_list().append(active_document()->url());
    response->header_list()->append({ "Content-Type"sv, "text/html"sv });
    response->set_body(Fetch::Infrastructure::byte_sequence_as_body(realm, result_utf8.bytes()));

    // 12. Let policyContainer be targetNavigable's active document's policy container.
    auto const& policy_container = active_document()->policy_container();

    // 13. Let finalSandboxFlags be policyContainer's CSP list's CSP-derived sandboxing flags.
    auto final_sandbox_flags = policy_container->csp_list->csp_derived_sandboxing_flags();

    // 14. Let coop be targetNavigable's active document's opener policy.
    auto const& coop = active_document()->opener_policy();

    // 15. Let coopEnforcementResult be a new opener policy enforcement result with
    //     url: url
    //     origin: newDocumentOrigin
    //     opener policy: coop
    OpenerPolicyEnforcementResult coop_enforcement_result {
        .url = url,
        .origin = new_document_origin,
        .opener_policy = coop,
    };

    // AD-HOC: Get the target snapshot params. This is missing from the spec, see https://github.com/whatwg/html/issues/12563
    auto target_snapshot_params = snapshot_target_snapshot_params(*this);

    // 16. Let navigationParams be a new navigation params, with
    //     id: navigationId
    //     navigable: targetNavigable
    //     request: null
    //     response: response
    //     fetch controller: null
    //     commit early hints: null
    //     COOP enforcement result: coopEnforcementResult
    //     reserved environment: null
    //     origin: newDocumentOrigin
    //     policy container: policyContainer
    //     final sandboxing flag set: finalSandboxFlags
    //     iframe element referrer policy: targetSnapshotParams's iframe element referrer policy
    //     opener policy: coop
    //     FIXME: navigation timing type: "navigate"
    //     about base URL: targetNavigable's active document's about base URL
    //     user involvement: userInvolvement
    auto navigation_params = vm.heap().allocate<NavigationParams>(
        navigation_id,
        this,
        nullptr,
        response,
        nullptr,
        nullptr,
        move(coop_enforcement_result),
        nullptr,
        new_document_origin,
        policy_container,
        final_sandbox_flags,
        target_snapshot_params.iframe_element_referrer_policy,
        coop,
        active_document()->about_base_url(),
        user_involvement);

    // 17. Return the result of loading an HTML document given navigationParams.
    // NB: The response body is a known byte sequence, so we can pass it directly for sniffing.
    return load_document(navigation_params, result_utf8.bytes());
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#navigate-to-a-javascript:-url
void LocalNavigable::navigate_to_a_javascript_url(URL::URL const& url, HistoryHandlingBehavior history_handling, GC::Ref<SourceSnapshotParams> source_snapshot_params, URL::Origin const& initiator_origin, UserNavigationInvolvement user_involvement, ContentSecurityPolicy::Directives::Directive::NavigationType csp_navigation_type, InitialInsertion initial_insertion, Utf16String navigation_id)
{
    auto& vm = this->vm();

    // AD-HOC: These return paths do not run finalize_a_cross_document_navigation(). Clear a child navigable's
    //         load-event delay and tell the UI that the admitted navigation produced no document.
    auto finish_loading_without_navigation = [&] {
        set_delaying_load_events(false);
        if (is_top_level_traversable())
            active_browsing_context()->page().client().navigation_population_failed(id(), navigation_id);
    };

    // 1. Assert: historyHandling is "replace".
    VERIFY(history_handling == HistoryHandlingBehavior::Replace);

    // 2. If targetNavigable's ongoing navigation is no longer navigationId, then return.
    // AD-HOC: See https://github.com/whatwg/html/issues/12120, other browsers only cancel pending navigations for form submissions.
    // if (ongoing_navigation() != navigation_id)
    //     return;

    // 3. Set the ongoing navigation for targetNavigable to null.
    set_ongoing_navigation({});

    // 4. If initiatorOrigin is not same origin-domain with targetNavigable's active document's origin, then return.
    if (!initiator_origin.is_same_origin_domain(active_document()->origin())) {
        finish_loading_without_navigation();
        return;
    }

    // 5. Let request be a new request whose URL is url and whose policy container is sourceSnapshotParams's source policy container.
    auto request = Fetch::Infrastructure::Request::create(vm);
    request->set_url(url);
    request->set_policy_container(source_snapshot_params->source_policy_container);

    // AD-HOC: See https://github.com/whatwg/html/issues/4651, requires some investigation to figure out what we should be setting here.
    request->set_client(source_snapshot_params->fetch_client);

    // 6. If the result of should navigation request of type be blocked by Content Security Policy? given request and cspNavigationType is "Blocked", then return.
    if (ContentSecurityPolicy::should_navigation_request_of_type_be_blocked_by_content_security_policy(request, csp_navigation_type) == ContentSecurityPolicy::Directives::Directive::Result::Blocked) {
        finish_loading_without_navigation();
        return;
    }

    // 7. Let newDocument be the result of evaluating a javascript: URL given targetNavigable, url, initiatorOrigin, and userInvolvement.
    auto new_document = evaluate_javascript_url(url, initiator_origin, user_involvement, navigation_id);

    // 8. If newDocument is null:
    if (!new_document) {
        // 1. If initialInsertion is true and targetNavigable's active document's is initial about:blank is true,
        //    then run the iframe load event steps given targetNavigable's container.
        if (initial_insertion == InitialInsertion::Yes && active_document()->is_initial_about_blank()) {
            run_iframe_load_event_steps(as<HTMLIFrameElement>(*container()));
        }

        finish_loading_without_navigation();

        // 2. Return.
        // NOTE: In this case, some JavaScript code was executed, but no new Document was created, so we will not perform a navigation.
        return;
    }

    // 9. Assert: initiatorOrigin is newDocument's origin.
    VERIFY(initiator_origin == new_document->origin());

    // 10. Let entryToReplace be targetNavigable's active session history entry.
    auto entry_to_replace = active_session_history_entry();

    // 11. Let oldDocState be entryToReplace's document state.
    auto old_doc_state = entry_to_replace->document_state();

    // 12. Let documentState be a new document state with
    //     document: newDocument
    //     history policy container: a clone of the oldDocState's history policy container if it is non-null; null otherwise
    //     request referrer: oldDocState's request referrer
    //     request referrer policy: oldDocState's request referrer policy
    //     initiator origin: initiatorOrigin
    //     origin: initiatorOrigin
    //     about base URL: oldDocState's about base URL
    //     resource: null
    //     ever populated: true
    //     navigable target name: oldDocState's navigable target name
    auto document_state = DocumentState::create(page().client().allocate_cross_process_id());
    document_state->set_history_policy_container(old_doc_state->history_policy_container());
    document_state->set_request_referrer(old_doc_state->request_referrer());
    document_state->set_request_referrer_policy(old_doc_state->request_referrer_policy());
    document_state->set_initiator_origin(initiator_origin);
    document_state->set_origin(initiator_origin);
    document_state->set_about_base_url(old_doc_state->about_base_url());
    document_state->set_ever_populated(true);
    document_state->set_navigable_target_name(old_doc_state->navigable_target_name());
    document_state->set_document_id(new_document->unique_id());

    // 13. Let historyEntry be a new session history entry, with
    //     URL: entryToReplace's URL
    //     document state: documentState
    auto history_entry = SessionHistoryEntry::create();
    history_entry->set_url(entry_to_replace->url());
    history_entry->set_document_state(document_state);

    // 14. Append session history traversal steps to targetNavigable's traversable to finalize a cross-document navigation with targetNavigable, historyHandling, userInvolvement, and historyEntry.
    finalize_a_cross_document_navigation(*this, history_handling, user_involvement, history_entry, new_document, {}, GC::create_function(heap(), [](HistoryStepResult) { }));
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#reload
void LocalNavigable::reload(Optional<StorageSerializationRecord> navigation_api_state, UserNavigationInvolvement user_involvement)
{
    // 1. If userInvolvement is not "browser UI", then:
    if (user_involvement != UserNavigationInvolvement::BrowserUI) {
        // 1. Let navigation be navigable's active window's navigation API.
        auto active_window = this->active_window();
        VERIFY(active_window);
        auto navigation = active_window->navigation();

        // 2. Let destinationNavigationAPIState be navigable's active session history entry's navigation API state.
        auto destination_navigation_api_state = active_session_history_entry()->navigation_api_state();

        // 3. If navigationAPIState is not null, then set destinationNavigationAPIState to navigationAPIState.
        if (navigation_api_state.has_value())
            destination_navigation_api_state = *navigation_api_state;

        // 4. Let continue be the result of firing a push/replace/reload navigate event at navigation with
        //    navigationType set to "reload", isSameDocument set to false, userInvolvement set to userInvolvement,
        //    destinationURL set to navigable's active session history entry's URL, navigationAPIState set to
        //    destinationNavigationAPIState, and apiMethodTracker set to apiMethodTracker.
        auto continue_ = navigation->fire_a_push_replace_reload_navigate_event(Bindings::NavigationType::Reload, active_session_history_entry()->url(), false, user_involvement, nullptr, {}, destination_navigation_api_state);

        // 5. If continue is false, then return.
        if (!continue_)
            return;
    }

    // 1. If navigationAPIState is not null, then set navigable's active session history entry's navigation API state
    //    to navigationAPIState.
    if (navigation_api_state.has_value())
        active_session_history_entry()->set_navigation_api_state(navigation_api_state.release_value());

    // 2. Set navigable's active session history entry's document state's reload pending to true.
    active_session_history_entry()->document_state()->set_reload_pending(true);

    // 3. Let traversable be navigable's traversable navigable.
    auto traversable = traversable_navigable();

    traversable->page().client().page_did_set_session_history_entry_document_state_reload_pending(
        id(), active_session_history_entry()->navigation_api_key(), true);

    // 4. Append the following session history traversal steps to traversable:
    // 1. Apply the reload history step to traversable given userInvolvement.
    traversable->request_history_operation(ReloadHistoryOperationParameters {
        .navigable_id = id(),
        .user_involvement = user_involvement,
    });
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#the-navigation-must-be-a-replace
bool navigation_must_be_a_replace(URL::URL const& url, DOM::Document const& document)
{
    return url.scheme() == "javascript"sv || document.is_initial_about_blank();
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#allowed-to-navigate
bool LocalNavigable::allowed_by_sandboxing_to_navigate(LocalNavigable const& target, SourceSnapshotParams const& source_snapshot_params)
{
    auto& source = *this;

    auto is_ancestor_of = [](LocalNavigable const& a, LocalNavigable const& b) {
        for (auto parent = b.parent(); parent; parent = parent->parent()) {
            if (parent.ptr() == &a)
                return true;
        }
        return false;
    };

    // A navigable source is allowed by sandboxing to navigate a second navigable target,
    // given a source snapshot params sourceSnapshotParams, if the following steps return true:

    // 1. If source is target, then return true.
    if (&source == &target)
        return true;

    // 2. If source is an ancestor of target, then return true.
    if (is_ancestor_of(source, target))
        return true;

    // 3. If target is an ancestor of source, then:
    if (is_ancestor_of(target, source)) {

        // 1. If target is not a top-level traversable, then return true.
        if (!target.is_top_level_traversable())
            return true;

        // 2. If sourceSnapshotParams's has transient activation is true, and sourceSnapshotParams's sandboxing flags's
        //    sandboxed top-level navigation with user activation browsing context flag is set, then return false.
        if (source_snapshot_params.has_transient_activation && has_flag(source_snapshot_params.sandboxing_flags, SandboxingFlagSet::SandboxedTopLevelNavigationWithUserActivation))
            return false;

        // 3. If sourceSnapshotParams's has transient activation is false, and sourceSnapshotParams's sandboxing flags's
        //    sandboxed top-level navigation without user activation browsing context flag is set, then return false.
        if (!source_snapshot_params.has_transient_activation && has_flag(source_snapshot_params.sandboxing_flags, SandboxingFlagSet::SandboxedTopLevelNavigationWithoutUserActivation))
            return false;

        // 4. Return true.
        return true;
    }

    // 4. If target is a top-level traversable:
    if (target.is_top_level_traversable()) {
        // FIXME: 1. If source is the one permitted sandboxed navigator of target, then return true.

        // 2. If sourceSnapshotParams's sandboxing flags's sandboxed navigation browsing context flag is set, then return false.
        if (has_flag(source_snapshot_params.sandboxing_flags, SandboxingFlagSet::SandboxedNavigation))
            return false;

        // 3. Return true.
        return true;
    }

    // 5. If sourceSnapshotParams's sandboxing flags's sandboxed navigation browsing context flag is set, then return false.
    // 6. Return true.
    return !has_flag(source_snapshot_params.sandboxing_flags, SandboxingFlagSet::SandboxedNavigation);
}

static Optional<Web::CrossDocumentNavigationFinalizationHostState> prepare_to_finalize_a_cross_document_navigation(GC::Ref<LocalNavigable> navigable, GC::Ptr<DOM::Document> pending_document, Optional<Utf16String> const& expected_ongoing_navigation_id)
{
    // NOTE: This is not in the spec but we should not navigate destroyed navigable.
    if (navigable->has_been_destroyed()) {
        navigable->set_delaying_load_events(false);
        return {};
    }

    // AD-HOC: This check is not in the spec but we should not continue navigation if ongoing navigation id has changed.
    if (expected_ongoing_navigation_id.has_value() && navigable->ongoing_navigation() != *expected_ongoing_navigation_id) {
        navigable->set_delaying_load_events(false);
        return {};
    }

    // The UI process has reached this navigation's position on the session history traversal queue. Perform the
    // parts of finalization that need the live navigable and Document, then return the facts needed to continue the
    // algorithm there.
    //
    // AD-HOC: Without this guard, decrementing the navigable's delay counter triggers schedule_load_event_delay_check
    //         on the parent, which can see the about:blank (ready_for_post_load_tasks=true) before the session
    //         history traversal activates the new document. The guard is cleared when the new document becomes ready
    //         for post-load tasks (via set_ready_for_post_load_tasks).
    if (auto container_document = navigable->container_document(); container_document && pending_document)
        navigable->set_navigation_load_event_guard(*container_document);

    navigable->set_delaying_load_events(false);

    if (!pending_document) {
        // AD-HOC: Clear the ongoing navigation, like the "navigation must be a replace" and download cases do.
        //         No history step will be applied for this navigation, so nothing else clears it, and a stale
        //         ongoing navigation ID makes later same-document traversals consider themselves superseded.
        if (expected_ongoing_navigation_id.has_value() && navigable->ongoing_navigation() == expected_ongoing_navigation_id)
            navigable->set_ongoing_navigation({});

        return Web::CrossDocumentNavigationFinalizationHostState {};
    }

    return Web::CrossDocumentNavigationFinalizationHostState {
        .pending_document_is_in_auxiliary_browsing_context_with_opener = pending_document->browsing_context()->is_auxiliary() && pending_document->browsing_context()->opener_browsing_context() != nullptr,
        .pending_document_origin = pending_document->origin(),
        .active_document_origin = navigable->active_document()->origin(),
    };
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#finalize-a-cross-document-navigation
void finalize_a_cross_document_navigation(GC::Ref<LocalNavigable> navigable, HistoryHandlingBehavior history_handling, UserNavigationInvolvement user_involvement, NonnullRefPtr<SessionHistoryEntry> history_entry, GC::Ptr<DOM::Document> pending_document, Optional<Utf16String> expected_ongoing_navigation_id, GC::Ref<OnApplyHistoryStepComplete> on_complete)
{
    auto traversable = navigable->traversable_navigable();
    traversable->request_history_operation(
        FinalizeCrossDocumentNavigationHistoryOperationParameters {
            .navigable_id = navigable->id(),
            .history_entry = create_pending_session_history_entry_descriptor(*history_entry),
            .navigation_id = expected_ongoing_navigation_id,
            .history_handling = history_handling,
            .user_involvement = user_involvement,
        },
        {
            .pending_document = pending_document,
            .expected_ongoing_navigation_navigable = navigable,
            .expected_ongoing_navigation_id = expected_ongoing_navigation_id,
            .local_target_navigable_id = navigable->id(),
            .local_target_entry = history_entry,
            .pre_steps = GC::create_function(navigable->heap(), [navigable, pending_document, expected_ongoing_navigation_id](Optional<Web::ReconstructedChildNavigation>, GC::Ref<LocalTraversableNavigable::OnHistoryOperationReady> ready) mutable {
                auto host_state = prepare_to_finalize_a_cross_document_navigation(navigable, pending_document, expected_ongoing_navigation_id);
                if (!host_state.has_value()) {
                    ready->function()(HistoryStepResult::Applied);
                    return;
                }
                ready->function()(host_state.release_value());
            }),
            .on_complete = GC::create_function(navigable->heap(), [navigable, on_complete](HistoryStepResult result) {
                // AD-HOC: Trigger a relayout in the container document for size negotiation with SVG documents.
                if (auto container = navigable->container())
                    container->set_needs_layout_update(DOM::SetNeedsLayoutReason::FinalizeACrossDocumentNavigation);
                on_complete->function()(result);
            }),
        });
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#url-and-history-update-steps
void perform_url_and_history_update_steps(DOM::Document& document, URL::URL new_url, Optional<StorageSerializationRecord> serialized_data, HistoryHandlingBehavior history_handling)
{
    // 1. Let navigable be document's node navigable.
    auto navigable = document.navigable();

    // 2. Let activeEntry be navigable's active session history entry.
    auto active_entry = navigable->active_session_history_entry();
    navigable->save_persisted_state_to_active_session_history_entry();
    auto previous_entry_persisted_state = create_session_history_entry_persisted_state(*active_entry);

    // 3. Let newEntry be a new session history entry, with
    //      URL: newURL
    //      serialized state: if serializedData is not null, serializedData; otherwise activeEntry's classic history API state
    //      document state: activeEntry's document state
    //      scroll restoration mode: activeEntry's scroll restoration mode
    // FIXME: persisted user state: activeEntry's persisted user state
    auto new_entry = SessionHistoryEntry::create();
    new_entry->set_url(new_url);
    new_entry->set_classic_history_api_state(serialized_data.value_or(active_entry->classic_history_api_state()));
    new_entry->set_document_state(active_entry->document_state());
    new_entry->set_scroll_restoration_mode(active_entry->scroll_restoration_mode());
    new_entry->set_scroll_position_data(active_entry->scroll_position_data());

    // 4. If document's is initial about:blank is true, then set historyHandling to "replace".
    if (document.is_initial_about_blank()) {
        history_handling = HistoryHandlingBehavior::Replace;
    }

    // 5. Let entryToReplace be activeEntry if historyHandling is "replace", otherwise null.
    auto entry_to_replace = history_handling == HistoryHandlingBehavior::Replace ? active_entry : nullptr;

    // 6. If historyHandling is "push", then:
    if (history_handling == HistoryHandlingBehavior::Push) {
        // 1. Increment document's history object's index.
        document.history()->m_index++;

        // 2. Set document's history object's length to its index + 1.
        document.history()->m_length = document.history()->m_index + 1;
    }

    // If serializedData is not null, then restore the history object state given document and newEntry.
    if (serialized_data.has_value())
        document.restore_the_history_object_state(new_entry);

    // 8. Set the URL given document to newURL.
    document.set_url(new_url);

    // 9. Set document's latest entry to newEntry.
    document.set_latest_entry(new_entry);

    // 10. Set navigable's active session history entry to newEntry.
    navigable->set_active_session_history_entry(new_entry);

    // 11. Update the navigation API entries for a same-document navigation given document's relevant global object's navigation API, newEntry, and historyHandling.
    // In the wrapper architecture the relevant global object is a JS wrapper,
    // not the internal Window itself. Use the document's owning Window directly.
    VERIFY(document.window());
    auto& relevant_global_object = *document.window();
    auto navigation_type = history_handling == HistoryHandlingBehavior::Push ? Bindings::NavigationType::Push : Bindings::NavigationType::Replace;
    relevant_global_object.navigation()->update_the_navigation_api_entries_for_a_same_document_navigation(new_entry, navigation_type);

    // 12. Let traversable be navigable's traversable navigable.
    auto traversable = navigable->traversable_navigable();

    // 13. Append the following session history synchronous navigation steps involving navigable to traversable:
    // 1. Finalize a same-document navigation given traversable, navigable, newEntry, entryToReplace,
    //    historyHandling, and "none".
    traversable->finalize_same_document_navigation(*navigable, new_entry, entry_to_replace, history_handling, UserNavigationInvolvement::None, move(previous_entry_persisted_state));

    // FIXME: Invoke WebDriver BiDi history updated with navigable.
}

void LocalNavigable::scroll_offset_did_change()
{
    // https://w3c.github.io/csswg-drafts/cssom-view-1/#scrolling-events
    // Whenever a viewport gets scrolled (whether in response to user interaction or by an API), the user agent must
    // run these steps:

    // 1. Let doc be the viewport’s associated Document.
    auto doc = active_document();
    VERIFY(doc);

    // FIXME: 2. If doc is a snap container, run the steps to update scrollsnapchanging targets for doc with doc’s eventual
    //    snap target in the block axis as newBlockTarget and doc’s eventual snap target in the inline axis as
    //    newInlineTarget.

    // 3. If (doc, "scroll") is already in doc’s pending scroll events, abort these steps.
    // 4. Append (doc, "scroll") to doc’s pending scroll events.
    doc->append_pending_scroll_event({ *doc, EventNames::scroll });
}

CSSPixelRect LocalNavigable::to_top_level_rect(CSSPixelRect const& a_rect)
{
    auto rect = a_rect;
    rect.set_location(to_top_level_position(a_rect.location()));
    return rect;
}

CSSPixelPoint LocalNavigable::to_top_level_position(CSSPixelPoint a_position)
{
    auto position = a_position;
    for (GC::Ptr<LocalNavigable> ancestor = this; ancestor;) {
        if (is<LocalTraversableNavigable>(*ancestor))
            break;
        if (!ancestor->container())
            return {};
        auto const* layout_node = ancestor->container()->layout_node();
        if (!layout_node || !Painting::has_committed_box(*layout_node))
            return {};

        auto point = Painting::absolute_position(*layout_node);
        point.translate_by(position);
        position = Painting::transform_rect_to_viewport(*layout_node, { point, { 0, 0 } }).location();

        auto parent = ancestor->parent();
        ancestor = parent ? &as<LocalNavigable>(*parent) : nullptr;
    }
    return position;
}

void LocalNavigable::set_viewport_size(CSSPixelSize size, InvalidateDisplayList invalidate_display_list)
{
    if (m_viewport_size == size && invalidate_display_list == InvalidateDisplayList::No)
        return;

    m_viewport_size = size;

    if (has_compositor_context()) {
        compositor_context().viewport_size_updated(
            page().css_to_device_rect(viewport_rect()).size().to_type<int>(),
            Compositor::WindowResizingInProgress::Yes);
        m_pending_set_browser_zoom_request = false;
    }

    if (auto document = active_document()) {
        if (invalidate_display_list == InvalidateDisplayList::Yes)
            document->record_style_environment_change();
        else
            document->invalidate_style_for_viewport_change();
        document->set_needs_media_query_evaluation();
        document->set_needs_layout_update(DOM::SetNeedsLayoutReason::NavigableSetViewportSize);
    }

    if (auto document = active_document()) {
        document->set_needs_repaint(Badge<HTML::LocalNavigable> {}, invalidate_display_list);

        document->inform_all_viewport_clients_about_the_current_viewport_rect();

        // Schedule the HTML event loop to ensure that a `resize` event gets fired.
        HTML::main_thread_event_loop().schedule();
    }
}

void LocalNavigable::clamp_viewport_scroll_offset()
{
    auto document = active_document();
    if (!document || !document->layout_is_up_to_date())
        return;
    auto* layout_node = document->layout_node();
    if (!layout_node)
        return;
    if (!Painting::scrollable_overflow_rect(*layout_node).has_value())
        return;
    auto minimum_scroll_offset = Painting::minimum_scroll_offset(*layout_node);
    auto maximum_scroll_offset = Painting::maximum_scroll_offset(*layout_node);
    CSSPixelPoint clamped = {
        clamp(m_viewport_scroll_offset.x(), minimum_scroll_offset.x(), maximum_scroll_offset.x()),
        clamp(m_viewport_scroll_offset.y(), minimum_scroll_offset.y(), maximum_scroll_offset.y()),
    };
    if (clamped != m_viewport_scroll_offset)
        perform_scroll_of_viewport_scrolling_box(clamped);
}

void LocalNavigable::perform_scroll_of_viewport_scrolling_box(CSSPixelPoint new_position)
{
    // NB: This method is ad-hoc, but is currently called where "perform a scroll of a scrolling box" would be,
    //     where the box is the viewport.
    //     https://drafts.csswg.org/cssom-view/#perform-a-scroll
    if (m_viewport_scroll_offset != new_position) {
        m_viewport_scroll_offset = new_position;
        scroll_offset_did_change();

        if (auto document = active_document()) {
            document->set_needs_repaint(Badge<HTML::LocalNavigable> {}, InvalidateDisplayList::No);
            document->set_needs_to_refresh_scroll_state(true);
            document->inform_all_viewport_clients_about_the_current_viewport_rect();
        }
    }

    // Schedule the HTML event loop to ensure that a `resize` event gets fired.
    HTML::main_thread_event_loop().schedule();
}

static CSSPixelPoint async_scroll_offset_to_css_pixels(Gfx::FloatPoint async_scroll_offset, double device_pixels_per_css_pixel)
{
    return {
        CSSPixels { async_scroll_offset.x() / device_pixels_per_css_pixel },
        CSSPixels { async_scroll_offset.y() / device_pixels_per_css_pixel },
    };
}

static Optional<CSS::PseudoElement> pseudo_element_from_async_scroll_node_stable_id(Compositor::AsyncScrollNodeStableID const& stable_id)
{
    if (stable_id.kind != Compositor::AsyncScrollNodeKind::PseudoElement)
        return {};
    if (stable_id.pseudo_element_type >= to_underlying(CSS::PseudoElement::KnownPseudoElementCount))
        return {};
    return static_cast<CSS::PseudoElement>(stable_id.pseudo_element_type);
}

static DOM::Element* element_for_async_scroll_node_stable_id(DOM::Document& document, Compositor::AsyncScrollNodeStableID const& stable_id)
{
    auto* node = DOM::Node::from_unique_id(stable_id.node_id);
    auto* element = as_if<DOM::Element>(node);
    if (!element || &element->document() != &document)
        return nullptr;
    return element;
}

static GC::Ptr<DOM::Element> adopt_async_element_scroll_delta(DOM::Document& document, Compositor::AsyncScrollNodeStableID const& stable_id, CSSPixelPoint scroll_delta)
{
    auto* element = element_for_async_scroll_node_stable_id(document, stable_id);
    if (!element)
        return {};

    Optional<CSS::PseudoElement> pseudo_element;
    switch (stable_id.kind) {
    case Compositor::AsyncScrollNodeKind::Viewport:
        return {};
    case Compositor::AsyncScrollNodeKind::Element:
        break;
    case Compositor::AsyncScrollNodeKind::PseudoElement:
        pseudo_element = pseudo_element_from_async_scroll_node_stable_id(stable_id);
        if (!pseudo_element.has_value())
            return {};
        if (!element->get_pseudo_element(*pseudo_element).has_value())
            return {};
        break;
    }

    auto scroll_offset = element->scroll_offset(pseudo_element);
    scroll_offset.translate_by(scroll_delta);
    if (element->scroll_offset(pseudo_element) == scroll_offset)
        return {};

    element->set_scroll_offset(pseudo_element, scroll_offset);

    document.set_needs_to_refresh_scroll_state(true);
    document.append_pending_scroll_event({ *element, EventNames::scroll });
    element->set_needs_repaint(InvalidateDisplayList::No);
    return element;
}

static void queue_async_scroll_operation_promise_resolution(GC::Ref<WebIDL::Promise> promise)
{
    auto& realm = promise->promise()->shape().realm();
    HTML::queue_a_microtask(nullptr, GC::create_function(realm.heap(), [promise] {
        auto& realm = promise->promise()->shape().realm();
        HTML::TemporaryExecutionContext execution_context {
            realm,
            HTML::TemporaryExecutionContext::CallbacksEnabled::Yes
        };
        WebIDL::resolve_promise(promise);
    }));
}

void LocalNavigable::queue_scrollend_event_and_promise_resolution_for_finished_scroll(Optional<Compositor::AsyncScrollNodeStableID> stable_node_id, ScrollTrigger trigger, Optional<CSSPixelPoint> scroll_offset_before_scroll, ScrollPromises const& promises)
{
    if (stable_node_id.has_value() && scroll_offset_before_scroll.has_value()) {
        auto final_scroll_offset = scroll_offset_for(*stable_node_id);
        if (final_scroll_offset.has_value() && *final_scroll_offset != *scroll_offset_before_scroll)
            queue_scrollend_event_for_finished_scroll(*stable_node_id, trigger, scroll_offset_before_scroll);
    }
    for (auto const& promise : promises)
        queue_async_scroll_operation_promise_resolution(promise);
}

LocalNavigable::ScrollPromises* LocalNavigable::promises_of_smooth_scroll_in_flight_toward(Compositor::AsyncScrollNodeStableID stable_node_id, CSSPixelPoint position, ScrollTrigger trigger)
{
    for (auto& pending : m_pending_async_scroll_operations) {
        if (pending.stable_node_id == stable_node_id && pending.destination_scroll_offset == position && pending.trigger == trigger)
            return &pending.promises;
    }
    for (auto& smooth_scroll : m_main_thread_smooth_scrolls) {
        if (smooth_scroll.stable_node_id == stable_node_id && smooth_scroll.destination_scroll_offset == position && smooth_scroll.trigger == trigger)
            return &smooth_scroll.promises;
    }
    return nullptr;
}

void LocalNavigable::wait_for_async_scroll_operation(Compositor::AsyncScrollOperationID operation_id, GC::Ref<WebIDL::Promise> promise)
{
    if (has_been_destroyed() || !all_local_navigables().contains(*this)) {
        queue_async_scroll_operation_promise_resolution(promise);
        return;
    }

    m_pending_async_scroll_operations.append(PendingAsyncScrollOperation {
        .operation_id = operation_id,
        .promises = { promise },
        .stable_node_id = {},
        .initial_scroll_offset = {},
        .destination_scroll_offset = {},
    });
}

void LocalNavigable::resolve_async_scroll_operation(Compositor::AsyncScrollOperationID operation_id, AsyncScrollCompletion completion)
{
    // Notifying a scroll's completion can start the next scroll of the same scrolling box, so the finished scroll
    // leaves the list of scrolls in progress before it is reported.
    Optional<PendingAsyncScrollOperation> finished;
    m_pending_async_scroll_operations.remove_first_matching([&](auto const& pending) {
        if (pending.operation_id != operation_id)
            return false;
        finished = pending;
        return true;
    });
    if (!finished.has_value())
        return;

    // A scroll that user input took over belongs to the gesture that input continues, which reports the end of the
    // combined scrolling operation; reporting no offset to have scrolled from resolves such a scroll's promise
    // without queueing an event of its own.
    auto scroll_offset_the_finished_scroll_reports = completion == AsyncScrollCompletion::Finished
        ? finished->initial_scroll_offset
        : Optional<CSSPixelPoint> {};

    queue_scrollend_event_and_promise_resolution_for_finished_scroll(finished->stable_node_id, finished->trigger, scroll_offset_the_finished_scroll_reports, finished->promises);
    settle_user_scroll_gesture_if_input_deadline_passed();
}

void LocalNavigable::resolve_all_pending_async_scroll_operations()
{
    while (!m_pending_async_scroll_operations.is_empty()) {
        auto pending = m_pending_async_scroll_operations.take_last();
        queue_scrollend_event_and_promise_resolution_for_finished_scroll(pending.stable_node_id, pending.trigger, pending.initial_scroll_offset, pending.promises);
    }

    while (!m_main_thread_smooth_scrolls.is_empty()) {
        auto smooth_scroll = m_main_thread_smooth_scrolls.take_last();
        queue_scrollend_event_and_promise_resolution_for_finished_scroll(smooth_scroll.stable_node_id, smooth_scroll.trigger, smooth_scroll.initial_scroll_offset, smooth_scroll.promises);
    }

    settle_user_scroll_gesture_if_input_deadline_passed();
}

Optional<CSSPixelPoint> LocalNavigable::scroll_offset_for(Compositor::AsyncScrollNodeStableID stable_node_id) const
{
    auto document = active_document();
    if (!document)
        return {};

    if (stable_node_id.kind == Compositor::AsyncScrollNodeKind::Viewport) {
        if (stable_node_id.node_id != document->unique_id())
            return {};
        return m_viewport_scroll_offset;
    }

    auto* element = element_for_async_scroll_node_stable_id(*document, stable_node_id);
    if (!element)
        return {};
    return element->scroll_offset(pseudo_element_from_async_scroll_node_stable_id(stable_node_id));
}

static Layout::Node* layout_node_for_async_scroll_node(DOM::Document& document, Compositor::AsyncScrollNodeStableID stable_node_id)
{
    if (stable_node_id.kind == Compositor::AsyncScrollNodeKind::Viewport) {
        if (stable_node_id.node_id != document.unique_id())
            return nullptr;
        return document.unsafe_layout_node();
    }

    auto* element = element_for_async_scroll_node_stable_id(document, stable_node_id);
    if (!element)
        return nullptr;
    if (auto pseudo_element = pseudo_element_from_async_scroll_node_stable_id(stable_node_id); pseudo_element.has_value()) {
        auto synthetic_pseudo_element = element->get_synthetic_pseudo_element(*pseudo_element);
        if (!synthetic_pseudo_element.has_value())
            return nullptr;
        return synthetic_pseudo_element->layout_node();
    }
    return element->layout_node();
}

bool LocalNavigable::set_scroll_offset_for(Compositor::AsyncScrollNodeStableID stable_node_id, CSSPixelPoint scroll_offset)
{
    auto document = active_document();
    if (!document)
        return false;

    if (stable_node_id.kind == Compositor::AsyncScrollNodeKind::Viewport) {
        if (stable_node_id.node_id != document->unique_id())
            return false;
        auto old_scroll_offset = m_viewport_scroll_offset;
        perform_scroll_of_viewport_scrolling_box(scroll_offset);
        return old_scroll_offset != m_viewport_scroll_offset;
    }

    if (!element_for_async_scroll_node_stable_id(*document, stable_node_id))
        return false;
    document->update_layout(DOM::UpdateLayoutReason::ElementScroll);
    auto* layout_node = layout_node_for_async_scroll_node(*document, stable_node_id);
    if (!layout_node)
        return false;
    return Painting::set_scroll_offset(*layout_node, scroll_offset) == Painting::ScrollHandled::Yes;
}

static void record_snapped_areas_of_scroll_container(DOM::Document& document, Compositor::AsyncScrollNodeStableID stable_node_id, Painting::SnapDestination& snap_destination)
{
    // https://drafts.csswg.org/css-scroll-snap-1/#re-snap
    // If the scroll container was snapped before the content change and those same snap areas still exist (e.g. their
    // associated elements were not deleted), the scroll container must be re-snapped to those same snap areas after the
    // content change.

    // NB: An axis the selection did not evaluate keeps the snap areas it is already snapped to.
    Painting::SnappedAreas snapped_areas = document.snapped_areas_of_scroll_container(stable_node_id);
    if (snap_destination.evaluated_x)
        snapped_areas.x = move(snap_destination.snapped_areas.x);
    if (snap_destination.evaluated_y)
        snapped_areas.y = move(snap_destination.snapped_areas.y);
    document.set_snapped_areas_of_scroll_container(stable_node_id, move(snapped_areas));
}

static GC::Ptr<DOM::EventTarget> scroll_event_target_for_async_scroll_node(DOM::Document& document, Compositor::AsyncScrollNodeStableID stable_node_id)
{
    if (stable_node_id.kind == Compositor::AsyncScrollNodeKind::Viewport) {
        if (stable_node_id.node_id != document.unique_id())
            return {};
        return document;
    }
    return element_for_async_scroll_node_stable_id(document, stable_node_id);
}

LocalNavigable::PendingUserScrollendTarget* LocalNavigable::latched_user_scroll_gesture_for(GC::Ref<DOM::EventTarget> target, Optional<Compositor::AsyncScrollNodeStableID> const& stable_node_id)
{
    auto index = m_pending_user_scrollend_targets.find_first_index_if([&](auto const& entry) {
        if (stable_node_id.has_value() && entry.stable_node_id.has_value())
            return *entry.stable_node_id == *stable_node_id;
        return entry.target.ptr() == target.ptr();
    });
    if (!index.has_value())
        return nullptr;
    return &m_pending_user_scrollend_targets[*index];
}

void LocalNavigable::queue_scrollend_event(Compositor::AsyncScrollNodeStableID stable_node_id, ScrollTrigger trigger, Optional<CSSPixelPoint> scroll_offset_before_scroll)
{
    auto document = active_document();
    if (!document)
        return;

    auto target = scroll_event_target_for_async_scroll_node(*document, stable_node_id);
    if (!target)
        return;

    queue_scrollend_event(*document, *target, stable_node_id, trigger, scroll_offset_before_scroll);
}

void LocalNavigable::queue_scrollend_event(DOM::Document& document, GC::Ref<DOM::EventTarget> target, Optional<Compositor::AsyncScrollNodeStableID> stable_node_id, ScrollTrigger trigger, Optional<CSSPixelPoint> scroll_offset_before_scroll)
{
    if (trigger == ScrollTrigger::UserInput)
        queue_scrollend_event_after_user_scroll(target, stable_node_id, scroll_offset_before_scroll);
    else
        document.append_pending_scroll_event({ target, EventNames::scrollend });
}

void LocalNavigable::queue_scrollend_event_for_finished_scroll(Compositor::AsyncScrollNodeStableID stable_node_id, ScrollTrigger trigger, Optional<CSSPixelPoint> scroll_offset_before_scroll)
{
    auto document = active_document();
    if (!document)
        return;
    auto target = scroll_event_target_for_async_scroll_node(*document, stable_node_id);
    if (!target)
        return;

    if (trigger != ScrollTrigger::UserInput) {
        document->append_pending_scroll_event({ *target, EventNames::scrollend });
        return;
    }

    if (latched_user_scroll_gesture_for(*target, stable_node_id))
        return;

    if (m_user_scroll_gesture_hold_count > 0) {
        queue_scrollend_event_after_user_scroll(*target, stable_node_id, scroll_offset_before_scroll);
        return;
    }

    document->append_pending_scroll_event({ *target, EventNames::scrollend });
}

void LocalNavigable::queue_scrollend_event_after_user_scroll(GC::Ref<DOM::EventTarget> target, Optional<Compositor::AsyncScrollNodeStableID> stable_node_id, Optional<CSSPixelPoint> scroll_offset_before_scroll, SnapPositionSelection snap_position_selection)
{
    // AD-HOC: Wheel events carry no gesture phase information, so a scroll gesture is considered finished once no
    //         user scrolling has moved this navigable's scrolling boxes for 500 milliseconds.
    static constexpr int user_scroll_settle_delay_ms = 500;

    if (auto* existing_entry = latched_user_scroll_gesture_for(target, stable_node_id)) {
        if (!existing_entry->scroll_offset_at_gesture_start.has_value())
            existing_entry->scroll_offset_at_gesture_start = scroll_offset_before_scroll;
        existing_entry->intent = m_user_scroll_input_intent;
        existing_entry->travels_under_momentum = m_user_scroll_gesture_travels_under_momentum;
        existing_entry->snap_position_selection = snap_position_selection;
        existing_entry->awaits_layout_for_snapping = false;
    } else {
        m_pending_user_scrollend_targets.append({ target, stable_node_id, scroll_offset_before_scroll, {}, m_user_scroll_input_intent, m_user_scroll_gesture_travels_under_momentum, snap_position_selection });
    }

    if (!m_user_scroll_settle_timer) {
        m_user_scroll_settle_timer = Core::Timer::create_single_shot(user_scroll_settle_delay_ms, [this] {
            user_scroll_did_settle();
        });
    }
    m_user_scroll_settle_timer->restart();
}

void LocalNavigable::note_user_scroll_input_intent(Painting::SnapSelectionStrategy::Type intent)
{
    m_user_scroll_input_intent = intent;
}

void LocalNavigable::defer_user_scroll_settlement()
{
    // User input activity postpones settlement of already latched targets, but never latches new ones, so scrolling
    // boxes that do not move still receive no scrollend event.
    if (m_pending_user_scrollend_targets.is_empty())
        return;
    m_user_scroll_settle_timer->restart();
}

void LocalNavigable::note_user_scroll_gesture_phase(ScrollGesturePhase phase)
{
    switch (phase) {
    case ScrollGesturePhase::None:
        m_user_scroll_gesture_travels_under_momentum = false;
        reset_momentum_fling_state();
        m_wheel_user_scroll_gesture_hold = nullptr;
        break;
    case ScrollGesturePhase::Ongoing:
    case ScrollGesturePhase::Momentum: {
        bool travels_under_momentum = phase == ScrollGesturePhase::Momentum;

        if (!travels_under_momentum)
            reset_momentum_fling_state();

        if (m_wheel_user_scroll_gesture_hold && m_user_scroll_gesture_travels_under_momentum != travels_under_momentum)
            m_wheel_user_scroll_gesture_hold = nullptr;
        m_user_scroll_gesture_travels_under_momentum = travels_under_momentum;

        if (!m_wheel_user_scroll_gesture_hold)
            m_wheel_user_scroll_gesture_hold = make<UserScrollGestureHold>(*this);
        break;
    }
    case ScrollGesturePhase::Ended:
        m_user_scroll_gesture_travels_under_momentum = false;
        reset_momentum_fling_state();
        if (m_wheel_user_scroll_gesture_hold) {
            m_wheel_user_scroll_gesture_hold = nullptr;
            break;
        }
        settle_user_scroll_gesture();
        break;
    }
}

void LocalNavigable::reset_momentum_fling_state()
{
    m_momentum_snap_position_selection = MomentumSnapPositionSelection::NotSelectedYet;
    m_momentum_fling_estimator.reset();
}

void LocalNavigable::settle_user_scroll_gesture()
{
    if (m_pending_user_scrollend_targets.is_empty())
        return;

    if (m_user_scroll_gesture_hold_count > 0)
        return;

    m_user_scroll_settle_timer->stop();
    user_scroll_did_settle();
}

void LocalNavigable::snap_user_scroll_gestures_that_awaited_layout()
{
    if (!any_of(m_pending_user_scrollend_targets, [](auto const& entry) { return entry.awaits_layout_for_snapping; }))
        return;
    user_scroll_did_settle(UserScrollSettlement::SnappingDeferredUntilLayout);
}

// https://drafts.csswg.org/css-scroll-snap-1/#re-snap
void LocalNavigable::re_snap_scroll_containers_after_layout_change()
{
    // If the content or layout of the document changes (e.g. content is added, moved, deleted, resized) such that the
    // content of a snapport changes, the UA must re-evaluate the resulting scroll position, and re-snap if required.

    if (m_is_re_snapping_scroll_containers)
        return;

    auto document = active_document();
    if (!document || !document->needs_scroll_container_resnap())
        return;

    if (!document->may_have_scroll_snap_areas()) {
        document->cancel_scheduled_scroll_container_resnap();
        return;
    }

    // NB: Not every completed layout update leaves usable layout behind, such as one for a document created for
    //     template contents; re-snapping then waits for an update that does.
    if (!document->layout_is_up_to_date() || document->is_running_update_layout())
        return;

    auto const* viewport_layout_node = document->unsafe_layout_node();
    if (!viewport_layout_node || !Painting::has_committed_box(*viewport_layout_node))
        return;

    if (m_user_scroll_gesture_hold_count > 0)
        return;

    document->cancel_scheduled_scroll_container_resnap();
    TemporaryChange re_snapping_in_progress { m_is_re_snapping_scroll_containers, true };

    auto snap_containers = document->collect_scroll_snap_containers();

    bool any_snap_container_deferred = false;
    for (auto const& snap_container : snap_containers) {
        auto stable_node_id = Painting::async_scroll_node_stable_id(snap_container);
        if (!stable_node_id.has_value())
            continue;

        // A scrolling box that is being scrolled re-snaps once that scroll settles from wherever it comes to rest,
        // rather than having the position it is moving toward re-evaluated out from under it.
        auto target = scroll_event_target_for_async_scroll_node(*document, *stable_node_id);
        bool has_latched_gesture = target && latched_user_scroll_gesture_for(*target, stable_node_id);
        if (has_latched_gesture || in_flight_scroll_for(*stable_node_id).has_value()) {
            any_snap_container_deferred = true;
            continue;
        }

        auto current_scroll_offset = scroll_offset_for(*stable_node_id);
        if (!current_scroll_offset.has_value())
            continue;

        auto const& snapped_areas = document->snapped_areas_of_scroll_container(*stable_node_id);
        Painting::ResnapSelection resnap_selection {
            .snapped_areas = snapped_areas,
            .focused_node = document->focused_area(),
            .targeted_element = document->target_element(),
        };
        auto snap_destination = Painting::select_resnap_destination(*snap_container, *current_scroll_offset, resnap_selection);

        // Scrolling behavior for re-snapping to the same box as before however, is UA-defined. The UA may, for
        // example, when snapped to the start of a section, choose not to animate the scroll to the section's new
        // position as content is dynamically added earlier in the document in order to create the illusion of not
        // scrolling.
        // NB: Re-snapping to snap areas the container was already snapped to is therefore instant.
        auto is_subset_of = [](Vector<Painting::SnapAreaReference> const& areas, Vector<Painting::SnapAreaReference> const& other_areas) {
            return all_of(areas, [&](auto const& area) { return other_areas.contains_slow(area); });
        };
        bool re_snapped_to_same_areas = !snap_destination.snapped_areas.is_empty()
            && is_subset_of(snap_destination.snapped_areas.x, snapped_areas.x)
            && is_subset_of(snap_destination.snapped_areas.y, snapped_areas.y);

        document->set_snapped_areas_of_scroll_container(*stable_node_id, move(snap_destination.snapped_areas));

        if (snap_destination.position == *current_scroll_offset)
            continue;

        // Scrolling required by a re-snap operation to a new or different box must behave and animate the same way as
        // any other scroll-into-view operation, including honoring controls such as scroll-behavior.
        auto behavior = re_snapped_to_same_areas ? Bindings::ScrollBehavior::Instant : Bindings::ScrollBehavior::Auto;
        GC::Ptr<DOM::Element> associated_element = stable_node_id->kind == Compositor::AsyncScrollNodeKind::Viewport
            ? document->document_element()
            : element_for_async_scroll_node_stable_id(*document, *stable_node_id);

        TemporaryExecutionContext temporary_execution_context { HTML::relevant_realm(*document) };
        perform_a_scroll_of_a_scrolling_box(*stable_node_id, snap_destination.position, behavior, associated_element, ScrollTrigger::Programmatic, {}, DestinationSnapping::DestinationIsSnapPosition);
    }

    // A deferred snap container re-snaps once the scroll that owns it completes and settlement runs this again.
    if (any_snap_container_deferred)
        document->schedule_scroll_container_resnap();
}

void LocalNavigable::cancel_user_scroll_settlement()
{
    if (m_user_scroll_settle_timer)
        m_user_scroll_settle_timer->stop();
    m_pending_user_scrollend_targets.clear();
    m_compositor_user_scroll_gesture_hold = nullptr;
    m_wheel_user_scroll_gesture_hold = nullptr;
}

void LocalNavigable::begin_user_scroll_gesture_hold(Badge<UserScrollGestureHold>)
{
    ++m_user_scroll_gesture_hold_count;
}

void LocalNavigable::end_user_scroll_gesture_hold(Badge<UserScrollGestureHold>)
{
    VERIFY(m_user_scroll_gesture_hold_count > 0);
    if (--m_user_scroll_gesture_hold_count > 0)
        return;

    // The release of the last held input completes the scroll gesture.
    settle_user_scroll_gesture();
}

Optional<LocalNavigable::InFlightScroll> LocalNavigable::in_flight_scroll_for(Optional<Compositor::AsyncScrollNodeStableID> const& stable_node_id) const
{
    if (!stable_node_id.has_value())
        return {};

    Optional<InFlightScroll> in_flight_scroll;
    auto consider = [&](ScrollTrigger trigger, Optional<CSSPixelPoint> destination_scroll_offset) {
        if (in_flight_scroll.has_value() && in_flight_scroll->trigger == ScrollTrigger::UserInput)
            return;
        in_flight_scroll = InFlightScroll { trigger, destination_scroll_offset };
    };
    for (auto const& pending : m_pending_async_scroll_operations) {
        if (pending.stable_node_id == stable_node_id)
            consider(pending.trigger, pending.destination_scroll_offset);
    }
    for (auto const& smooth_scroll : m_main_thread_smooth_scrolls) {
        if (smooth_scroll.stable_node_id == *stable_node_id)
            consider(smooth_scroll.trigger, smooth_scroll.destination_scroll_offset);
    }
    return in_flight_scroll;
}

void LocalNavigable::abandon_snapping_of_user_scroll_gesture(Compositor::AsyncScrollNodeStableID stable_node_id)
{
    auto document = active_document();
    if (!document)
        return;
    auto target = scroll_event_target_for_async_scroll_node(*document, stable_node_id);
    if (!target)
        return;

    auto* entry = latched_user_scroll_gesture_for(*target, stable_node_id);
    if (!entry)
        return;

    // The entry remains only to deliver the scrollend event the gesture owes.
    entry->scroll_offset_at_gesture_start = {};
    entry->unsnapped_scroll_destination = {};
    entry->intent = Painting::SnapSelectionStrategy::Type::EndPosition;
    entry->snap_position_selection = SnapPositionSelection::AtGestureEnd;
}

void LocalNavigable::settle_user_scroll_gesture_if_input_deadline_passed()
{
    if (m_pending_user_scrollend_targets.is_empty())
        return;
    if (m_user_scroll_settle_timer && m_user_scroll_settle_timer->is_active())
        return;

    // Starting a scroll of a scrolling box aborts the scrolls already running for it, and settling in the middle of
    // that would enqueue a snap scroll of the same box alongside the one being started. The scroll that is starting
    // settles the gesture once it is under way instead.
    if (m_scrolls_being_started > 0) {
        m_user_scroll_settlement_awaits_scroll_start = true;
        return;
    }

    user_scroll_did_settle();
}

void LocalNavigable::user_scroll_did_settle(UserScrollSettlement settlement)
{
    if (has_been_destroyed())
        return;

    // A held input keeps the scroll gesture in progress; its release completes the settlement instead.
    if (m_user_scroll_gesture_hold_count > 0)
        return;

    auto targets = move(m_pending_user_scrollend_targets);
    auto document = active_document();
    if (!document)
        return;

    // Settlement can occur inside a layout update when tearing down a scrollbar whose thumb is grabbed, so snapping
    // geometry is only consulted when layout is already up to date.
    bool can_snap = document->layout_is_up_to_date() && !document->is_running_update_layout();

    bool queued_any_scrollend_event = false;
    for (auto& entry : targets) {
        // A settlement performed once layout is up to date is only for the gestures an earlier one left waiting for
        // it; the rest are still waiting for their own input to run out.
        if (settlement == UserScrollSettlement::SnappingDeferredUntilLayout && !entry.awaits_layout_for_snapping) {
            m_pending_user_scrollend_targets.append(move(entry));
            continue;
        }

        auto const& target = entry.target;
        if (auto* element = as_if<DOM::Element>(*target)) {
            if (&element->document() != document.ptr() || !element->is_connected())
                continue;
        } else if (target.ptr() != document.ptr() && target.ptr() != document->visual_viewport().ptr()) {
            continue;
        }

        auto const& stable_node_id = entry.stable_node_id;
        auto in_flight_scroll_trigger = in_flight_scroll_for(stable_node_id).map([](auto const& in_flight_scroll) { return in_flight_scroll.trigger; });

        // A user scroll that is still running has not reached the position the gesture ends at. The completion of that
        // scroll settles the gesture instead.
        if (in_flight_scroll_trigger == ScrollTrigger::UserInput) {
            m_pending_user_scrollend_targets.append(move(entry));
            continue;
        }

        // https://drafts.csswg.org/css-scroll-snap-1/#snap-strictness
        // If a valid snap position exists then the scroll container must snap at the termination of a scroll (if none
        // exist then no snapping occurs).
        // NB: A programmatic scroll of the scrolling box decides where it comes to rest, and snapped its own
        //     destination when it started, so the gesture only delivers the scrollend event it owes.
        bool snaps_at_this_settlement = in_flight_scroll_trigger != ScrollTrigger::Programmatic && stable_node_id.has_value();

        // A gesture whose snap position cannot be selected yet keeps the scrolling box latched and snaps once layout
        // is up to date. A settlement that already waited for layout once takes what it can get, so that a scrolling
        // box cannot stay latched.
        if (snaps_at_this_settlement && !can_snap && !entry.awaits_layout_for_snapping) {
            entry.awaits_layout_for_snapping = true;
            m_pending_user_scrollend_targets.append(move(entry));
            main_thread_event_loop().queue_task_to_update_the_rendering();
            continue;
        }

        if (snaps_at_this_settlement && can_snap) {
            auto const* snap_container = layout_node_for_async_scroll_node(*document, *stable_node_id);
            auto current_scroll_offset = scroll_offset_for(*stable_node_id);
            if (snap_container && current_scroll_offset.has_value()) {
                Painting::SnapSelectionStrategy strategy;
                if (entry.scroll_offset_at_gesture_start.has_value() && entry.snap_position_selection == SnapPositionSelection::AtGestureEnd) {
                    strategy.displacement = *current_scroll_offset - *entry.scroll_offset_at_gesture_start;
                    if (!strategy.displacement.is_zero()) {
                        strategy.type = entry.intent;
                        if (entry.intent != Painting::SnapSelectionStrategy::Type::EndPosition || entry.travels_under_momentum)
                            strategy.start_offset = *entry.scroll_offset_at_gesture_start;
                    }
                }
                auto snap_destination = Painting::adjust_scroll_destination_for_snapping(*snap_container, *current_scroll_offset, strategy);
                record_snapped_areas_of_scroll_container(*document, *stable_node_id, snap_destination);
                if (snap_destination.position != *current_scroll_offset) {
                    // The snap animation queues this target's scrollend event once it completes.
                    TemporaryExecutionContext temporary_execution_context { HTML::relevant_realm(*document) };
                    perform_a_scroll_of_a_scrolling_box(*stable_node_id, snap_destination.position, Bindings::ScrollBehavior::Smooth, nullptr, ScrollTrigger::UserInput);
                    continue;
                }
            }
        }

        if (document->append_pending_scroll_event({ target, EventNames::scrollend }))
            queued_any_scrollend_event = true;
    }

    if (queued_any_scrollend_event)
        main_thread_event_loop().queue_task_to_update_the_rendering();
}

void LocalNavigable::resolve_pending_smooth_scrolls(Compositor::AsyncScrollNodeStableID stable_node_id, SmoothScrollAbortCause abort_cause)
{
    Vector<PendingAsyncScrollOperation> finished_async_scroll_operations;
    m_pending_async_scroll_operations.remove_all_matching([&](auto const& pending) {
        if (pending.stable_node_id != stable_node_id)
            return false;
        finished_async_scroll_operations.append(pending);
        return true;
    });

    Vector<MainThreadSmoothScroll> finished_smooth_scrolls;
    m_main_thread_smooth_scrolls.remove_all_matching([&](auto const& smooth_scroll) {
        if (smooth_scroll.stable_node_id != stable_node_id)
            return false;
        finished_smooth_scrolls.append(smooth_scroll);
        return true;
    });

    // A smooth scroll aborted by a new scroll of the same scrolling box hands the reporting of the scrolling
    // operation's end to its replacement; reporting no offset to have scrolled from resolves such a scroll's promise
    // without queueing an event of its own.
    auto scroll_offset_a_finished_scroll_reports = [&](Optional<CSSPixelPoint> initial_scroll_offset) {
        if (abort_cause == SmoothScrollAbortCause::ReplacedByNewScroll)
            return Optional<CSSPixelPoint> {};
        return initial_scroll_offset;
    };
    for (auto const& finished : finished_async_scroll_operations)
        queue_scrollend_event_and_promise_resolution_for_finished_scroll(stable_node_id, finished.trigger, scroll_offset_a_finished_scroll_reports(finished.initial_scroll_offset), finished.promises);
    for (auto const& finished : finished_smooth_scrolls)
        queue_scrollend_event_and_promise_resolution_for_finished_scroll(stable_node_id, finished.trigger, scroll_offset_a_finished_scroll_reports(finished.initial_scroll_offset), finished.promises);

    settle_user_scroll_gesture_if_input_deadline_passed();
}

void LocalNavigable::process_main_thread_smooth_scrolls()
{
    auto now = MonotonicTime::now();

    Vector<MainThreadSmoothScroll> finished_smooth_scrolls;
    for (size_t index = 0; index < m_main_thread_smooth_scrolls.size();) {
        auto& smooth_scroll = m_main_thread_smooth_scrolls[index];
        if (!scroll_offset_for(smooth_scroll.stable_node_id).has_value()) {
            for (auto const& promise : smooth_scroll.promises)
                queue_async_scroll_operation_promise_resolution(promise);
            m_main_thread_smooth_scrolls.remove(index);
            continue;
        }

        auto elapsed_since_last_tick = now - smooth_scroll.last_tick;
        auto minimum_tick_duration = AK::Duration::from_milliseconds(1);
        smooth_scroll.elapsed += max(elapsed_since_last_tick, minimum_tick_duration);
        smooth_scroll.last_tick = now;
        auto sample = smooth_scroll.animation.sample(smooth_scroll.elapsed);
        set_scroll_offset_for(smooth_scroll.stable_node_id, sample.offset.to_type<CSSPixels>());
        if (sample.complete) {
            finished_smooth_scrolls.append(m_main_thread_smooth_scrolls.take(index));
        } else {
            ++index;
        }
    }

    for (auto const& finished : finished_smooth_scrolls)
        queue_scrollend_event_and_promise_resolution_for_finished_scroll(finished.stable_node_id, finished.trigger, finished.initial_scroll_offset, finished.promises);

    // A scroll whose scrolling box went away is dropped above without being reported, so settlement is retried for
    // every pass rather than only for the scrolls that ran to their destination.
    settle_user_scroll_gesture_if_input_deadline_passed();

    if (!m_main_thread_smooth_scrolls.is_empty())
        main_thread_event_loop().queue_task_to_update_the_rendering();
}

static bool adopt_async_viewport_scroll_delta(LocalNavigable& navigable, CSSPixelPoint scroll_delta)
{
    auto document = navigable.active_document();
    if (!document)
        return false;

    auto visual_viewport = document->visual_viewport();
    CSSPixelPoint page_position { CSSPixels(visual_viewport->page_left()), CSSPixels(visual_viewport->page_top()) };
    auto viewport_scroll_offset = navigable.viewport_scroll_offset();
    navigable.scroll_viewport_by_delta(scroll_delta);

    CSSPixelPoint new_page_position { CSSPixels(visual_viewport->page_left()), CSSPixels(visual_viewport->page_top()) };
    return new_page_position != page_position
        || navigable.viewport_scroll_offset() != viewport_scroll_offset;
}

void LocalNavigable::adopt_pending_async_scroll_offsets()
{
    if (!page().async_scrolling_enabled() || !has_compositor_context())
        return;

    // The compositor process may have already presented newer scroll offsets. Adopt the latest ones before running
    // rendering-update observers so they see the same scroll positions as the user.
    auto async_scroll_updates = compositor_context().take_pending_async_scroll_updates();

    // A gesture that both began and ended since the previous update is held for the length of this one, so that it
    // settles here rather than once its input deadline passes.
    if ((async_scroll_updates.user_scroll_gesture_in_progress || async_scroll_updates.user_scroll_gesture_ended)
        && !m_compositor_user_scroll_gesture_hold) {
        m_compositor_user_scroll_gesture_hold = make<UserScrollGestureHold>(*this);
    }

    ScopeGuard release_gesture_hold_once_its_scrolls_are_adopted = [&] {
        if (!async_scroll_updates.user_scroll_gesture_in_progress)
            m_compositor_user_scroll_gesture_hold = nullptr;
    };

    if (async_scroll_updates.scroll_offsets.is_empty() && async_scroll_updates.completed_operation_ids.is_empty())
        return;

    auto document = active_document();
    if (!document) {
        for (auto operation_id : async_scroll_updates.completed_operation_ids)
            resolve_async_scroll_operation(operation_id);
        return;
    }

    // https://drafts.csswg.org/css-scroll-snap-1/#scroll-types
    // AD-HOC: The scrolling the compositor process performs on its own is panning and scrollbar thumb dragging, both
    //         of which report where the user's input came to rest, so their offsets settle as absolute scrolls even
    //         though the specification lists a panning gesture among the relative scrolls with both an intended
    //         direction and end position.
    if (!async_scroll_updates.scroll_offsets.is_empty())
        note_user_scroll_input_intent(Painting::SnapSelectionStrategy::Type::EndPosition);

    // The compositor process merges the progress of a scroll that user input took over and the delta of that input
    // into one offset per scrolling box, so a box that such input scrolled is recognized from the scroll it ended.
    auto user_input_took_over_the_scroll_of = [&](Compositor::AsyncScrollNodeStableID stable_node_id) {
        return any_of(async_scroll_updates.operation_ids_taken_over_by_user_input, [&](auto operation_id) {
            return any_of(m_pending_async_scroll_operations, [&](auto const& pending_operation) {
                return pending_operation.operation_id == operation_id && pending_operation.stable_node_id == stable_node_id;
            });
        });
    };

    auto device_pixels_per_css_pixel = page().client().device_pixels_per_css_pixel();
    bool adopted_any_scroll_offset = false;
    for (auto const& async_scroll_offset : async_scroll_updates.scroll_offsets) {
        auto css_scroll_delta = async_scroll_offset_to_css_pixels(async_scroll_offset.unadopted_scroll_delta, device_pixels_per_css_pixel);
        bool has_in_flight_smooth_scroll = false;
        for (auto const& pending_operation : m_pending_async_scroll_operations) {
            if (pending_operation.stable_node_id == async_scroll_offset.stable_node_id) {
                has_in_flight_smooth_scroll = true;
                break;
            }
        }

        // NB: A smooth scroll of this box has an absolute destination. Adopt the
        //     compositor's absolute position so that replacing scroll snapshots
        //     during the animation cannot cause overlapping deltas to accumulate.
        if (has_in_flight_smooth_scroll) {
            auto scroll_offset_before_scroll = scroll_offset_for(async_scroll_offset.stable_node_id);
            auto css_scroll_offset = async_scroll_offset_to_css_pixels(async_scroll_offset.compositor_scroll_offset, device_pixels_per_css_pixel);
            if (set_scroll_offset_for(async_scroll_offset.stable_node_id, css_scroll_offset)) {
                adopted_any_scroll_offset = true;
                dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Main thread adopting async programmatic scroll offset {},{}",
                    async_scroll_offset.compositor_scroll_offset.x(), async_scroll_offset.compositor_scroll_offset.y());
            }

            // The gesture of the input that took the scroll over is latched here, so that the scrollend event the
            // taken-over scroll owes is delivered once that gesture settles rather than in the middle of it.
            if (user_input_took_over_the_scroll_of(async_scroll_offset.stable_node_id)) {
                if (auto target = scroll_event_target_for_async_scroll_node(*document, async_scroll_offset.stable_node_id))
                    queue_scrollend_event_after_user_scroll(*target, async_scroll_offset.stable_node_id, scroll_offset_before_scroll);
            }
            continue;
        }

        if (async_scroll_offset.stable_node_id.kind == Compositor::AsyncScrollNodeKind::Viewport) {
            if (async_scroll_offset.stable_node_id.node_id != document->unique_id())
                continue;
            if (adopt_async_viewport_scroll_delta(*this, css_scroll_delta)) {
                adopted_any_scroll_offset = true;
                dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Main thread adopting async viewport delta {},{}",
                    async_scroll_offset.unadopted_scroll_delta.x(), async_scroll_offset.unadopted_scroll_delta.y());
            }
            continue;
        }

        auto scroll_offset_before_scroll = scroll_offset_for(async_scroll_offset.stable_node_id);
        if (auto element = adopt_async_element_scroll_delta(*document, async_scroll_offset.stable_node_id, css_scroll_delta)) {
            adopted_any_scroll_offset = true;
            queue_scrollend_event_after_user_scroll(*element, async_scroll_offset.stable_node_id, scroll_offset_before_scroll);
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Main thread adopting async element delta {},{}",
                async_scroll_offset.unadopted_scroll_delta.x(), async_scroll_offset.unadopted_scroll_delta.y());
        }
    }

    if (adopted_any_scroll_offset)
        schedule_hover_update_after_async_scroll();

    for (auto operation_id : async_scroll_updates.completed_operation_ids) {
        auto completion = async_scroll_updates.operation_ids_taken_over_by_user_input.contains_slow(operation_id)
            ? AsyncScrollCompletion::TakenOverByUserInput
            : AsyncScrollCompletion::Finished;
        resolve_async_scroll_operation(operation_id, completion);
    }
}

void LocalNavigable::schedule_hover_update_after_async_scroll()
{
    static constexpr int hover_update_after_async_scroll_delay_ms = 100;

    if (!m_async_scroll_hover_update_timer) {
        m_async_scroll_hover_update_timer = Core::Timer::create_single_shot(hover_update_after_async_scroll_delay_ms, [this] {
            update_hover_after_async_scroll_stops();
        });
        m_async_scroll_hover_update_timer->start();
        return;
    }

    m_async_scroll_hover_update_timer->restart(hover_update_after_async_scroll_delay_ms);
}

void LocalNavigable::update_hover_after_async_scroll_stops()
{
    if (has_been_destroyed())
        return;
    event_handler().update_hover_after_scroll();
}

void LocalNavigable::cancel_hover_update_after_async_scroll()
{
    if (m_async_scroll_hover_update_timer)
        m_async_scroll_hover_update_timer->stop();
}

// https://html.spec.whatwg.org/multipage/webappapis.html#rendering-opportunity
bool LocalNavigable::has_a_rendering_opportunity() const
{
    // A navigable has a rendering opportunity if the user agent is currently able to present
    // the contents of the navigable to the user,
    // accounting for hardware refresh rate constraints and user agent throttling for performance reasons,
    // but considering content presentable even if it's outside the viewport.

    // A navigable's rendering opportunities are determined based on hardware constraints
    // such as display refresh rates and other factors such as page performance
    // or whether its active document's visibility state is "visible".
    // Rendering opportunities typically occur at regular intervals.

    // FIXME: Return `false` here if we're an inactive browser tab.
    return true;
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#inform-the-navigation-api-about-child-navigable-destruction
void LocalNavigable::inform_the_navigation_api_about_child_navigable_destruction()
{
    // 1. Inform the navigation API about aborting navigation in navigable.
    inform_the_navigation_api_about_aborting_navigation();

    // FIXME: 2. Let navigation be navigable's active window's navigation API.

    // FIXME: 3. Let traversalAPIMethodTrackers be a clone of navigation's upcoming traverse API method trackers.

    // FIXME: 4. For each apiMethodTracker of traversalAPIMethodTrackers: reject the finished promise for apiMethodTracker with a new "AbortError" DOMException created in navigation's relevant realm.
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#inform-the-navigation-api-about-aborting-navigation
void LocalNavigable::inform_the_navigation_api_about_aborting_navigation()
{
    // 1. If this algorithm is running on navigable's active window's relevant agent's event loop, then continue on to the following steps.
    //    Otherwise, queue a global task on the navigation and traversal task source given navigable's active window to run the following steps.
    // NB: WebContent uses a single main-thread event loop, so the active window's relevant agent is always running on
    //     the current event loop and we run the steps inline. Queuing here would defer the abort past the creation of
    //     a new ongoing navigate event by a subsequent fire_a_push_replace_reload_navigate_event, causing the deferred
    //     abort to cancel that newer event.

    // AD-HOC: Not in the spec but subsequent steps will fail if the navigable doesn't have an active window.
    if (!active_window())
        return;

    HTML::TemporaryExecutionContext execution_context { active_window()->principal_realm() };

    // 2. Let navigation be navigable's active window's navigation API.
    auto navigation = active_window()->navigation();

    // 3. If navigation's ongoing navigate event is null, then return.
    if (navigation->ongoing_navigate_event() == nullptr)
        return;

    // 4. Abort the ongoing navigation given navigation.
    navigation->abort_the_ongoing_navigation();
}

bool LocalNavigable::is_focused() const
{
    if (!m_page->client().has_focus())
        return false;

    // A top-level traversable retains system focus while the focus chain descends into a child navigable.
    if (is_traversable())
        return true;
    return &m_page->focused_navigable() == this;
}

Utf16String LocalNavigable::selected_text() const
{
    auto document = active_document();
    if (!document)
        return Utf16String {};

    document->update_layout(DOM::UpdateLayoutReason::NavigableSelectedText);

    auto const* input_element = as_if<HTML::HTMLInputElement>(document->active_element());
    if (input_element && input_element->type_state() == HTML::HTMLInputElement::TypeAttributeState::Password) {
        // Apparently nobody wants bullet characters. We leave the clipboard alone here like other browsers.
        return Utf16String {};
    }
    auto selection = document->get_selection();
    if (auto form_text = selection->try_form_control_selected_text_for_stringifier(); form_text.has_value())
        return form_text.release_value();

    auto range = selection->range();
    if (!range)
        return Utf16String {};
    return Editing::serialize_range_as_plain_text_for_clipboard(*range);
}

Utf16String LocalNavigable::selected_html_for_clipboard() const
{
    auto document = active_document();
    if (!document)
        return {};

    auto selection = document->get_selection();
    if (selection->try_form_control_selected_text_for_stringifier().has_value())
        return {};

    auto range = selection->range();
    if (!range)
        return {};
    return Editing::serialize_range_as_html_for_clipboard(*range);
}

Utf16String LocalNavigable::cut_selected_text() const
{
    auto document = active_document();
    if (!document)
        return {};

    auto* target = document->active_input_events_target();
    if (!target)
        return {};

    auto text = selected_text();
    if (text.is_empty())
        return {};

    target->handle_delete(UIEvents::InputTypes::deleteByCut);
    return text;
}

void LocalNavigable::select_all()
{
    auto document = active_document();
    if (!document)
        return;

    auto selection = document->get_selection();
    if (!selection)
        return;

    if (auto target = document->active_input_events_target()) {
        target->select_all();
    } else if (auto* body = document->body()) {
        (void)selection->select_all_children(*body);
    }
}

void LocalNavigable::paste(Utf16View text)
{
    auto document = active_document();
    if (!document)
        return;

    // The UI process hands off the text to paste: The system clipboard's text for chrome-initiated paste or the primary
    // selection's text for middle-click paste. Run the paste action with that as the content to paste — so the paste
    // clipboard event fires, and canceling suppresses the insertion (as for a paste initiated with keyboard shortcut).
    // INTEROP: For middle-click paste, Gecko and Blink also fire the paste event — with the event's clipboard data
    //          reading from the primary selection.
    auto data_store = DragDataStore::create();
    data_store->add_item({
        .kind = DragDataStoreItem::Kind::Text,
        .type_string = "text/plain"_utf16_fly_string,
        .data = Utf16String::from_utf16(text),
        .file_data = {},
        .file_name = {},
    });
    m_event_handler.perform_paste_action(data_store);
}

void LocalNavigable::paste_from_clipboard()
{
    // Run the whole paste action: It retrieves the system clipboard's contents from the UI process itself, which
    // preserves every supported clipboard representation — where a bare-text handover would keep only the text.
    (void)m_event_handler.perform_paste_action();
}

void LocalNavigable::undo()
{
    auto document = active_document();
    if (!document)
        return;

    (void)Editing::perform_history_action(*document, Editing::HistoryAction::Undo);
}

void LocalNavigable::redo()
{
    auto document = active_document();
    if (!document)
        return;

    (void)Editing::perform_history_action(*document, Editing::HistoryAction::Redo);
}

void LocalNavigable::set_marked_text_from_input_method(Utf16View text)
{
    // Platform input methods call this on each composition update, with the current marked/preedit text. LibWeb owns
    // the marked-text range – so each update replaces the previously-marked text. The UI doesn't track the preedit
    // extent or pass a replacement length. An empty marked string means there's no preedit: so, clear any text marked
    // thus far, and end the composition — rather than starting or keeping a composition that has no marked text.
    if (text.is_empty()) {
        if (m_input_method_composition_node)
            replace_input_method_marked_text(text);
        m_input_method_composition_node = nullptr;
        return;
    }
    replace_input_method_marked_text(text);
}

void LocalNavigable::commit_text_from_input_method(Utf16View text, i32 replacement_start, i32 replacement_length)
{
    if ((replacement_start != 0 || replacement_length != 0) && apply_input_method_commit_replacement(text, replacement_start, replacement_length)) {
        m_input_method_composition_node = nullptr;
        return;
    }

    // The input method has committed text and finished the composition. Replace the marked text with the committed
    // text, then end the composition — so the text becomes ordinary editable content.
    replace_input_method_marked_text(text);
    m_input_method_composition_node = nullptr;
}

void LocalNavigable::unmark_text_from_input_method()
{
    // The input method has finished the composition — leaving the current marked text in place. End the composition
    // without altering the content.
    m_input_method_composition_node = nullptr;
}

void LocalNavigable::replace_input_method_marked_text(Utf16View text)
{
    // Insert text from a platform input method into the currently-focused editable, via the same input-events target
    // that keyboard typing uses — so observers see the correct InputEvent.inputType.
    auto document = active_document();
    if (!document || !document->is_fully_active()) {
        m_input_method_composition_node = nullptr;
        return;
    }
    auto* target = document->active_input_events_target();
    if (!target) {
        m_input_method_composition_node = nullptr;
        return;
    }

    // Drop a stale composition start (for example, if the editable content was replaced out from under us, or focus moved
    // to a different editable).
    if (m_input_method_composition_node && (!m_input_method_composition_node->is_connected() || document->active_input_events_target(m_input_method_composition_node.ptr()) != target))
        m_input_method_composition_node = nullptr;

    // The caret is the end of the marked text. Read it while the selection is still collapsed. Forming the marked-text
    // selection below would otherwise make cursor_position() return null for form controls.
    auto caret = document->cursor_position();
    if (!caret) {
        if (!m_input_method_composition_node)
            target->handle_insert(UIEvents::InputTypes::insertText, text);
        return;
    }

    if (m_input_method_composition_node) {
        // A composition is already in progress. Select the existing marked text [composition start, caret] — so that
        // the insertion below replaces it.
        target->set_selection_anchor(*m_input_method_composition_node, m_input_method_composition_offset);
        target->set_selection_focus(caret->node(), caret->offset());
    } else {
        // Begin a new composition at the caret. The marked text spans from here to the caret as it is updated.
        m_input_method_composition_node = caret->node();
        m_input_method_composition_offset = caret->offset();
    }

    target->handle_insert(UIEvents::InputTypes::insertText, text);
}

bool LocalNavigable::apply_input_method_commit_replacement(Utf16View text, i32 replacement_start, i32 replacement_length)
{
    if (replacement_length < 0)
        return false;

    auto document = active_document();
    if (!document || !document->is_fully_active()) {
        m_input_method_composition_node = nullptr;
        return true;
    }
    auto* target = document->active_input_events_target();
    if (!target) {
        m_input_method_composition_node = nullptr;
        return true;
    }

    if (m_input_method_composition_node && (!m_input_method_composition_node->is_connected() || document->active_input_events_target(m_input_method_composition_node.ptr()) != target))
        m_input_method_composition_node = nullptr;

    auto caret = document->cursor_position();
    if (!caret) {
        if (!m_input_method_composition_node) {
            target->handle_insert(UIEvents::InputTypes::insertText, text);
            return true;
        }
        return false;
    }

    auto preedit_start_node = m_input_method_composition_node ? m_input_method_composition_node : caret->node();
    auto preedit_start_offset = m_input_method_composition_node ? m_input_method_composition_offset : caret->offset();
    if (!preedit_start_node || preedit_start_node != caret->node())
        return false;

    size_t replacement_start_offset = preedit_start_offset;
    if (replacement_start < 0) {
        auto offset_delta = static_cast<size_t>(-static_cast<i64>(replacement_start));
        if (offset_delta > preedit_start_offset)
            return false;
        replacement_start_offset -= offset_delta;
    } else {
        auto offset_delta = static_cast<size_t>(replacement_start);
        if (offset_delta > NumericLimits<size_t>::max() - replacement_start_offset)
            return false;
        replacement_start_offset += offset_delta;
    }

    auto replacement_length_as_size = static_cast<size_t>(replacement_length);
    if (replacement_start_offset > preedit_start_node->length() || replacement_length_as_size > preedit_start_node->length() - replacement_start_offset)
        return false;

    target->set_selection_anchor(*preedit_start_node, replacement_start_offset);
    target->set_selection_focus(*preedit_start_node, replacement_start_offset + replacement_length_as_size);
    target->handle_insert(UIEvents::InputTypes::insertText, text);
    return true;
}

// https://drafts.csswg.org/css-view-transitions-1/#snapshot-containing-block
CSSPixelRect LocalNavigable::snapshot_containing_block()
{
    // The snapshot containing block is a rectangle that covers all areas of the window that could potentially display
    // page content (and is therefore consistent regardless of root scrollbars or interactive widgets).

    // Within a child navigable, the snapshot containing block is the union of the navigable’s viewport with any scrollbar gutters.
    // FIXME: Actually get the correct rectangle here.
    return viewport_rect();
}
// https://drafts.csswg.org/css-view-transitions-1/#snapshot-containing-block-size
CSSPixelSize LocalNavigable::snapshot_containing_block_size()
{
    return this->snapshot_containing_block().size();
}

void LocalNavigable::register_navigation_observer(Badge<NavigationObserver>, NavigationObserver& navigation_observer)
{
    m_navigation_observers.append(navigation_observer);
}

void LocalNavigable::unregister_navigation_observer(Badge<NavigationObserver>, NavigationObserver& navigation_observer)
{
    m_navigation_observers.remove(navigation_observer);
}

// https://html.spec.whatwg.org/multipage/document-lifecycle.html#stop-document-loading
void LocalNavigable::stop_loading()
{
    // 1. Let document be navigable's active document.
    auto document = active_document();

    // AD-HOC: The HTML Standard does not cancel planned navigations here, but Chromium, WebKit, and Gecko do so when
    //         handling window.stop(). Prevent navigations deferred behind an ongoing traversal from starting once it
    //         completes. See https://github.com/whatwg/html/issues/12609.
    clear_pending_navigations();

    // 2. If document's unload counter is 0, and navigable's ongoing navigation is a navigation ID, then set the ongoing navigation for navigable to null.
    if (document->unload_counter() == 0 && ongoing_navigation().has<Utf16String>())
        set_ongoing_navigation(Empty {});

    // 3. Abort a document and its descendants given document.
    document->abort_a_document_and_its_descendants();
}

void LocalNavigable::set_has_session_history_entry_and_ready_for_navigation()
{
    m_has_session_history_entry_and_ready_for_navigation = true;
    process_pending_navigations();
}

void LocalNavigable::clear_parent_compositor_context()
{
    if (has_compositor_context())
        compositor_context().set_parent_context({});
}

void LocalNavigable::destroy_compositor_context()
{
    clear_parent_compositor_context();
    m_compositor_context.clear();
}

void LocalNavigable::repaint_after_compositor_process_reconnect()
{
    resolve_all_pending_async_scroll_operations();

    if (has_compositor_context()) {
        if (auto parent = this->parent()) {
            auto& local_parent = as<LocalNavigable>(*parent);
            if (local_parent.has_compositor_context())
                compositor_context().set_parent_context(local_parent.compositor_context().id());
        }
        compositor_context().viewport_size_updated(
            page().css_to_device_rect(viewport_rect()).size().to_type<int>(),
            Compositor::WindowResizingInProgress::No);

        m_needs_repaint = true;
        m_needs_to_record_display_list = true;
        m_compositor_display_list_paint_config.clear();
        m_compositor_display_list.clear();
        m_compositor_visual_context_tree.clear();
        m_compositor_scroll_state_snapshot.clear();
        m_compositor_display_list_resources = {};
    }

    for (auto const& child_navigable : child_navigables())
        child_navigable->repaint_after_compositor_process_reconnect();
}

void LocalNavigable::set_should_show_line_box_borders(bool value)
{
    m_should_show_line_box_borders = value;
    set_needs_repaint();

    for (auto const& child_navigable : child_navigables())
        child_navigable->set_should_show_line_box_borders(value);
}

void LocalNavigable::set_should_show_caret_hit_test_debug_overlay(bool value)
{
    m_should_show_caret_hit_test_debug_overlay = value;

    if (auto document = active_document()) {
        if (value)
            document->set_needs_repaint(Badge<HTML::LocalNavigable> {}, InvalidateDisplayList::Yes);
        else
            document->set_caret_hit_test_debug_rect({});
    }

    for (auto const& child_navigable : child_navigables())
        child_navigable->set_should_show_caret_hit_test_debug_overlay(value);
}

bool LocalNavigable::record_display_list_and_scroll_state(PaintConfig paint_config, Gfx::IntRect* damage_rect)
{
    if (!has_compositor_context())
        return false;

    auto document = active_document();
    if (!document)
        return false;

    adopt_pending_async_scroll_offsets();
    document->update_paint_and_hit_testing_properties_if_needed();

    auto should_record_display_list = m_needs_to_record_display_list
        || !m_compositor_display_list_paint_config.has_value()
        || !(m_compositor_display_list_paint_config.value() == paint_config);

    RefPtr<Painting::DisplayList> display_list;
    Painting::DisplayListResourceSet display_list_resources;
    Painting::DisplayListResourceTransaction resource_transaction;
    Optional<Painting::AccumulatedVisualContextTree> visual_context_tree;
    auto& document_paint_state = document->paint_state();
    if (should_record_display_list) {
        display_list = document->record_display_list(paint_config, m_display_list_resource_storage, Painting::PaintCommandCacheMode::ReadWrite);
        if (!display_list)
            return false;
        VERIFY(document->has_committed_viewport_box());
        visual_context_tree = document_paint_state.visual_context_tree(*document);
        if (document_paint_state.display_list_used_as_paint_command_cache_source() == display_list.ptr()) {
            display_list_resources.include(document_paint_state.paint_command_cache_source_referenced_resources());
        } else {
            // A recording downgraded to cache-read-only leaves the retained source and the cached ranges
            // into it live, so the resources they reference must survive the pruning below.
            display_list_resources = m_display_list_resource_storage.collect_referenced_resources(*display_list);
            document_paint_state.append_paint_command_cache_source_resources(display_list_resources);
        }
        resource_transaction = m_display_list_resource_storage.create_transaction(
            m_compositor_display_list_resources,
            display_list_resources);
    }

    VERIFY(document->has_committed_viewport_box());
    auto visual_context_tree_needs_compositor_update = document_paint_state.visual_context_tree_needs_compositor_update();
    document_paint_state.refresh_scroll_state(*document);

    Painting::ScrollStateSnapshot scroll_state_snapshot { document_paint_state.scroll_state_snapshot() };
    auto viewport_rect = page().css_to_device_rect(this->viewport_rect()).to_type<int>();
    Gfx::IntRect surface_rect { {}, viewport_rect.size() };
    if (damage_rect)
        *damage_rect = surface_rect;
    if (should_record_display_list) {
        if (damage_rect
            && m_compositor_display_list
            && m_compositor_visual_context_tree.has_value()
            && m_compositor_scroll_state_snapshot.has_value()
            && m_compositor_scroll_state_snapshot->device_offsets() == scroll_state_snapshot.device_offsets()
            && m_compositor_display_list_paint_config == paint_config) {
            auto computed_damage = Painting::compute_display_list_damage(
                m_compositor_display_list->command_bytes(),
                *m_compositor_visual_context_tree,
                *m_compositor_scroll_state_snapshot,
                display_list->command_bytes(),
                *visual_context_tree,
                scroll_state_snapshot,
                surface_rect);
            if (computed_damage.has_value())
                *damage_rect = *computed_damage;
        }

        m_compositor_display_list = display_list;
        m_compositor_visual_context_tree = *visual_context_tree;
        m_compositor_scroll_state_snapshot = scroll_state_snapshot;
        m_compositor_display_list_visual_context_tree_version = display_list->compatible_visual_context_tree_version();
        compositor_context().update_display_list(*display_list, visual_context_tree.release_value(), move(resource_transaction), move(scroll_state_snapshot));
        document_paint_state.did_update_visual_context_tree_in_compositor();
        m_display_list_resource_storage.retain_only(display_list_resources);
        m_compositor_display_list_resources = move(display_list_resources);
        m_needs_to_record_display_list = false;
        m_compositor_display_list_paint_config = paint_config;
    } else {
        if (visual_context_tree_needs_compositor_update) {
            VERIFY(document_paint_state.visual_context_tree(*document).version() == m_compositor_display_list_visual_context_tree_version);
            compositor_context().update_visual_context_tree(document_paint_state.visual_context_tree(*document));
            document_paint_state.did_update_visual_context_tree_in_compositor();
        }
        compositor_context().update_scroll_state(move(scroll_state_snapshot));
    }
    return true;
}

void LocalNavigable::paint_next_frame()
{
    if (has_been_destroyed())
        return;
    if (!has_compositor_context()) {
        m_needs_repaint = false;
        return;
    }

    auto viewport_rect = page().css_to_device_rect(this->viewport_rect()).to_type<int>();
    PaintConfig paint_config { .paint_overlay = true, .should_show_line_box_borders = m_should_show_line_box_borders, .should_show_caret_hit_test_debug_overlay = m_should_show_caret_hit_test_debug_overlay };
    if (is_top_level_traversable()) {
        paint_config.canvas_fill_rect = Gfx::IntRect { {}, viewport_rect.size() };
    } else {
        // Nested navigables paint transparent bitmaps for their parent compositor context.
        auto parent = this->parent();
        if (!parent || !as<LocalNavigable>(*parent).has_compositor_context())
            return;
    }

    m_needs_repaint = false;

    Gfx::IntRect damage_rect;
    if (!record_display_list_and_scroll_state(paint_config, &damage_rect))
        return;
    viewport_rect = page().css_to_device_rect(this->viewport_rect()).to_type<int>();
    compositor_context().present_frame(viewport_rect, damage_rect);
}

void LocalNavigable::render_screenshot(Gfx::PaintingSurface& painting_surface, PaintConfig paint_config, Function<void()>&& callback)
{
    if (!has_compositor_context()) {
        callback();
        return;
    }

    if (!record_display_list_and_scroll_state(paint_config)) {
        callback();
        return;
    }
    compositor_context().request_screenshot(painting_surface, move(callback));
}

void LocalNavigable::abort_in_flight_smooth_scrolls(Compositor::AsyncScrollNodeStableID stable_node_id, SmoothScrollAbortCause abort_cause)
{
    if (has_compositor_context())
        compositor_context().cancel_smooth_scroll(stable_node_id);
    resolve_pending_smooth_scrolls(stable_node_id, abort_cause);
}

void LocalNavigable::abort_in_flight_smooth_scrolls_taken_over_by_user_input(Compositor::AsyncScrollNodeStableID stable_node_id, CSSPixelPoint scroll_offset_at_gesture_start)
{
    auto document = active_document();
    auto target = document ? scroll_event_target_for_async_scroll_node(*document, stable_node_id) : nullptr;

    // A user scroll that is still running belongs to the gesture this input continues, so that gesture is latched
    // before the scroll is taken over from it and the scrollend event the scroll owes is delivered once the gesture
    // settles.
    auto in_flight_scroll = in_flight_scroll_for(stable_node_id);
    if (target && in_flight_scroll.has_value() && in_flight_scroll->trigger == ScrollTrigger::UserInput) {
        if (!latched_user_scroll_gesture_for(*target, stable_node_id))
            queue_scrollend_event_after_user_scroll(*target, stable_node_id, scroll_offset_at_gesture_start);
    }

    abort_in_flight_smooth_scrolls(stable_node_id, SmoothScrollAbortCause::TakenOverByUserInput);
}

GC::Ref<WebIDL::Promise> LocalNavigable::perform_a_scroll_of_a_scrolling_box(Compositor::AsyncScrollNodeStableID stable_node_id, CSSPixelPoint position, Bindings::ScrollBehavior behavior, GC::Ptr<DOM::Element> associated_element, ScrollTrigger trigger, Optional<CSSPixelPoint> relative_displacement, DestinationSnapping destination_snapping, Compositor::ScrollAnimationKind animation_kind)
{
    auto document = active_document();
    VERIFY(document);

    // A gesture latched for this scrolling box may run out of input while this scroll is being started, so its
    // settlement waits until this scroll is under way rather than enqueuing a scroll of its own alongside it.
    ++m_scrolls_being_started;
    ScopeGuard settle_gesture_that_ran_out_of_input = [this] {
        if (--m_scrolls_being_started > 0)
            return;
        if (!m_user_scroll_settlement_awaits_scroll_start)
            return;
        m_user_scroll_settlement_awaits_scroll_start = false;
        settle_user_scroll_gesture_if_input_deadline_passed();
    };

    auto initial_scroll_offset = scroll_offset_for(stable_node_id);
    if (!initial_scroll_offset.has_value())
        return WebIDL::create_resolved_promise_for(*document, JS::js_undefined());

    // https://drafts.csswg.org/css-scroll-snap-1/#snap-strictness
    // If a valid snap position exists then the scroll container must snap at the termination of a scroll (if none
    // exist then no snapping occurs).
    if (trigger == ScrollTrigger::Programmatic && destination_snapping == DestinationSnapping::SelectSnapPosition) {
        abandon_snapping_of_user_scroll_gesture(stable_node_id);
        document->update_layout(DOM::UpdateLayoutReason::ElementScroll);
        if (auto const* snap_container = layout_node_for_async_scroll_node(*document, stable_node_id)) {
            Painting::SnapSelectionStrategy strategy;
            if (relative_displacement.has_value() && !relative_displacement->is_zero())
                strategy = { Painting::SnapSelectionStrategy::Type::EndPositionAndDirection, *initial_scroll_offset, *relative_displacement };
            auto snap_destination = Painting::adjust_scroll_destination_for_snapping(*snap_container, position, strategy);
            position = snap_destination.position;
            record_snapped_areas_of_scroll_container(*document, stable_node_id, snap_destination);
        }
    }

    auto should_scroll_smoothly = behavior == Bindings::ScrollBehavior::Smooth;
    if (behavior == Bindings::ScrollBehavior::Auto && associated_element) {
        if (auto const* values = associated_element->style_group<CSS::ComputedValues::MiscResetValues>())
            should_scroll_smoothly = static_cast<CSS::ScrollBehavior>(values->scroll_behavior) == CSS::ScrollBehavior::Smooth;
    }

    // AD-HOC: A smooth scroll requested while a smooth scroll of the same scrolling box toward the same position is in
    //         flight continues that scroll instead of restarting it, matching other engines.
    if (should_scroll_smoothly) {
        if (auto* promises = promises_of_smooth_scroll_in_flight_toward(stable_node_id, position, trigger)) {
            auto scroll_promise = WebIDL::create_promise_for(*document);
            promises->append(scroll_promise);
            return scroll_promise;
        }
    }

    // https://drafts.csswg.org/cssom-view-1/#perform-a-scroll
    // 1. Abort any ongoing smooth scroll for box.
    // 2. Resolve all pending scroll promises for box.
    abort_in_flight_smooth_scrolls(stable_node_id, SmoothScrollAbortCause::ReplacedByNewScroll);

    // 3. Let scrollPromise be a new promise and return it while the remaining
    //    steps run in parallel.
    auto scroll_promise = WebIDL::create_promise_for(*document);

    // 4. If the user agent honors the scroll-behavior property and either the
    //    requested behavior or the associated element's computed behavior is
    //    smooth, perform a smooth scroll. Otherwise, perform an instant scroll.
    if (!should_scroll_smoothly) {
        auto did_scroll = set_scroll_offset_for(stable_node_id, position);
        if (did_scroll)
            queue_scrollend_event(stable_node_id, trigger, initial_scroll_offset);
        WebIDL::resolve_promise(scroll_promise);
        return scroll_promise;
    }

    if (has_compositor_context()) {
        // NB: Do not adopt compositor progress while replacing one smooth scroll
        //     with another. All listeners in the current JavaScript task must
        //     observe the same main-thread scroll offset. The compositor starts
        //     the replacement from its own current visual offset.
        auto device_pixels_per_css_pixel = page().client().device_pixels_per_css_pixel();
        auto target_offset = Gfx::FloatPoint {
            static_cast<float>(position.x().to_double() * device_pixels_per_css_pixel),
            static_cast<float>(position.y().to_double() * device_pixels_per_css_pixel),
        };
        auto main_thread_offset = Gfx::FloatPoint {
            static_cast<float>(initial_scroll_offset->x().to_double() * device_pixels_per_css_pixel),
            static_cast<float>(initial_scroll_offset->y().to_double() * device_pixels_per_css_pixel),
        };
        auto viewport_rect = page().css_to_device_rect(this->viewport_rect()).to_type<int>();
        auto enqueue_result = compositor_context().smooth_scroll_to(stable_node_id, target_offset, main_thread_offset, viewport_rect, device_pixels_per_css_pixel, animation_kind);
        if (enqueue_result.accepted) {
            VERIFY(enqueue_result.operation_id.has_value());
            m_pending_async_scroll_operations.append(PendingAsyncScrollOperation {
                .operation_id = *enqueue_result.operation_id,
                .promises = { scroll_promise },
                .stable_node_id = stable_node_id,
                .initial_scroll_offset = *initial_scroll_offset,
                .destination_scroll_offset = position,
                .trigger = trigger,
            });
            return scroll_promise;
        }
    }

    // NB: A page can lack compositor scroll state before its first paint, or
    //     asynchronous scrolling can be disabled. Keep the same algorithm on
    //     the main thread in those cases.
    if (has_compositor_context()) {
        // NB: The compositor rejected the replacement, so consume its last
        //     offset before falling back to a main-thread animation.
        adopt_pending_async_scroll_offsets();
        initial_scroll_offset = scroll_offset_for(stable_node_id);
        if (!initial_scroll_offset.has_value()) {
            WebIDL::resolve_promise(scroll_promise);
            return scroll_promise;
        }
    }
    if (position == *initial_scroll_offset) {
        WebIDL::resolve_promise(scroll_promise);
        return scroll_promise;
    }
    m_main_thread_smooth_scrolls.append(MainThreadSmoothScroll {
        .stable_node_id = stable_node_id,
        .animation = Compositor::SmoothScrollAnimation { initial_scroll_offset->to_type<float>(), position.to_type<float>(), 1.0, animation_kind },
        .last_tick = MonotonicTime::now(),
        .elapsed = AK::Duration::zero(),
        .initial_scroll_offset = *initial_scroll_offset,
        .destination_scroll_offset = position,
        .promises = { scroll_promise },
        .trigger = trigger,
    });
    main_thread_event_loop().queue_task_to_update_the_rendering();
    return scroll_promise;
}

GC::Ref<WebIDL::Promise> LocalNavigable::perform_a_scroll_of_an_element(DOM::Element& element, CSSPixelPoint position, Bindings::ScrollBehavior behavior, Optional<CSSPixelPoint> relative_displacement)
{
    return perform_a_scroll_of_a_scrolling_box({
                                                   .node_id = element.unique_id(),
                                                   .kind = Compositor::AsyncScrollNodeKind::Element,
                                               },
        position, behavior, element, ScrollTrigger::Programmatic, relative_displacement);
}

bool LocalNavigable::perform_a_snapped_relative_user_scroll(Layout::Node& scroll_container, CSSPixelPoint delta, Painting::SnapSelectionStrategy::Type strategy_type, SnapStepAccumulation step_accumulation, Compositor::ScrollAnimationKind animation_kind)
{
    auto document = active_document();
    if (!document)
        return false;

    auto stable_node_id = Painting::async_scroll_node_stable_id(scroll_container);
    if (!stable_node_id.has_value())
        return false;

    auto current_scroll_offset = scroll_offset_for(*stable_node_id);
    if (!current_scroll_offset.has_value())
        return false;

    auto target = scroll_event_target_for_async_scroll_node(*document, *stable_node_id);
    if (!target)
        return false;

    // A scroll started for any reason other than user input is going somewhere the gesture never asked for, so a
    // gesture's steps then travel from the scrolling box itself instead.
    auto in_flight_scroll = in_flight_scroll_for(*stable_node_id);
    Optional<CSSPixelPoint> in_flight_destination;
    if (in_flight_scroll.has_value() && in_flight_scroll->trigger == ScrollTrigger::UserInput)
        in_flight_destination = in_flight_scroll->destination_scroll_offset;

    // A step selects its snap position from the offset the gesture's input deltas have reached rather than from the
    // snap position it is scrolling to, so a burst of steps advances by the distance they asked for instead of by one
    // snap position each.
    auto* latched_gesture = latched_user_scroll_gesture_for(*target, *stable_node_id);
    bool travels_from_input_deltas = latched_gesture
        && (step_accumulation == SnapStepAccumulation::UntilGestureSettles || in_flight_destination.has_value());
    auto step_start = travels_from_input_deltas
        ? latched_gesture->unsnapped_scroll_destination.value_or(*current_scroll_offset)
        : *current_scroll_offset;
    auto unsnapped_destination = Painting::clamp_scroll_offset(scroll_container, step_start + delta);

    // https://drafts.csswg.org/css-scroll-snap-1/#scroll-types
    // NOTE: Scroll snapping responds to a relative scroll by finding the nearest valid snap position in the intended
    //       direction (if possible), so a snapped element can't get "trapped" when the snap positions are far apart.
    Painting::SnapSelectionStrategy strategy { strategy_type, step_start, delta };
    // NB: A step with only an intended direction ignores every snap position up to the offset its input asked for. A
    //     step with an intended end position selects the snap position nearest that destination, so snap positions
    //     short of it remain selectable.
    if (strategy_type == Painting::SnapSelectionStrategy::Type::Direction)
        strategy.starting_positions_boundary = unsnapped_destination;
    auto snap_destination = Painting::adjust_scroll_destination_for_snapping(scroll_container, unsnapped_destination, strategy);

    // NB: The step travels only along axes the container selects no snap position in, so it is left to the ordinary
    //     relative scroll.
    if (!(snap_destination.snapped_x && delta.x() != 0) && !(snap_destination.snapped_y && delta.y() != 0))
        return false;

    record_snapped_areas_of_scroll_container(*document, *stable_node_id, snap_destination);

    // NB: A step whose selected snap position is where the scrolling box already rests, or is already scrolling to, is
    //     consumed without disturbing where it is going.
    bool step_rests_at_its_snap_position = snap_destination.position == in_flight_destination.value_or(*current_scroll_offset);
    if (!step_rests_at_its_snap_position)
        queue_scrollend_event_after_user_scroll(*target, *stable_node_id, *current_scroll_offset, SnapPositionSelection::PerScroll);

    // NB: Latching the gesture above may have moved the entry the offset is recorded on, so it is looked up again.
    if (auto* entry = latched_user_scroll_gesture_for(*target, *stable_node_id))
        entry->unsnapped_scroll_destination = unsnapped_destination;

    if (step_rests_at_its_snap_position)
        return true;

    TemporaryExecutionContext temporary_execution_context { HTML::relevant_realm(*document) };
    perform_a_scroll_of_a_scrolling_box(*stable_node_id, snap_destination.position, Bindings::ScrollBehavior::Smooth, nullptr, ScrollTrigger::UserInput, {}, DestinationSnapping::SelectSnapPosition, animation_kind);
    return true;
}

// https://drafts.csswg.org/css-scroll-snap-1/#choosing
bool LocalNavigable::perform_a_snapped_momentum_scroll(Layout::Node& scroll_container, CSSPixelPoint momentum_delta)
{
    if (m_momentum_snap_position_selection == MomentumSnapPositionSelection::ScrollingToSelectedPosition)
        return true;

    if (m_momentum_snap_position_selection == MomentumSnapPositionSelection::NoPositionSelected)
        return false;

    auto remaining_displacement = m_momentum_fling_estimator.estimate_remaining_displacement(momentum_delta);
    if (!remaining_displacement.has_value())
        return false;

    if (!perform_a_snapped_relative_user_scroll(scroll_container, *remaining_displacement, Painting::SnapSelectionStrategy::Type::EndPositionAndDirection, SnapStepAccumulation::UntilScrollFinishes, Compositor::ScrollAnimationKind::Momentum)) {
        m_momentum_snap_position_selection = MomentumSnapPositionSelection::NoPositionSelected;
        return false;
    }

    m_momentum_snap_position_selection = MomentumSnapPositionSelection::ScrollingToSelectedPosition;
    return true;
}

GC::Ref<WebIDL::Promise> LocalNavigable::scroll_viewport_by_delta(CSSPixelPoint delta, Bindings::ScrollBehavior behavior)
{
    auto vv = active_document()->visual_viewport();
    CSSPixelPoint page_position { CSSPixels(vv->page_left()), CSSPixels(vv->page_top()) };
    return perform_a_scroll_of_the_viewport(page_position + delta, behavior, ScrollTrigger::UserInput);
}

// https://drafts.csswg.org/cssom-view/#viewport-perform-a-scroll
GC::Ref<WebIDL::Promise> LocalNavigable::perform_a_scroll_of_the_viewport(CSSPixelPoint position, Bindings::ScrollBehavior behavior, ScrollTrigger trigger, Optional<CSSPixelPoint> relative_displacement)
{
    // AD-HOC: User input keeps the scroll gesture in progress even when this scroll does not move the viewport, such
    //         as when a held scroll key repeats at the scroll extent.
    if (trigger == ScrollTrigger::UserInput)
        defer_user_scroll_settlement();

    // 1. Let doc be the viewport’s associated Document.
    auto doc = active_document();

    // 2. Let vv be the VisualViewport whose associated document is doc.
    auto vv = doc->visual_viewport();

    CSSPixelRect viewport_rect = { m_viewport_scroll_offset, m_viewport_size };

    // 3. Let maxX be the difference between viewport’s scrolling box’s width and the value of vv’s width attribute.
    auto max_x = viewport_rect.width().to_double() - vv->width();

    // 4. Let maxY be the difference between viewport’s scrolling box’s height and the value of vv’s height attribute.
    auto max_y = viewport_rect.height().to_double() - vv->height();

    // 5. Let dx be the horizontal component of position - the value vv’s pageLeft attribute
    auto dx = position.x().to_double() - vv->page_left();

    // 6. Let dy be the vertical component of position - the value of vv’s pageTop attribute
    auto dy = position.y().to_double() - vv->page_top();

    // 7. Let visual x be the value of vv’s offsetLeft attribute.
    auto visual_x = vv->offset_left();

    // 8. Let visual y be the value of vv’s offsetTop attribute.
    auto visual_y = vv->offset_top();

    // 9. Let visual dx be min(maxX, max(0, visual x + dx)) - visual x.
    auto visual_dx = min(max_x, max(0.0, visual_x + dx)) - visual_x;

    // 10. Let visual dy be min(maxY, max(0, visual y + dy)) - visual y.
    auto visual_dy = min(max_y, max(0.0, visual_y + dy)) - visual_y;

    // 11. Let layout dx be dx - visual dx
    auto layout_dx = dx - visual_dx;

    // 12. Let layout dy be dy - visual dy
    auto layout_dy = dy - visual_dy;

    // 13. Let element be doc’s root element if there is one, null otherwise.

    // 14. Perform a scroll of the viewport’s scrolling box to its current scroll position + (layout dx, layout dy)
    //     with element as the associated element, and behavior as the scroll behavior. Let scrollPromise1 be the
    //     Promise returned from this step.
    TemporaryExecutionContext temporary_execution_context { HTML::relevant_realm(*doc) };

    // 15. Perform a scroll of vv’s scrolling box to its current scroll position + (visual dx, visual dy) with element
    //     as the associated element, and behavior as the scroll behavior. Let scrollPromise2 be the Promise returned
    //     from this step.
    // FIXME: Get a Promise from this.
    // AD-HOC: Step 15 is performed before step 14 so that the visual viewport's scroll and scrollend events are
    //         dispatched before the window's.
    CSSPixelPoint visual_delta { visual_dx, visual_dy };
    vv->scroll_by(visual_delta);
    if (visual_delta.is_zero())
        doc->set_needs_repaint(Badge<HTML::LocalNavigable> {}, InvalidateDisplayList::No);
    else
        queue_scrollend_event(*doc, *vv, {}, trigger);

    // NB: Must update layout before accessing paintables.
    doc->update_layout(DOM::UpdateLayoutReason::NavigableViewportScroll);

    auto minimum_scroll_offset = Painting::minimum_scroll_offset(*doc->layout_node()).to_type<double>();
    auto maximum_scroll_offset = Painting::maximum_scroll_offset(*doc->layout_node()).to_type<double>();
    auto new_viewport_scroll_offset = m_viewport_scroll_offset.to_type<double>() + Gfx::Point(layout_dx, layout_dy);
    // NOTE: Clamp to the scrolling area.
    new_viewport_scroll_offset.set_x(clamp(new_viewport_scroll_offset.x(), minimum_scroll_offset.x(), maximum_scroll_offset.x()));
    new_viewport_scroll_offset.set_y(clamp(new_viewport_scroll_offset.y(), minimum_scroll_offset.y(), maximum_scroll_offset.y()));

    auto scroll_promise = perform_a_scroll_of_a_scrolling_box({
                                                                  .node_id = doc->unique_id(),
                                                                  .kind = Compositor::AsyncScrollNodeKind::Viewport,
                                                              },
        new_viewport_scroll_offset.to_type<CSSPixels>(), behavior, doc->document_element(), trigger, relative_displacement);

    // 17. Return scrollPromise, and run the remaining steps in parallel.
    // 18. Resolve scrollPromise when both scrollPromise1 and scrollPromise2 have settled.
    // FIXME: Actually wait for visual viewport scrolling to settle as well.
    return scroll_promise;
}

void LocalNavigable::reset_zoom()
{
    auto document = active_document();
    if (!document)
        return;
    document->visual_viewport()->reset();
}

bool LocalNavigable::has_inclusive_ancestor_with_visibility_hidden() const
{
    if (auto container = this->container()) {
        if (auto const* values = container->style_group<CSS::ComputedValues::InheritedBoxValues>()) {
            if (static_cast<CSS::Visibility>(values->visibility) == CSS::Visibility::Hidden)
                return true;
        }
        if (auto ancestor_navigable = container->document().navigable()) {
            if (auto ancestor_container = ancestor_navigable->container())
                return ancestor_navigable->has_inclusive_ancestor_with_visibility_hidden();
        }
    }
    return false;
}

UserScrollGestureHold::UserScrollGestureHold(LocalNavigable& navigable)
    : m_navigable(navigable)
{
    m_navigable->begin_user_scroll_gesture_hold({});
}

UserScrollGestureHold::~UserScrollGestureHold()
{
    if (m_navigable)
        m_navigable->end_user_scroll_gesture_hold({});
}

}
