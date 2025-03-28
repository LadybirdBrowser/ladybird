/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/LexicalPath.h>
#include <LibGfx/ImageFormats/ImageDecoder.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentLoading.h>
#include <LibWeb/HTML/HTMLHeadElement.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/NavigationParams.h>
#include <LibWeb/HTML/Parser/HTMLEncodingDetection.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/Loader/GeneratedPagesLoader.h>
#include <LibWeb/MimeSniff/Resource.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/XML/XMLDocumentBuilder.h>

namespace Web {

// Replaces a document's content with a simple error message.
static void convert_to_xml_error_document(DOM::Document& document, String error_string)
{
    auto html_element = MUST(DOM::create_element(document, HTML::TagNames::html, Namespace::HTML));
    auto body_element = MUST(DOM::create_element(document, HTML::TagNames::body, Namespace::HTML));
    MUST(html_element->append_child(body_element));
    MUST(body_element->append_child(document.realm().create<DOM::Text>(document, error_string)));
    document.remove_all_children();
    MUST(document.append_child(html_element));
}

bool build_xml_document(DOM::Document& document, ByteBuffer const& data, Optional<String> content_encoding)
{
    Optional<TextCodec::Decoder&> decoder;
    // The actual HTTP headers and other metadata, not the headers as mutated or implied by the algorithms given in this specification,
    // are the ones that must be used when determining the character encoding according to the rules given in the above specifications.
    if (content_encoding.has_value())
        decoder = TextCodec::decoder_for(*content_encoding);
    if (!decoder.has_value()) {
        auto encoding = HTML::run_encoding_sniffing_algorithm(document, data);
        decoder = TextCodec::decoder_for(encoding);
    }
    VERIFY(decoder.has_value());
    // Well-formed XML documents contain only properly encoded characters
    if (!decoder->validate(data)) {
        convert_to_xml_error_document(document, "XML Document contains improperly-encoded characters"_string);
        return false;
    }
    auto source = decoder->to_utf8(data).release_value_but_fixme_should_propagate_errors();
    XML::Parser parser(source, { .resolve_external_resource = resolve_xml_resource });
    XMLDocumentBuilder builder { document };
    auto result = parser.parse_with_listener(builder);
    return !result.is_error() && !builder.has_error();
}

// https://html.spec.whatwg.org/multipage/document-lifecycle.html#navigate-html
static WebIDL::ExceptionOr<GC::Ref<DOM::Document>> load_html_document(HTML::NavigationParams const& navigation_params)
{
    // To load an HTML document, given navigation params navigationParams:

    // 1. Let document be the result of creating and initializing a Document object given "html", "text/html", and navigationParams.
    auto document = TRY(DOM::Document::create_and_initialize(DOM::Document::Type::HTML, "text/html"_string, navigation_params));

    // 2. If document's URL is about:blank, then populate with html/head/body given document.
    // FIXME: The additional check for a non-empty body fixes issues with loading javascript urls in iframes, which
    //        default to an "about:blank" url. Is this a spec bug?
    if (document->url_string() == "about:blank"_string
        && navigation_params.response->body()->length().value_or(0) == 0) {
        TRY(document->populate_with_html_head_and_body());
        // Nothing else is added to the document, so mark it as loaded.
        HTML::HTMLParser::the_end(document);
    }

    // 3. Otherwise, create an HTML parser and associate it with the document.
    //    Each task that the networking task source places on the task queue while fetching runs must then fill the
    //    parser's input byte stream with the fetched bytes and cause the HTML parser to perform the appropriate
    //    processing of the input stream.
    //    The first task that the networking task source places on the task queue while fetching runs must process link
    //    headers given document, navigationParams's response, and "media", after the task has been processed by the
    //    HTML parser.
    //    Before any script execution occurs, the user agent must wait for scripts may run for the newly-created
    //    document to be true for document.
    //    When no more bytes are available, the user agent must queue a global task on the networking task source given
    //    document's relevant global object to have the parser to process the implied EOF character, which eventually
    //    causes a load event to be fired.
    else {
        // FIXME: Parse as we receive the document data, instead of waiting for the whole document to be fetched first.
        auto process_body = GC::create_function(document->heap(), [document, url = navigation_params.response->url().value(), mime_type = navigation_params.response->header_list()->extract_mime_type()](ByteBuffer data) {
            Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(document->heap(), [document = document, data = move(data), url = url, mime_type] {
                auto parser = HTML::HTMLParser::create_with_uncertain_encoding(document, data, mime_type);
                parser->run(url);
            }));
        });

        auto process_body_error = GC::create_function(document->heap(), [](JS::Value) {
            dbgln("FIXME: Load html page with an error if read of body failed.");
        });

        auto& realm = document->realm();
        navigation_params.response->body()->fully_read(realm, process_body, process_body_error, GC::Ref { realm.global_object() });
    }

