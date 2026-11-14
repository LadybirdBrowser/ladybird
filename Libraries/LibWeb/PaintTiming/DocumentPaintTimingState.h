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
#include <LibCompositing/RustFFI.h>
#include <LibGC/Ptr.h>
#include <LibGC/RootHashMap.h>
#include <LibGC/RootVector.h>
#include <LibGC/Weak.h>
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

    void visit_edges(GC::Cell::Visitor&) const;
};

// https://w3c.github.io/largest-contentful-paint/#largest-contentful-paint-candidate
struct LargestContentfulPaintCandidate {
    // element, an element
    // NB: Held weakly, as only the size of the current candidate is read once its element is removed.
    GC::Weak<DOM::Element> element;

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
using PaintedTextNodes = GC::OrderedRootHashMap<GC::Ref<DOM::Element>, GC::RootVector<GC::Ref<DOM::Text>>>;

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

    // NB: The user agent learns that it paints a Text node for the first time from the layout nodes of text nodes: the
    //     layout node arena notes each text layout node that was given new rendered text, as one made for an inserted
    //     text node or one whose text changed is, and the notes are taken as each frame is painted. These are the
    //     taken text layout nodes that were not paintable yet but may be once their style or layout changes, by slot.
    //     A slot whose layout node is gone is dropped the next time the list is looked at, so the list holds nothing of
    //     a removed node.
    Vector<Compositing::RustFFI::NodeSlotId> text_boxes_pending_first_paint;

    // NB: Whether the layout node arena was told to stop noting text layout nodes, once nothing reports them.
    bool stopped_noting_text_boxes { false };

    // https://w3c.github.io/largest-contentful-paint/#current-largest-contentful-paint-candidate
    // A current largest contentful paint candidate, a largest contentful paint candidate or null, initially null.
    Optional<LargestContentfulPaintCandidate> current_largest_contentful_paint_candidate;

    void visit_edges(GC::Cell::Visitor&);
};

}
