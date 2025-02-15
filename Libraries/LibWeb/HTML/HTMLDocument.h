/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Document.h>

namespace Web::HTML {

// NOTE: This class is not currently in the specifications but it *is* implemented by all major browsers.
//       There is discussion about bringing it back:
//       https://github.com/whatwg/html/issues/4792
//       https://github.com/whatwg/dom/issues/221
class HTMLDocument final : public DOM::Document {
    WEB_PLATFORM_OBJECT(HTMLDocument, DOM::Document);
    GC_DECLARE_ALLOCATOR(HTMLDocument);

public:
    virtual ~HTMLDocument() override;

    [[nodiscard]] static GC::Ref<HTMLDocument> create(JS::Realm&, URL::URL const& url = URL::about_blank());
    WebIDL::ExceptionOr<GC::Ref<HTMLDocument>> construct_impl(JS::Realm&);

private:
    virtual void initialize(JS::Realm&) override;

    HTMLDocument(JS::Realm&, URL::URL const&);
};

}