    // 4. Return document.
    return document;
}

// https://html.spec.whatwg.org/multipage/document-lifecycle.html#read-xml
static WebIDL::ExceptionOr<GC::Ref<DOM::Document>> load_xml_document(HTML::NavigationParams const& navigation_params, MimeSniff::MimeType type)
{
    // When faced with displaying an XML file inline, provided navigation params navigationParams and a string type, user agents
    // must follow the requirements defined in XML and Namespaces in XML, XML Media Types, DOM, and other relevant specifications
    // to create and initialize a Document object document, given "xml", type, and navigationParams, and return that Document.
    // They must also create a corresponding XML parser. [XML] [XMLNS] [RFC7303] [DOM]
    //
    // Note: At the time of writing, the XML specification community had not actually yet specified how XML and the DOM interact.
    //
    // The first task that the networking task source places on the task queue while fetching runs must process link headers
    // given document, navigationParams's response, and "media", after the task has been processed by the XML parser.
    //
    // The actual HTTP headers and other metadata, not the headers as mutated or implied by the algorithms given in this
    // specification, are the ones that must be used when determining the character encoding according to the rules given in the
    // above specifications. Once the character encoding is established, the document's character encoding must be set to that
    // character encoding.
    //
    // Before any script execution occurs, the user agent must wait for scripts may run for the newly-created document to be
    // true for the newly-created Document.
    //
    // Once parsing is complete, the user agent must set document's during-loading navigation ID for WebDriver BiDi to null.
    //
    // Note: For HTML documents this is reset when parsing is complete, after firing the load event.
    //
    // Error messages from the parse process (e.g., XML namespace well-formedness errors) may be reported inline by mutating
    // the Document.

    // FIXME: Actually follow the spec! This is just the ad-hoc code we had before, modified somewhat.

    auto document = TRY(DOM::Document::create_and_initialize(DOM::Document::Type::XML, type.essence(), navigation_params));

    Optional<String> content_encoding;
    if (auto maybe_encoding = type.parameters().get("charset"sv); maybe_encoding.has_value())
        content_encoding = maybe_encoding.value();

    auto process_body = GC::create_function(document->heap(), [document, url = navigation_params.response->url().value(), content_encoding = move(content_encoding), mime = type](ByteBuffer data) {
        Optional<TextCodec::Decoder&> decoder;
        // The actual HTTP headers and other metadata, not the headers as mutated or implied by the algorithms given in this specification,
        // are the ones that must be used when determining the character encoding according to the rules given in the above specifications.
        if (content_encoding.has_value())
            decoder = TextCodec::decoder_for(*content_encoding);
        if (!decoder.has_value()) {
            auto encoding = HTML::run_encoding_sniffing_algorithm(document, data, mime);
            decoder = TextCodec::decoder_for(encoding);
        }
        VERIFY(decoder.has_value());
        // Well-formed XML documents contain only properly encoded characters
        if (!decoder->validate(data)) {
            // FIXME: Insert error message into the document.
            dbgln("XML Document contains improperly-encoded characters");
            convert_to_xml_error_document(document, "XML Document contains improperly-encoded characters"_string);

            // NOTE: This ensures that the `load` event gets fired for the frame loading this document.
            document->completely_finish_loading();
            return;
        }
        auto source = decoder->to_utf8(data);
        if (source.is_error()) {
            // FIXME: Insert error message into the document.
            dbgln("Failed to decode XML document: {}", source.error());
            convert_to_xml_error_document(document, MUST(String::formatted("Failed to decode XML document: {}", source.error())));

            // NOTE: This ensures that the `load` event gets fired for the frame loading this document.
            document->completely_finish_loading();
            return;
        }
        XML::Parser parser(source.value(), { .resolve_external_resource = resolve_xml_resource });
        XMLDocumentBuilder builder { document };
        auto result = parser.parse_with_listener(builder);
        if (result.is_error()) {
            // FIXME: Insert error message into the document.
            dbgln("Failed to parse XML document: {}", result.error());
            convert_to_xml_error_document(document, MUST(String::formatted("Failed to parse XML document: {}", result.error())));

            // NOTE: XMLDocumentBuilder ensures that the `load` event gets fired. We don't need to do anything else here.
        }
    });

    auto process_body_error = GC::create_function(document->heap(), [](JS::Value) {
        dbgln("FIXME: Load html page with an error if read of body failed.");
    });

    auto& realm = document->realm();
    navigation_params.response->body()->fully_read(realm, process_body, process_body_error, GC::Ref { realm.global_object() });

    return document;
}

