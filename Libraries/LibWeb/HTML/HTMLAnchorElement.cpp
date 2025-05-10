/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <LibURL/Parser.h>
#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/Bindings/HTMLAnchorElementPrototype.h>
#include <LibWeb/DOM/DOMTokenList.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/HTMLAnchorElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/Navigation.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/PixelUnits.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/ReferrerPolicy/ReferrerPolicy.h>
#include <LibWeb/UIEvents/MouseEvent.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLAnchorElement);

HTMLAnchorElement::HTMLAnchorElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLAnchorElement::~HTMLAnchorElement() = default;

void HTMLAnchorElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLAnchorElement);
    Base::initialize(realm);
}

void HTMLAnchorElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_rel_list);
}

void HTMLAnchorElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (name == HTML::AttributeNames::href) {
        set_the_url();
    } else if (name == HTML::AttributeNames::rel) {
        if (m_rel_list)
            m_rel_list->associated_attribute_changed(value.value_or(String {}));
    }
}

Optional<String> HTMLAnchorElement::hyperlink_element_utils_href() const
{
    return attribute(HTML::AttributeNames::href);
}

WebIDL::ExceptionOr<void> HTMLAnchorElement::set_hyperlink_element_utils_href(String href)
{
    return set_attribute(HTML::AttributeNames::href, move(href));
}

Optional<String> HTMLAnchorElement::hyperlink_element_utils_referrerpolicy() const
{
    return attribute(HTML::AttributeNames::referrerpolicy);
}

bool HTMLAnchorElement::has_activation_behavior() const
{
    return true;
}

// https://html.spec.whatwg.org/multipage/links.html#links-created-by-a-and-area-elements
void HTMLAnchorElement::activation_behavior(Web::DOM::Event const& event)
{
    // The activation behavior of an a or area element element given an event event is:

    // 1. If element has no href attribute, then return.
    if (href().is_empty())
        return;

    // AD-HOC: Do not activate the element for clicks with the ctrl/cmd modifier present. This lets
    //         the browser process open the link in a new tab.
    if (is<UIEvents::MouseEvent>(event)) {
        auto const& mouse_event = static_cast<UIEvents::MouseEvent const&>(event);
        if (mouse_event.platform_ctrl_key())
            return;
    }

    // 2. Let hyperlinkSuffix be null.
    Optional<String> hyperlink_suffix {};

    // 3. If element is an a element, and event's target is an img with an ismap attribute specified, then:
    if (event.target() && is<HTMLImageElement>(*event.target()) && static_cast<HTMLImageElement const&>(*event.target()).has_attribute(AttributeNames::ismap)) {
        // 1. Let x and y be 0.
        CSSPixels x { 0 };
        CSSPixels y { 0 };

        // 2. If event's isTrusted attribute is initialized to true, then set x to the distance in CSS pixels from the left edge of the image
        //    to the location of the click, and set y to the distance in CSS pixels from the top edge of the image to the location of the click.
        if (event.is_trusted() && is<UIEvents::MouseEvent>(event)) {
            auto const& mouse_event = static_cast<UIEvents::MouseEvent const&>(event);
            x = CSSPixels { mouse_event.offset_x() };
            y = CSSPixels { mouse_event.offset_y() };
        }

        // 3. If x is negative, set x to 0.
        x = max(x, 0);

        // 4. If y is negative, set y to 0.
        y = max(y, 0);

        // 5. Set hyperlinkSuffix to the concatenation of U+003F (?), the value of x expressed as a base-ten integer using ASCII digits,
        //    U+002C (,), and the value of y expressed as a base-ten integer using ASCII digits.
        hyperlink_suffix = MUST(String::formatted("?{},{}", x.to_int(), y.to_int()));
    }

    // 4. Let userInvolvement be event's user navigation involvement.
    auto user_involvement = user_navigation_involvement(event);

    // 5. If the user has expressed a preference to download the hyperlink, then set userInvolvement to "browser UI".
    // NOTE: That is, if the user has expressed a specific preference for downloading, this no longer counts as merely "activation".
    if (has_download_preference())
        user_involvement = UserNavigationInvolvement::BrowserUI;

    // 6. If element has a download attribute, or if the user has expressed a preference to download the
    //    hyperlink, then download the hyperlink created by element with hyperlinkSuffix set to hyperlinkSuffix and
    //    userInvolvement set to userInvolvement.
    if (has_attribute(AttributeNames::download)) {
        download_the_hyperlink(hyperlink_suffix, user_involvement);
    }

    // 7. Otherwise, follow the hyperlink created by element with hyperlinkSuffix set to hyperlinkSuffix and userInvolvement set to userInvolvement.
    else {
        follow_the_hyperlink(hyperlink_suffix, user_involvement);
    }
}

