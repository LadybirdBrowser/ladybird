/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebCommon/HTML/BrowsingContext.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/urls-and-fetching.html#matches-about:blank
bool url_matches_about_blank(URL::URL const& url)
{
    // A URL matches about:blank if its scheme is "about", its path contains a single string "blank", its username and password are the empty string, and its host is null.
    return url.scheme() == "about"sv
        && url.path_segment_count() == 1 && url.path_segments().first() == "blank"sv
        && url.username().is_empty()
        && url.password().is_empty()
        && !url.host().has_value();
}

// https://html.spec.whatwg.org/multipage/urls-and-fetching.html#matches-about:srcdoc
bool url_matches_about_srcdoc(URL::URL const& url)
{
    // A URL matches about:srcdoc if its scheme is "about", its path contains a single string "srcdoc", its query is null, its username and password are the empty string, and its host is null.
    return url.scheme() == "about"sv
        && url.path_segment_count() == 1 && url.path_segments().first() == "srcdoc"sv
        && !url.query().has_value()
        && url.username().is_empty()
        && url.password().is_empty()
        && !url.host().has_value();
}

// NB: Steps 2 to 5 of determining the origin, so that step 1 can ask for the origin this document would have had
//     if it were not sandboxed.
static URL::Origin determine_the_origin_ignoring_sandboxing(Optional<URL::URL const&> url, Optional<URL::Origin> source_origin)
{
    // 2. If url is null, then return a new opaque origin.
    if (!url.has_value()) {
        return URL::Origin::create_opaque();
    }

    // 3. If url is about:srcdoc, then:
    if (url == URL::about_srcdoc()) {
        // 1. Assert: sourceOrigin is non-null.
        VERIFY(source_origin.has_value());

        // 2. Return sourceOrigin.
        return source_origin.release_value();
    }

    // 4. If url matches about:blank and sourceOrigin is non-null, then return sourceOrigin.
    if (url_matches_about_blank(*url) && source_origin.has_value())
        return source_origin.release_value();

    // 5. Return url's origin.
    return url->origin();
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#determining-the-origin
URL::Origin determine_the_origin(Optional<URL::URL const&> url, SandboxingFlagSet sandbox_flags, Optional<URL::Origin> source_origin)
{
    // 1. If sandboxFlags has its sandboxed origin browsing context flag set, then return a new opaque origin.
    if (has_flag(sandbox_flags, SandboxingFlagSet::SandboxedOrigin)) {
        // AD-HOC: A file: URL's origin is opaque, tagged as such so that a document from the local file system stays
        //         apart from every other opaque origin. Mark the origin sandboxing hands out when the document would
        //         otherwise have had one, so that sandboxing a local document does not also cost it the files sitting
        //         next to it.
        auto type = determine_the_origin_ignoring_sandboxing(url, move(source_origin)).is_file_origin()
            ? URL::Origin::OpaqueData::Type::SandboxedFile
            : URL::Origin::OpaqueData::Type::Standard;
        return URL::Origin::create_opaque(type);
    }

    return determine_the_origin_ignoring_sandboxing(url, move(source_origin));
}

}