// https://html.spec.whatwg.org/multipage/document-lifecycle.html#navigate-text
static WebIDL::ExceptionOr<GC::Ref<DOM::Document>> load_text_document(HTML::NavigationParams const& navigation_params, MimeSniff::MimeType type)
{
    // To load a text document, given a navigation params navigationParams and a string type:

    // 1. Let document be the result of creating and initializing a Document object given "html", type, and navigationParams.
    auto document = TRY(DOM::Document::create_and_initialize(DOM::Document::Type::HTML, type.essence(), navigation_params));

    // FIXME: 2. Set document's parser cannot change the mode flag to true.

    // 3. Set document's mode to "no-quirks".
    document->set_quirks_mode(DOM::QuirksMode::No);

    // 4. Create an HTML parser and associate it with the document. Act as if the tokenizer had emitted a start tag token
    //    with the tag name "pre" followed by a single U+000A LINE FEED (LF) character, and switch the HTML parser's tokenizer
    //    to the PLAINTEXT state. Each task that the networking task source places on the task queue while fetching runs must
    //    then fill the parser's input byte stream with the fetched bytes and cause the HTML parser to perform the appropriate
    //    processing of the input stream.
    //    document's encoding must be set to the character encoding used to decode the document during parsing.
    //    The first task that the networking task source places on the task queue while fetching runs must process link
    //    headers given document, navigationParams's response, and "media", after the task has been processed by the HTML parser.
    //    Before any script execution occurs, the user agent must wait for scripts may run for the newly-created document to be
    //    true for document.
    //    When no more bytes are available, the user agent must queue a global task on the networking task source given
    //    document's relevant global object to have the parser to process the implied EOF character, which eventually causes a
    //    load event to be fired.
    // FIXME: Parse as we receive the document data, instead of waiting for the whole document to be fetched first.
    auto process_body = GC::create_function(document->heap(), [document, url = navigation_params.response->url().value(), mime = type](ByteBuffer data) {
        auto encoding = run_encoding_sniffing_algorithm(document, data, mime);
        dbgln_if(HTML_PARSER_DEBUG, "The encoding sniffing algorithm returned encoding '{}'", encoding);

        auto parser = HTML::HTMLParser::create_for_scripting(document);
        parser->tokenizer().update_insertion_point();

        parser->tokenizer().insert_input_at_insertion_point("<pre>\n"sv);
        parser->run();

        parser->tokenizer().switch_to(HTML::HTMLTokenizer::State::PLAINTEXT);
        parser->tokenizer().insert_input_at_insertion_point(data);
        parser->tokenizer().insert_eof();
        parser->run(url);

        document->set_encoding(MUST(String::from_byte_string(encoding)));

        // 5. User agents may add content to the head element of document, e.g., linking to a style sheet, providing
        //    script, or giving the document a title.
        auto title = MUST(String::from_byte_string(LexicalPath::basename(url.to_byte_string())));
        auto title_element = MUST(DOM::create_element(document, HTML::TagNames::title, Namespace::HTML));
        MUST(document->head()->append_child(title_element));
        auto title_text = document->realm().create<DOM::Text>(document, title);
        MUST(title_element->append_child(*title_text));
    });

    auto process_body_error = GC::create_function(document->heap(), [](JS::Value) {
        dbgln("FIXME: Load html page with an error if read of body failed.");
    });

    auto& realm = document->realm();
    navigation_params.response->body()->fully_read(realm, process_body, process_body_error, GC::Ref { realm.global_object() });

    // 6. Return document.
    return document;
}