bool HTMLAnchorElement::has_download_preference() const
{
    return has_attribute(HTML::AttributeNames::download);
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-tabindex
i32 HTMLAnchorElement::default_tab_index_value() const
{
    // See the base function for the spec comments.
    return 0;
}

Optional<ARIA::Role> HTMLAnchorElement::default_role() const
{
    // https://www.w3.org/TR/html-aria/#el-a-no-href
    if (!href().is_empty())
        return ARIA::Role::link;
    // https://www.w3.org/TR/html-aria/#el-a
    return ARIA::Role::generic;
}

// https://html.spec.whatwg.org/multipage/text-level-semantics.html#dom-a-rellist
GC::Ref<DOM::DOMTokenList> HTMLAnchorElement::rel_list()
{
    // The IDL attribute relList must reflect the rel content attribute.
    if (!m_rel_list)
        m_rel_list = DOM::DOMTokenList::create(*this, HTML::AttributeNames::rel);
    return *m_rel_list;
}

// https://html.spec.whatwg.org/multipage/text-level-semantics.html#dom-a-text
String HTMLAnchorElement::text() const
{
    // The text attribute's getter must return this element's descendant text content.
    return descendant_text_content();
}

// https://html.spec.whatwg.org/multipage/text-level-semantics.html#dom-a-text
void HTMLAnchorElement::set_text(String const& text)
{
    // The text attribute's setter must string replace all with the given value within this element.
    string_replace_all(text);
}

// https://html.spec.whatwg.org/multipage/links.html#downloading-hyperlinks
void HTMLAnchorElement::download_the_hyperlink(Optional<String> hyperlink_suffix = {}, UserNavigationInvolvement user_involvement = UserNavigationInvolvement::None)
{
    // 1. If subject cannot navigate, then return.
    if (HTMLElement::cannot_navigate())
        return;

    // 2. If subject's node document's active sandboxing flag set has the sandboxed downloads browsing context flag set, then return.
    if (has_flag(document().active_sandboxing_flag_set(), SandboxingFlagSet::SandboxedDownloads))
        return;

    // 3. Let urlString be the result of encoding-parsing-and-serializing a URL given subject's href attribute value, relative to subject's node document.
    auto url_string = document().encoding_parse_and_serialize_url(href());

    // 4. If urlString is failure, then return.
    if (url_string->is_empty())
        return;

    // 5. If hyperlinkSuffix is non-null, then append it to urlString.
    if (hyperlink_suffix.has_value())
        url_string = MUST(String::formatted("{}{}", url_string, *hyperlink_suffix));

    auto url = URL::Parser::basic_parse(url_string.value());
    VERIFY(url.has_value());

    // 6. If userInvolvement is not "browser UI", then:
    if (user_involvement != UserNavigationInvolvement::BrowserUI) {
        // 1. Assert: subject has a download attribute.
        VERIFY(has_attribute(AttributeNames::download));

        // 2. Let navigation be subject's relevant global object's navigation API.
        auto navigation = as<Window>(relevant_global_object(*this)).navigation();

        // 3. Let filename be the value of subject's download attribute.
        auto filename = get_attribute_value(AttributeNames::download);

        // 4. Let continue be the result of firing a download request navigate event at navigation with destinationURL set to urlString,
        //    userInvolvement set to userInvolvement, sourceElement set to subject, and filename set to filename.
        auto continue_ = navigation->fire_a_download_request_navigate_event(url.value(), user_involvement, this, filename);

        // 5. If continue is false, then return.
        if (!continue_)
            return;
    }

    // AD-HOC: Get the download attribute
    auto download_attribute = get_attribute(AttributeNames::download);

    // 7. Run these steps in parallel:
    Platform::EventLoopPlugin::the()
        .deferred_invoke(GC::create_function(heap(), [this, url, download_attribute] {
            // FIXME: 1. Optionally, the user agent may abort these steps, if it believes doing so would safeguard the user from a potentially hostile download.

            // 2. Let request be a new request whose URL is urlString, client is entry settings object, initiator is "download",
            //    destination is the empty string, and whose synchronous flag and use-URL-credentials flag are set.
            //    NOTE: Fetch requests no longer have a synchronous flag, see https://github.com/whatwg/fetch/pull/1165
            auto request = Fetch::Infrastructure::Request::create(vm());
            request->set_url(url.value());
            request->set_client(&entry_settings_object());
            request->set_initiator(Fetch::Infrastructure::Request::Initiator::Download);
            request->set_destination({});
            request->set_use_url_credentials(true);

            // 3. Handle as a download the result of fetching request.
            Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input {};
            fetch_algorithms_input.process_response = [this, download_attribute](GC::Ref<Fetch::Infrastructure::Response> response) {
                (void)handle_as_a_download(response, document().page(), document(), download_attribute);
            };
            (void)Fetch::Fetching::fetch(realm(), request, Fetch::Infrastructure::FetchAlgorithms::create(vm(), move(fetch_algorithms_input)));
        }));
}

// https://html.spec.whatwg.org/multipage/links.html#handle-as-a-download
String handle_as_a_download(GC::Ref<Fetch::Infrastructure::Response> response, Page& page, Optional<DOM::Document&> document, Optional<String> download_attribute)
{
    // 1. Let suggestedFilename be the result of getting the suggested filename for response.
    auto suggested_filename = get_the_suggested_filename(response, page, document, download_attribute);

    // 2. Provide the user with a way to save response for later use. If the user agent needs a filename, it should use suggestedFilename. Report any problems downloading the file to the user.
    // FIXME: Report any problems downloading the file to the user.
    auto bytes = response->body()->source().visit([](Empty) { return ByteBuffer {}; }, [](ByteBuffer const& buffer) { return buffer; }, [](GC::Root<FileAPI::Blob> const& blob) { return MUST(ByteBuffer::copy(blob->raw_bytes())); });
    page.did_request_download(suggested_filename, bytes);

    // 3. Return suggestedFilename.
    return suggested_filename;
}

// https://html.spec.whatwg.org/multipage/links.html#getting-the-suggested-filename
String get_the_suggested_filename(GC::Ref<Fetch::Infrastructure::Response> response, Page& page, Optional<DOM::Document&> document, Optional<String&> download_attribute)
{
    // 1. Let filename be the undefined value.
    Optional<String> filename;

    // FIXME: 2. If response has a `Content-Disposition` header, that header specifies the attachment disposition type, and the header includes
    //    filename information, then let filename have the value specified by the header, and jump to the step labeled sanitize below. [RFC6266]

    // 3. Let interface origin be the origin of the Document in which the download or navigate action resulting in the download was initiated, if any.
    Optional<URL::Origin> interface_origin;
    if (document.has_value())
        interface_origin = document->origin();

    // 4. Let response origin be the origin of the URL of response, unless that URL's scheme component is data, in which case let response origin be the same as the interface origin, if any.
    Optional<URL::Origin> response_origin;
    if (response->url()->scheme() == "data") {
        response_origin = interface_origin;
    } else {
        response_origin = response->url()->origin();
    }

    // 5. If there is no interface origin, then let trusted operation be true. Otherwise, let trusted operation be true if response origin is the same origin as interface origin, and false otherwise.
    bool trusted_operation;
    if (!interface_origin.has_value()) {
        trusted_operation = true;
    } else {
        trusted_operation = response_origin == interface_origin;
    }

    // FIXME: 6. If trusted operation is true and response has a `Content-Disposition` header and that header includes filename information,
    //    then let filename have the value specified by the header, and jump to the step labeled sanitize below. [RFC6266]

    // 7. If the download was not initiated from a hyperlink created by an a or area element, or if the element of the hyperlink from which it
    //    was initiated did not have a download attribute when the download was initiated, or if there was such an attribute but its value when
    //    the download was initiated was the empty string, then jump to the step labeled no proposed filename.
    if (!download_attribute.has_value() || download_attribute->is_empty()) {
        goto no_proposed_filename;
    }

    // 8. Let proposed filename have the value of the download attribute of the element of the hyperlink that initiated the download at the time the download was initiated.
    // 9. If trusted operation is true, let filename have the value of proposed filename, and jump to the step labeled sanitize below.
    if (trusted_operation) {
        filename = download_attribute.value();
        goto sanitize;
    }

    // FIXME: 10. If response has a `Content-Disposition` header and that header specifies the attachment disposition type, let filename have the value
    //     of proposed filename, and jump to the step labeled sanitize below. [RFC6266]

no_proposed_filename: {
    // 11. No proposed filename: If trusted operation is true, or if the user indicated a preference for having the response in question downloaded,
    //     let filename have a value derived from the URL of response in an implementation-defined manner, and jump to the step labeled sanitize below.
    // FIXME: If the user indicated a preference for having the response in question downloaded.
    if (trusted_operation) {
        auto path = response->url()->serialize_path();
        filename = MUST(String::from_byte_string(AK::LexicalPath::basename(path.to_byte_string())));
        goto sanitize;
    }

    // 12. Let filename be set to the user's preferred filename or to a filename selected by the user agent, and jump to the step labeled sanitize below.
    // FIXME: Users preferred filename.
    filename = "download"_string;
}

sanitize: {
    // 13. Sanitize: Optionally, allow the user to influence filename. For example, a user agent could prompt the user for a filename, potentially
    //     providing the value of filename as determined above as a default value.
    filename = page.did_request_prompt("Please enter a filename:"_string, filename.value());

    // FIXME: 14. Adjust filename to be suitable for the local file system.

    // FIXME: 15. If the platform conventions do not in any way use extensions to determine the types of file on the file system, then return filename as the filename.

    // 16. Let claimed type be the type given by response's Content-Type metadata, if any is known. Let named type be the type given by filename's extension,
    //     if any is known. For the purposes of this step, a type is a mapping of a MIME type to an extension.
    String claimed_type = ""_string;
    if (auto content_type = response->header_list()->extract_mime_type(); content_type.has_value()) {
        // FIXME: Map the content_type to an extension.
    }
    auto parsed_filename = AK::LexicalPath(filename->to_byte_string());
    auto named_type = parsed_filename.extension();

    // FIXME: 17. If named type is consistent with the user's preferences (e.g., because the value of filename was determined by prompting the user), then return filename as the filename.

    // 18. If claimed type and named type are the same type (i.e., the type given by response's Content-Type metadata is consistent with the type given by filename's extension), then return filename as the filename.
    if (claimed_type == named_type) {
        return filename.value();
    }

    // 19. If the claimed type is known, then alter filename to add an extension corresponding to claimed type.
    //     Otherwise, if named type is known to be potentially dangerous (e.g.it will be treated by the platform conventions as a native executable, shell script,
    //     HTML application, or executable-macro-capable document) then optionally alter filename to add a known-safe extension (e.g.".txt").
    // FIXME: Replace dangerous filetypes.
    if (!claimed_type.is_empty()) {
        filename = MUST(String::formatted("{}.{}", parsed_filename.title(), claimed_type));
    }

    // 20. Return filename as the filename.
    return filename.value();
}
}

}
