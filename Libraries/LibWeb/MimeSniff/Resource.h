/*
 * Copyright (c) 2023, Kemal Zebari <kemalzebra@gmail.com>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/MimeSniff/MimeType.h>

namespace Web::MimeSniff {

enum class SniffingContext {
    None,
    Browsing,
    Image,
    AudioOrVideo,
    Font,

    // Non-standard but used in cases where the spec expects us to only run
    // https://mimesniff.spec.whatwg.org/#sniffing-a-mislabeled-binary-resource
    TextOrBinary,
};

struct SniffingConfiguration {
    SniffingContext sniffing_context { SniffingContext::None };
    StringView scheme { ""sv };
    Optional<MimeType> supplied_type = {};
    bool no_sniff { false };
};

// https://mimesniff.spec.whatwg.org/#resource
class Resource {
public:
    static Resource create(ReadonlyBytes data, SniffingConfiguration configuration = {});
    static MimeType sniff(ReadonlyBytes data, SniffingConfiguration configuration = {});

    ~Resource();

    MimeType const& computed_mime_type() const { return m_computed_mime_type; }
    ReadonlyBytes resource_header() const { return m_resource_header; }

private:
    Resource(ReadonlyBytes data, bool no_sniff, MimeType&& default_computed_mime_type);

    void read_the_resource_header(ReadonlyBytes data);
    void supplied_mime_type_detection_algorithm(StringView scheme, Optional<MimeType> supplied_type);
    void mime_type_sniffing_algorithm();

    void rules_for_distinguishing_if_a_resource_is_text_or_binary();

    void context_specific_sniffing_algorithm(SniffingContext sniffing_context);
    void rules_for_sniffing_images_specifically();
    void rules_for_sniffing_audio_or_video_specifically();
    void rules_for_sniffing_fonts_specifically();

    // https://mimesniff.spec.whatwg.org/#supplied-mime-type
    // A supplied MIME type, the MIME type determined by the supplied MIME type detection algorithm.
    Optional<MimeType> m_supplied_mime_type;

    // https://mimesniff.spec.whatwg.org/#check-for-apache-bug-flag
    // A check-for-apache-bug flag, which defaults to unset.
    bool m_check_for_apache_bug_flag { false };

    // https://mimesniff.spec.whatwg.org/#no-sniff-flag
    // A no-sniff flag, which defaults to set if the user agent does not wish to perform sniffing on the resource and unset otherwise.
    bool m_no_sniff { false };

    // https://mimesniff.spec.whatwg.org/#computed-mime-type
    // A computed MIME type, the MIME type determined by the MIME type sniffing algorithm.
    MimeType m_computed_mime_type;

    // https://mimesniff.spec.whatwg.org/#resource-header
    // A resource header is the byte sequence at the beginning of a resource, as determined by reading the resource header.
    ByteBuffer m_resource_header;
};

}