// https://html.spec.whatwg.org/multipage/document-lifecycle.html#navigate-media
static WebIDL::ExceptionOr<GC::Ref<DOM::Document>> load_media_document(HTML::NavigationParams const& navigation_params, MimeSniff::MimeType type)
{
    // To load a media document, given navigationParams and a string type:

    // 1. Let document be the result of creating and initializing a Document object given "html", type, and navigationParams.
    auto document = TRY(DOM::Document::create_and_initialize(DOM::Document::Type::HTML, type.essence(), navigation_params));

    // 2. Set document's mode to "no-quirks".
    document->set_quirks_mode(DOM::QuirksMode::No);

    // 3. Populate with html/head/body given document.
    TRY(document->populate_with_html_head_and_body());

    // 4. Append an element host element for the media, as described below, to the body element.
    // 5. Set the appropriate attribute of the element host element, as described below, to the address of the image,
    //    video, or audio resource.
    // 6. User agents may add content to the head element of document, or attributes to host element, e.g., to link
    //    to a style sheet, to provide a script, to give the document a title, or to make the media autoplay.
    auto insert_title = [](auto& document, auto title) -> WebIDL::ExceptionOr<void> {
        auto title_element = TRY(DOM::create_element(document, HTML::TagNames::title, Namespace::HTML));
        TRY(document->head()->append_child(title_element));

        auto title_text = document->realm().template create<DOM::Text>(document, title);
        TRY(title_element->append_child(*title_text));
        return {};
    };

    auto style_element = TRY(DOM::create_element(document, HTML::TagNames::style, Namespace::HTML));
    style_element->set_text_content(R"~~~(
        :root {
            background-color: #222;
        }
        img, video, audio {
            position: absolute;
            inset: 0;
            max-width: 100vw;
            max-height: 100vh;
            margin: auto;
        }
        img {
            background-color: #fff;
        }
    )~~~"_string);
    TRY(document->head()->append_child(style_element));

    auto url_string = document->url_string();
    if (type.is_image()) {
        auto img_element = TRY(DOM::create_element(document, HTML::TagNames::img, Namespace::HTML));
        TRY(img_element->set_attribute(HTML::AttributeNames::src, url_string));
        TRY(document->body()->append_child(img_element));
        TRY(insert_title(document, MUST(String::from_byte_string(LexicalPath::basename(url_string.to_byte_string())))));

    } else if (type.type() == "video"sv) {
        auto video_element = TRY(DOM::create_element(document, HTML::TagNames::video, Namespace::HTML));
        TRY(video_element->set_attribute(HTML::AttributeNames::src, url_string));
        TRY(video_element->set_attribute(HTML::AttributeNames::autoplay, String {}));
        TRY(video_element->set_attribute(HTML::AttributeNames::controls, String {}));
        TRY(document->body()->append_child(video_element));
        TRY(insert_title(document, MUST(String::from_byte_string(LexicalPath::basename(url_string.to_byte_string())))));

    } else if (type.type() == "audio"sv) {
        auto audio_element = TRY(DOM::create_element(document, HTML::TagNames::audio, Namespace::HTML));
        TRY(audio_element->set_attribute(HTML::AttributeNames::src, url_string));
        TRY(audio_element->set_attribute(HTML::AttributeNames::autoplay, String {}));
        TRY(audio_element->set_attribute(HTML::AttributeNames::controls, String {}));
        TRY(document->body()->append_child(audio_element));
        TRY(insert_title(document, MUST(String::from_byte_string(LexicalPath::basename(url_string.to_byte_string())))));

    } else {
        // FIXME: According to https://mimesniff.spec.whatwg.org/#audio-or-video-mime-type we might have to deal with
        //        "application/ogg" and figure out whether it's audio or video.
        VERIFY_NOT_REACHED();
    }

    // FIXME: 7. Process link headers given document, navigationParams's response, and "media".

    // 8. Act as if the user agent had stopped parsing document.
    // FIXME: We should not need to force the media file to load before saying that parsing has completed!
    //        However, if we don't, then we get stuck in HTMLParser::the_end() waiting for the media file to load, which
    //        never happens.
    auto& realm = document->realm();
    navigation_params.response->body()->fully_read(
        realm,
        GC::create_function(document->heap(), [document](ByteBuffer) { HTML::HTMLParser::the_end(document); }),
        GC::create_function(document->heap(), [](JS::Value) {}),
        GC::Ref { realm.global_object() });

    // 9. Return document.
    return document;

    // The element host element to create for the media is the element given in the table below in the second cell of
    // the row whose first cell describes the media. The appropriate attribute to set is the one given by the third cell
    // in that same row.
    // Type of media | Element for the media | Appropriate attribute
    // -------------------------------------------------------------
    // Image         | img                   | src
    // Video         | video                 | src
    // Audio         | audio                 | src

    // Before any script execution occurs, the user agent must wait for scripts may run for the newly-created document to
    // be true for the Document.
}

