/*
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::FileAPI {

// https://w3c.github.io/FileAPI/#dfn-filereader
class FileReader : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(FileReader, DOM::EventTarget);
    JS_DECLARE_ALLOCATOR(FileReader);

public:
    using Result = Variant<Empty, String, JS::Handle<JS::ArrayBuffer>>;

    virtual ~FileReader() override;

    [[nodiscard]] static JS::NonnullGCPtr<FileReader> create(JS::Realm&);
    static JS::NonnullGCPtr<FileReader> construct_impl(JS::Realm&);

    // async read methods
    WebIDL::ExceptionOr<void> read_as_array_buffer(Blob&);
    WebIDL::ExceptionOr<void> read_as_binary_string(Blob&);
    WebIDL::ExceptionOr<void> read_as_text(Blob&, Optional<String> const& encoding = {});
    WebIDL::ExceptionOr<void> read_as_data_url(Blob&);

    void abort();

    // states
    enum class State : u16 {
        // The FileReader object has been constructed, and there are no pending reads. None of the read methods have been called.
        // This is the default state of a newly minted FileReader object, until one of the read methods have been called on it.
        Empty = 0,

        // A File or Blob is being read. One of the read methods is being processed, and no error has occurred during the read.
        Loading = 1,

        // The entire File or Blob has been read into memory, OR a file read error occurred, OR the read was aborted using abort().
        // The FileReader is no longer reading a File or Blob.
        // If readyState is set to DONE it means at least one of the read methods have been called on this FileReader.
        Done = 2,
    };

    // https://w3c.github.io/FileAPI/#dom-filereader-readystate
    State ready_state() const { return m_state; }

    // File or Blob data

    // https://w3c.github.io/FileAPI/#dom-filereader-result
    Result result() const { return m_result; }

    // https://w3c.github.io/FileAPI/#dom-filereader-error
    JS::GCPtr<WebIDL::DOMException> error() const { return m_error; }

    // event handler attributes
    void set_onloadstart(WebIDL::CallbackType*);
    WebIDL::CallbackType* onloadstart();

    void set_onprogress(WebIDL::CallbackType*);
    WebIDL::CallbackType* onprogress();

    void set_onload(WebIDL::CallbackType*);
    WebIDL::CallbackType* onload();

    void set_onabort(WebIDL::CallbackType*);
    WebIDL::CallbackType* onabort();

    void set_onerror(WebIDL::CallbackType*);
    WebIDL::CallbackType* onerror();

    void set_onloadend(WebIDL::CallbackType*);
    WebIDL::CallbackType* onloadend();

protected:
    FileReader(JS::Realm&, ByteBuffer);

    virtual void initialize(JS::Realm&) override;

    virtual void visit_edges(JS::Cell::Visitor&) override;

private:
    explicit FileReader(JS::Realm&);

    enum class Type {
        ArrayBuffer,
        BinaryString,
        Text,
        DataURL,
    };

    WebIDL::ExceptionOr<void> read_operation(Blob&, Type, Optional<String> const& encoding_name = {});

    static WebIDL::ExceptionOr<Result> blob_package_data(JS::Realm& realm, ByteBuffer, FileReader::Type type, Optional<String> const&, Optional<String> const& encoding_name);

    // A FileReader has an associated state, that is "empty", "loading", or "done". It is initially "empty".
    // https://w3c.github.io/FileAPI/#filereader-state
    State m_state { State::Empty };

    // A FileReader has an associated result (null, a DOMString or an ArrayBuffer). It is initially null.
    // https://w3c.github.io/FileAPI/#filereader-result
    Result m_result;

    // A FileReader has an associated error (null or a DOMException). It is initially null.
    // https://w3c.github.io/FileAPI/#filereader-error
    JS::GCPtr<WebIDL::DOMException> m_error;
};

}
