/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibGC/ConservativeHashMap.h>
#include <LibGC/ConservativeVector.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/HighResolutionTime/DOMHighResTimeStamp.h>
#include <LibWebCommon/PixelUnits.h>

namespace Web::PaintTiming {

// https://html.spec.whatwg.org/multipage/images.html#image-request
// NB: What paint timing needs to know about an image request.
struct ImageRequestInfo {
    Utf16String url;

    // https://html.spec.whatwg.org/multipage/images.html#img-req-data
    // NB: The natural dimensions of the image data, which an image request has once it is available.
    Optional<CSSPixelSize> natural_size;
};

// https://w3c.github.io/paint-timing/#pending-image-record
struct PendingImageRecord {
    // element, an Element
    GC::Ref<DOM::Element> element;

    // request, an image request
    ImageRequestInfo request;

    // loadTime, a DOMHighResTimeStamp
    HighResolutionTime::DOMHighResTimeStamp load_time { 0 };
};

// https://w3c.github.io/largest-contentful-paint/#largest-contentful-paint-candidate
struct LargestContentfulPaintCandidate {
    // element, an element
    GC::Ref<DOM::Element> element;

    // size, a number
    double size { 0 };

    // width, a number
    double width { 0 };

    // height, a number
    double height { 0 };

    // request, a Request or null
    Optional<ImageRequestInfo> request;

    // loadTime, a number
    HighResolutionTime::DOMHighResTimeStamp load_time { 0 };
};

// NB: The elements that get a text node in their set of owned text nodes for the first time as a frame is painted,
//     with those text nodes.
using PaintedTextNodes = GC::OrderedConservativeHashMap<GC::Ref<DOM::Element>, GC::ConservativeVector<GC::Ref<DOM::Text>>>;

struct DocumentPaintTimingState {
    // https://w3c.github.io/paint-timing/#set-of-previously-reported-paints
    // Each Document has a set of previously reported paints, which is an ordered set of strings, initially empty.
    OrderedHashTable<Utf16String> set_of_previously_reported_paints;

    // https://w3c.github.io/paint-timing/#images-pending-rendering
    // Each Document has an images pending rendering, which is a list of pending image records, initially empty.
    Vector<PendingImageRecord> images_pending_rendering;

    // https://w3c.github.io/paint-timing/#set-of-elements-with-rendered-text
    // NB: Only whether an element is in the document's set of elements with rendered text is ever asked, so each
    //     element knows whether it is in it. See Element::is_in_set_of_elements_with_rendered_text().

    // NB: The text nodes of the document that have not been painted yet, which are looked at as each frame is
    //     painted, so that the user agent knows when it paints a Text node for the first time. A text node is in
    //     here once, however often it is inserted.
    OrderedHashTable<GC::Ref<DOM::Text>> text_nodes_pending_first_paint;

    // https://w3c.github.io/largest-contentful-paint/#current-largest-contentful-paint-candidate
    // A current largest contentful paint candidate, a largest contentful paint candidate or null, initially null.
    Optional<LargestContentfulPaintCandidate> current_largest_contentful_paint_candidate;

    void visit_edges(GC::Cell::Visitor&);
};

}