bool can_load_document_with_type(MimeSniff::MimeType const& type)
{
    if (type.is_html())
        return true;
    if (type.is_xml())
        return true;
    if (type.is_javascript()
        || type.is_json()
        || type.essence() == "text/css"_string
        || type.essence() == "text/plain"_string
        || type.essence() == "text/vtt"_string) {
        return true;
    }
    if (type.essence() == "multipart/x-mixed-replace"_string)
        return true;
    if (type.is_image() || type.is_audio_or_video())
        return true;
    if (type.essence() == "application/pdf"_string || type.essence() == "text/pdf"_string)
        return true;
    if (type.essence() == "text/markdown"sv)
        return true;
    return false;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#loading-a-document
GC::Ptr<DOM::Document> load_document(HTML::NavigationParams const& navigation_params)
{
    // To load a document given navigation params navigationParams, source snapshot params sourceSnapshotParams,
    // and origin initiatorOrigin, perform the following steps. They return a Document or null.

    // 1. Let type be the computed type of navigationParams's response.
    auto supplied_type = navigation_params.response->header_list()->extract_mime_type();
    auto type = MimeSniff::Resource::sniff(
        navigation_params.response->body()->source().visit(
            [](Empty) { return ReadonlyBytes {}; },
            [](ByteBuffer const& buffer) { return ReadonlyBytes { buffer }; },
            [](GC::Root<FileAPI::Blob> const& blob) { return blob->raw_bytes(); }),
        MimeSniff::SniffingConfiguration {
            .sniffing_context = MimeSniff::SniffingContext::Browsing,
            .supplied_type = move(supplied_type) });

    VERIFY(navigation_params.response->body());

    // 2. If the user agent has been configured to process resources of the given type using some mechanism other than
    //    rendering the content in a navigable, then skip this step.
    //    Otherwise, if the type is one of the following types:

    // -> an HTML MIME type
    if (type.is_html()) {
        // Return the result of loading an HTML document, given navigationParams.
        return load_html_document(navigation_params).release_value_but_fixme_should_propagate_errors();
    }

    // -> an XML MIME type that is not an explicitly supported XML MIME type
    //    FIXME: that is not an explicitly supported XML MIME type
    if (type.is_xml()) {
        // Return the result of loading an XML document given navigationParams and type.
        return load_xml_document(navigation_params, type).release_value_but_fixme_should_propagate_errors();
    }

    // -> a JavaScript MIME type
    // -> a JSON MIME type that is not an explicitly supported JSON MIME type
    // -> "text/css"
    // -> "text/plain"
    // -> "text/vtt"
    if (type.is_javascript()
        || type.is_json()
        || type.essence() == "text/css"_string
        || type.essence() == "text/plain"_string
        || type.essence() == "text/vtt"_string) {
        // Return the result of loading a text document given navigationParams and type.
        return load_text_document(navigation_params, type).release_value_but_fixme_should_propagate_errors();
    }

    // -> "multipart/x-mixed-replace"
    if (type.essence() == "multipart/x-mixed-replace"_string) {
        // FIXME: Return the result of loading a multipart/x-mixed-replace document, given navigationParams,
        //        sourceSnapshotParams, and initiatorOrigin.
    }

    // -> A supported image, video, or audio type
    if (type.is_image()
        || type.is_audio_or_video()) {
        // Return the result of loading a media document given navigationParams and type.
        return load_media_document(navigation_params, type).release_value_but_fixme_should_propagate_errors();
    }

    // -> "application/pdf"
    // -> "text/pdf"
    if (type.essence() == "application/pdf"_string
        || type.essence() == "text/pdf"_string) {
        // FIXME: If the user agent's PDF viewer supported is true, return the result of creating a document for inline
        //        content that doesn't have a DOM given navigationParams's navigable, navigationParams's id,
        //        navigationParams's navigation timing type, and navigationParams's user involvement.
    }

    // Otherwise, proceed onward.

    // FIXME: 3. If, given type, the new resource is to be handled by displaying some sort of inline content, e.g., a
    //    native rendering of the content or an error message because the specified type is not supported, then
    //    return the result of creating a document for inline content that doesn't have a DOM given navigationParams's
    //    navigable, navigationParams's id, navigationParams's navigation timing type, and navigationParams's user involvement.

    // FIXME: 4. Otherwise, the document's type is such that the resource will not affect navigationParams's navigable,
    //        e.g., because the resource is to be handed to an external application or because it is an unknown type
    //        that will be processed by handle as a download. Hand-off to external software given navigationParams's
    //        response, navigationParams's navigable, navigationParams's final sandboxing flag set,
    //        sourceSnapshotParams's has transient activation, and initiatorOrigin.

    // 5. Return null.
    return nullptr;
}

}
