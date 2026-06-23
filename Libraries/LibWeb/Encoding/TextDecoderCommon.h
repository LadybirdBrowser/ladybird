/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/OwnPtr.h>
#include <LibJS/Forward.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Encoding {

struct EndOfQueue {
};

class TextDecoderOutputQueue {
public:
    ErrorOr<void> push(String);
    ErrorOr<String> serialize();

private:
    Optional<String> m_single_output;
    StringBuilder m_builder;
    bool m_has_builder { false };
};

// https://encoding.spec.whatwg.org/#textdecodercommon
class TextDecoderCommonMixin {
public:
    // https://encoding.spec.whatwg.org/#dom-textdecoder-encoding
    FlyString const& encoding() const { return m_encoding; }

    // https://encoding.spec.whatwg.org/#dom-textdecoder-fatal
    bool fatal() const { return m_error_mode == TextCodec::ErrorMode::Fatal; }

    // https://encoding.spec.whatwg.org/#dom-textdecoder-ignorebom
    bool ignore_bom() const { return m_ignore_bom; }

protected:
    TextDecoderCommonMixin(FlyString encoding, TextCodec::ErrorMode error_mode, bool ignore_bom);

    void set_decoder_to_new_instance_of_encoding_decoder();
    WebIDL::ExceptionOr<void> process_an_item(JS::VM&, ReadonlyBytes item, TextDecoderOutputQueue& output);
    WebIDL::ExceptionOr<void> process_an_item(JS::VM&, EndOfQueue, TextDecoderOutputQueue& output);
    WebIDL::ExceptionOr<String> serialize_io_queue(JS::VM&, TextDecoderOutputQueue& output);

    // https://encoding.spec.whatwg.org/#textdecodercommon-decoder
    OwnPtr<TextCodec::StreamingDecoder> m_decoder;

    // https://encoding.spec.whatwg.org/#textdecoder-encoding
    FlyString m_encoding;

    // https://encoding.spec.whatwg.org/#textdecoder-error-mode
    TextCodec::ErrorMode m_error_mode { TextCodec::ErrorMode::Replacement };

    // https://encoding.spec.whatwg.org/#textdecoder-ignore-bom-flag
    bool m_ignore_bom { false };
};

}
