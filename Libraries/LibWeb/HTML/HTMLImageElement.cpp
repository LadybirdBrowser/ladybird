/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Timer.h>
#include <LibGfx/Bitmap.h>
#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/Bindings/HTMLImageElementPrototype.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Response.h>
#include <LibWeb/HTML/AnimatedBitmapDecodedImageData.h>
#include <LibWeb/HTML/CORSSettingAttribute.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLLinkElement.h>
#include <LibWeb/HTML/HTMLPictureElement.h>
#include <LibWeb/HTML/HTMLSourceElement.h>
#include <LibWeb/HTML/ImageRequest.h>
#include <LibWeb/HTML/ListOfAvailableImages.h>
#include <LibWeb/HTML/Numbers.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/PotentialCORSRequest.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Layout/ImageBox.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Platform/ImageCodecPlugin.h>
#include <LibWeb/SVG/SVGDecodedImageData.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLImageElement);

HTMLImageElement::HTMLImageElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
    m_animation_timer = Core::Timer::try_create().release_value_but_fixme_should_propagate_errors();
    m_animation_timer->on_timeout = [this] { animate(); };

    document.register_viewport_client(*this);
}

HTMLImageElement::~HTMLImageElement() = default;

void HTMLImageElement::finalize()
{
    Base::finalize();
    document().unregister_viewport_client(*this);
}

void HTMLImageElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLImageElement);

    m_current_request = ImageRequest::create(realm, document().page());
}

void HTMLImageElement::adopted_from(DOM::Document& old_document)
{
    old_document.unregister_viewport_client(*this);
    document().register_viewport_client(*this);
}

void HTMLImageElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_current_request);
    visitor.visit(m_pending_request);
    visit_lazy_loading_element(visitor);
}

bool HTMLImageElement::is_presentational_hint(FlyString const& name) const
{
    if (Base::is_presentational_hint(name))
        return true;

    return first_is_one_of(name,
        HTML::AttributeNames::hspace,
        HTML::AttributeNames::vspace,
        HTML::AttributeNames::border);
}

void HTMLImageElement::apply_presentational_hints(GC::Ref<CSS::CascadedProperties> cascaded_properties) const
{
    for_each_attribute([&](auto& name, auto& value) {
        if (name == HTML::AttributeNames::hspace) {
            if (auto parsed_value = parse_dimension_value(value)) {
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::MarginLeft, *parsed_value);
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::MarginRight, *parsed_value);
            }
        } else if (name == HTML::AttributeNames::vspace) {
            if (auto parsed_value = parse_dimension_value(value)) {
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::MarginTop, *parsed_value);
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::MarginBottom, *parsed_value);
            }
        } else if (name == HTML::AttributeNames::border) {
            if (auto parsed_value = parse_non_negative_integer(value); parsed_value.has_value()) {
                auto width_value = CSS::LengthStyleValue::create(CSS::Length::make_px(*parsed_value));
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BorderTopWidth, width_value);
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BorderRightWidth, width_value);
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BorderBottomWidth, width_value);
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BorderLeftWidth, width_value);

                auto solid_value = CSS::CSSKeywordValue::create(CSS::Keyword::Solid);
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BorderTopStyle, solid_value);
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BorderRightStyle, solid_value);
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BorderBottomStyle, solid_value);
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BorderLeftStyle, solid_value);
            }
        }
    });
}

void HTMLImageElement::form_associated_element_attribute_changed(FlyString const& name, Optional<String> const& value, Optional<FlyString> const&)
{
    if (name == HTML::AttributeNames::crossorigin) {
        m_cors_setting = cors_setting_attribute_from_keyword(value);
    }

    if (name.is_one_of(HTML::AttributeNames::src, HTML::AttributeNames::srcset)) {
        update_the_image_data(true).release_value_but_fixme_should_propagate_errors();
    }

    if (name == HTML::AttributeNames::alt) {
        if (layout_node())
            did_update_alt_text(as<Layout::ImageBox>(*layout_node()));
    }

    if (name == HTML::AttributeNames::decoding) {
        if (value.has_value() && (value->equals_ignoring_ascii_case("sync"sv) || value->equals_ignoring_ascii_case("async"sv)))
            dbgln("FIXME: HTMLImageElement.decoding = '{}' is not implemented yet", value->to_ascii_lowercase());
    }
}

GC::Ptr<Layout::Node> HTMLImageElement::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    return heap().allocate<Layout::ImageBox>(document(), *this, move(style), *this);
}

void HTMLImageElement::adjust_computed_style(CSS::ComputedProperties& style)
{
    // https://drafts.csswg.org/css-display-3/#unbox
    if (style.display().is_contents())
        style.set_property(CSS::PropertyID::Display, CSS::DisplayStyleValue::create(CSS::Display::from_short(CSS::Display::Short::None)));
}

RefPtr<Gfx::ImmutableBitmap> HTMLImageElement::immutable_bitmap() const
{
    return current_image_bitmap();
}

bool HTMLImageElement::is_image_available() const
{
    return m_current_request && m_current_request->is_available();
}

Optional<CSSPixels> HTMLImageElement::intrinsic_width() const
{
    if (auto image_data = m_current_request->image_data())
        return image_data->intrinsic_width();
    return {};
}

Optional<CSSPixels> HTMLImageElement::intrinsic_height() const
{
    if (auto image_data = m_current_request->image_data())
        return image_data->intrinsic_height();
    return {};
}

Optional<CSSPixelFraction> HTMLImageElement::intrinsic_aspect_ratio() const
{
    if (auto image_data = m_current_request->image_data())
        return image_data->intrinsic_aspect_ratio();
    return {};
}

RefPtr<Gfx::ImmutableBitmap> HTMLImageElement::current_image_bitmap(Gfx::IntSize size) const
{
    if (auto data = m_current_request->image_data())
        return data->bitmap(m_current_frame_index, size);
    return nullptr;
}

void HTMLImageElement::set_visible_in_viewport(bool)
{
    // FIXME: Loosen grip on image data when it's not visible, e.g via volatile memory.
}

// https://html.spec.whatwg.org/multipage/embedded-content.html#dom-img-width
WebIDL::UnsignedLong HTMLImageElement::width() const
{
    const_cast<DOM::Document&>(document()).update_layout();

    // Return the rendered width of the image, in CSS pixels, if the image is being rendered.
    if (auto* paintable_box = this->paintable_box())
        return paintable_box->content_width().to_int();

    // On setting [the width or height IDL attribute], they must act as if they reflected the respective content attributes of the same name.
    if (auto width_attr = get_attribute(HTML::AttributeNames::width); width_attr.has_value()) {
        if (auto converted = parse_non_negative_integer(*width_attr); converted.has_value() && *converted <= 2147483647)
            return *converted;
    }

    // ...or else the density-corrected intrinsic width and height of the image, in CSS pixels,
    // if the image has intrinsic dimensions and is available but not being rendered.
    if (auto bitmap = current_image_bitmap())
        return bitmap->width();

    // ...or else 0, if the image is not available or does not have intrinsic dimensions.
    return 0;
}

WebIDL::ExceptionOr<void> HTMLImageElement::set_width(WebIDL::UnsignedLong width)
{
    if (width > 2147483647)
        width = 0;
    return set_attribute(HTML::AttributeNames::width, String::number(width));
}

// https://html.spec.whatwg.org/multipage/embedded-content.html#dom-img-height
WebIDL::UnsignedLong HTMLImageElement::height() const
{
    const_cast<DOM::Document&>(document()).update_layout();

    // Return the rendered height of the image, in CSS pixels, if the image is being rendered.
    if (auto* paintable_box = this->paintable_box())
        return paintable_box->content_height().to_int();

    // On setting [the width or height IDL attribute], they must act as if they reflected the respective content attributes of the same name.
    if (auto height_attr = get_attribute(HTML::AttributeNames::height); height_attr.has_value()) {
        if (auto converted = parse_non_negative_integer(*height_attr); converted.has_value() && *converted <= 2147483647)
            return *converted;
    }

    // ...or else the density-corrected intrinsic height and height of the image, in CSS pixels,
    // if the image has intrinsic dimensions and is available but not being rendered.
    if (auto bitmap = current_image_bitmap())
        return bitmap->height();

    // ...or else 0, if the image is not available or does not have intrinsic dimensions.
    return 0;
}

WebIDL::ExceptionOr<void> HTMLImageElement::set_height(WebIDL::UnsignedLong height)
{
    if (height > 2147483647)
        height = 0;
    return set_attribute(HTML::AttributeNames::height, String::number(height));
}

// https://html.spec.whatwg.org/multipage/embedded-content.html#dom-img-naturalwidth
unsigned HTMLImageElement::natural_width() const
{
    // Return the density-corrected intrinsic width of the image, in CSS pixels,
    // if the image has intrinsic dimensions and is available.
    if (auto bitmap = current_image_bitmap())
        return bitmap->width();

    // ...or else 0.
    return 0;
}

// https://html.spec.whatwg.org/multipage/embedded-content.html#dom-img-naturalheight
unsigned HTMLImageElement::natural_height() const
{
    // Return the density-corrected intrinsic height of the image, in CSS pixels,
    // if the image has intrinsic dimensions and is available.
    if (auto bitmap = current_image_bitmap())
        return bitmap->height();

    // ...or else 0.
    return 0;
}

// https://html.spec.whatwg.org/multipage/embedded-content.html#dom-img-complete
bool HTMLImageElement::complete() const
{
    // The IDL attribute complete must return true if any of the following conditions is true:

    // - Both the src attribute and the srcset attribute are omitted.
    if (!has_attribute(HTML::AttributeNames::src) && !has_attribute(HTML::AttributeNames::srcset))
        return true;

    // - The srcset attribute is omitted and the src attribute's value is the empty string.
    if (!has_attribute(HTML::AttributeNames::srcset) && attribute(HTML::AttributeNames::src).value().is_empty())
        return true;

    // - The img element's current request's state is completely available and its pending request is null.
    if (m_current_request->state() == ImageRequest::State::CompletelyAvailable && !m_pending_request)
        return true;

    // - The img element's current request's state is broken and its pending request is null.
    if (m_current_request->state() == ImageRequest::State::Broken && !m_pending_request)
        return true;

    return false;
}

// https://html.spec.whatwg.org/multipage/embedded-content.html#dom-img-currentsrc
String HTMLImageElement::current_src() const
{
    // The currentSrc IDL attribute must return the img element's current request's current URL.
    auto current_url = m_current_request->current_url();
    if (!current_url.is_valid())
        return {};
    return current_url.to_string();
}

// https://html.spec.whatwg.org/multipage/embedded-content.html#dom-img-decode
WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> HTMLImageElement::decode() const
{
    auto& realm = this->realm();

    // 1. Let promise be a new promise.
    auto promise = WebIDL::create_promise(realm);

    // 2. Queue a microtask to perform the following steps:
    queue_a_microtask(&document(), GC::create_function(realm.heap(), [this, promise, &realm]() mutable {
        // 1. Let global be this's relevant global object.
        auto& global = relevant_global_object(*this);

        auto reject_if_document_not_fully_active = [this, promise, &realm]() -> bool {
            if (this->document().is_fully_active())
                return false;

            auto exception = WebIDL::EncodingError::create(realm, "Node document not fully active"_string);
            HTML::TemporaryExecutionContext context(realm);
            WebIDL::reject_promise(realm, promise, exception);
            return true;
        };

        auto reject_if_current_request_state_broken = [this, promise, &realm]() {
            if (this->current_request().state() != ImageRequest::State::Broken)
                return false;

            auto exception = WebIDL::EncodingError::create(realm, "Current request state is broken"_string);
            HTML::TemporaryExecutionContext context(realm);
            WebIDL::reject_promise(realm, promise, exception);
            return true;
        };

        // 2. If any of the following are true:
        //    - this's node document is not fully active;
        //    - or this's current request's state is broken,
        //    then reject promise with an "EncodingError" DOMException.
        if (reject_if_document_not_fully_active() || reject_if_current_request_state_broken()) {
            return;
        }

        // 3. Otherwise, in parallel wait for one of the following cases to occur, and perform the corresponding actions:
        Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(heap(), [this, promise, &realm, &global] {
            Platform::EventLoopPlugin::the().spin_until(GC::create_function(heap(), [this, promise, &realm, &global] {
                auto queue_reject_task = [promise, &realm, &global](String const& message) {
                    queue_global_task(Task::Source::DOMManipulation, global, GC::create_function(realm.heap(), [&realm, promise, message = String(message)] {
                        auto exception = WebIDL::EncodingError::create(realm, message);
                        HTML::TemporaryExecutionContext context(realm);
                        WebIDL::reject_promise(realm, promise, exception);
                    }));
                };

                // -> This img element's node document stops being fully active
                if (!document().is_fully_active()) {
                    // Queue a global task on the DOM manipulation task source with global to reject promise with an "EncodingError" DOMException.
                    queue_reject_task("Node document not fully active"_string);
                    return true;
                }

                auto state = this->current_request().state();

                // -> FIXME: This img element's current request changes or is mutated
                if (false) {
                    // Queue a global task on the DOM manipulation task source with global to reject promise with an "EncodingError" DOMException.
                    queue_reject_task("Current request changed or was mutated"_string);
                    return true;
                }

                // -> This img element's current request's state becomes broken
                if (state == ImageRequest::State::Broken) {
                    // Queue a global task on the DOM manipulation task source with global to reject promise with an "EncodingError" DOMException.
                    queue_reject_task("Current request state is broken"_string);
                    return true;
                }

                // -> This img element's current request's state becomes completely available
                if (state == ImageRequest::State::CompletelyAvailable) {
                    // FIXME: Decode the image.
                    // FIXME: If decoding does not need to be performed for this image (for example because it is a vector graphic) or the decoding process completes successfully, then queue a global task on the DOM manipulation task source with global to resolve promise with undefined.
                    // FIXME: If decoding fails (for example due to invalid image data), then queue a global task on the DOM manipulation task source with global to reject promise with an "EncodingError" DOMException.

                    // NOTE: For now we just resolve it.
                    queue_global_task(Task::Source::DOMManipulation, global, GC::create_function(realm.heap(), [&realm, promise] {
                        HTML::TemporaryExecutionContext context(realm);
                        WebIDL::resolve_promise(realm, promise, JS::js_undefined());
                    }));
                    return true;
                }

                return false;
            }));
        }));
    }));

    // 3. Return promise.
    return promise;
}

Optional<ARIA::Role> HTMLImageElement::default_role() const
{
    // https://www.w3.org/TR/html-aria/#el-img
    // https://www.w3.org/TR/html-aria/#el-img-no-alt
    // https://w3c.github.io/aria/#image
    // NOTE: The "image" role value is a synonym for the older "img" role value; however, the el-img test in
    //       https://wpt.fyi/results/html-aam/roles.html expects the value to be "image" (not "img").
    if (!alt().is_empty())
        return ARIA::Role::image;
    // https://www.w3.org/TR/html-aria/#el-img-empty-alt
    // NOTE: The "none" role value is a synonym for the older "presentation" role value; however, the el-img-alt-no-value
    //       test in https://wpt.fyi/results/html-aam/roles.html expects the value to be "none" (not "presentation").
    return ARIA::Role::none;
}

// https://html.spec.whatwg.org/multipage/images.html#use-srcset-or-picture
bool HTMLImageElement::uses_srcset_or_picture() const
{
    // An img element is said to use srcset or picture if it has a srcset attribute specified
    // or if it has a parent that is a picture element.
    return has_attribute(HTML::AttributeNames::srcset) || (parent() && is<HTMLPictureElement>(*parent()));
}

// We batch handling of successfully fetched images to avoid interleaving 1 image, 1 layout, 1 image, 1 layout, etc.
// The processing timer is 1ms instead of 0ms, since layout is driven by a 0ms timer, and if we use 0ms here,
// the event loop will process them in insertion order. This is a bit of a hack, but it works.
struct BatchingDispatcher {
public:
    BatchingDispatcher()
        : m_timer(Core::Timer::create_single_shot(1, [this] { process(); }))
    {
    }

    void enqueue(GC::Root<GC::Function<void()>> callback)
    {
        // NOTE: We don't want to flush the queue on every image load, since that would be slow.
        //       However, we don't want to keep growing the batch forever either.
        static constexpr size_t max_loads_to_batch_before_flushing = 16;

        m_queue.append(move(callback));
        if (m_queue.size() < max_loads_to_batch_before_flushing)
            m_timer->restart();
    }

private:
    void process()
    {
        auto queue = move(m_queue);
        for (auto& callback : queue)
            callback->function()();
    }

    NonnullRefPtr<Core::Timer> m_timer;
    Vector<GC::Root<GC::Function<void()>>> m_queue;
};

static BatchingDispatcher& batching_dispatcher()
{
    static BatchingDispatcher dispatcher;
    return dispatcher;
}

// https://html.spec.whatwg.org/multipage/images.html#update-the-image-data
ErrorOr<void> HTMLImageElement::update_the_image_data(bool restart_animations, bool maybe_omit_events)
{
    // 1. If the element's node document is not fully active, then:
    if (!document().is_fully_active()) {
        // FIXME: 1. Continue running this algorithm in parallel.
        // FIXME: 2. Wait until the element's node document is fully active.
        // FIXME: 3. If another instance of this algorithm for this img element was started after this instance
        //           (even if it aborted and is no longer running), then return.
        // FIXME: 4. Queue a microtask to continue this algorithm.
    }

    // 2. FIXME: If the user agent cannot support images, or its support for images has been disabled,
    //           then abort the image request for the current request and the pending request,
    //           set current request's state to unavailable, set pending request to null, and return.

    // 3. Let previous URL be the current request's current URL.
    auto previous_url = m_current_request->current_url();

    // 4. Let selected source be null and selected pixel density be undefined.
    Optional<String> selected_source;
    Optional<float> selected_pixel_density;

    // 5. If the element does not use srcset or picture
    //    and it has a src attribute specified whose value is not the empty string,
    //    then set selected source to the value of the element's src attribute
    //    and set selected pixel density to 1.0.
    auto maybe_src_attribute = attribute(HTML::AttributeNames::src);
    if (!uses_srcset_or_picture() && maybe_src_attribute.has_value() && !maybe_src_attribute.value().is_empty()) {
        selected_source = maybe_src_attribute.release_value();
        selected_pixel_density = 1.0f;
    }

    // 6. Set the element's last selected source to selected source.
    m_last_selected_source = selected_source;

    // 7. If selected source is not null, then:
    if (selected_source.has_value()) {
        // 1. Parse selected source, relative to the element's node document.
        //    If that is not successful, then abort this inner set of steps.
        //    Otherwise, let urlString be the resulting URL string.
        auto url_string = document().parse_url(selected_source.value());
        if (!url_string.is_valid())
            goto after_step_7;

        // 2. Let key be a tuple consisting of urlString, the img element's crossorigin attribute's mode,
        //    and, if that mode is not No CORS, the node document's origin.
        ListOfAvailableImages::Key key;
        key.url = url_string;
        key.mode = m_cors_setting;
        key.origin = document().origin();

        // 3. If the list of available images contains an entry for key, then:
        if (auto* entry = document().list_of_available_images().get(key)) {
            // 1. Set the ignore higher-layer caching flag for that entry.
            entry->ignore_higher_layer_caching = true;

            // 2. Abort the image request for the current request and the pending request.
            abort_the_image_request(realm(), m_current_request);
            abort_the_image_request(realm(), m_pending_request);

            // 3. Set pending request to null.
            m_pending_request = nullptr;

            // 4. Let current request be a new image request whose image data is that of the entry and whose state is completely available.
            m_current_request = ImageRequest::create(realm(), document().page());
            m_current_request->set_image_data(entry->image_data);
            m_current_request->set_state(ImageRequest::State::CompletelyAvailable);

            // 5. Prepare current request for presentation given img.
            m_current_request->prepare_for_presentation(*this);

            // 6. Set current request's current pixel density to selected pixel density.
            // FIXME: Spec bug! `selected_pixel_density` can be undefined here, per the spec.
            //        That's why we value_or(1.0f) it.
            m_current_request->set_current_pixel_density(selected_pixel_density.value_or(1.0f));

            // 7. Queue an element task on the DOM manipulation task source given the img element and following steps:
            queue_an_element_task(HTML::Task::Source::DOMManipulation, [this, restart_animations, maybe_omit_events, url_string, previous_url] {
                // 1. If restart animation is set, then restart the animation.
                if (restart_animations)
                    restart_the_animation();

                // 2. Set current request's current URL to urlString.
                m_current_request->set_current_url(realm(), url_string);

                // 3. If maybe omit events is not set or previousURL is not equal to urlString, then fire an event named load at the img element.
                if (!maybe_omit_events || previous_url != url_string)
                    dispatch_event(DOM::Event::create(realm(), HTML::EventNames::load));
            });

            // 8. Abort the update the image data algorithm.
            return {};
        }
    }
after_step_7:
    // 8. Queue a microtask to perform the rest of this algorithm, allowing the task that invoked this algorithm to continue.
    queue_a_microtask(&document(), GC::create_function(this->heap(), [this, restart_animations, maybe_omit_events, previous_url]() mutable {
        // FIXME: 9. If another instance of this algorithm for this img element was started after this instance
        //           (even if it aborted and is no longer running), then return.

        // 10. Let selected source and selected pixel density be
        //    the URL and pixel density that results from selecting an image source, respectively.
        Optional<ImageSource> selected_source;
        Optional<float> pixel_density;
        if (auto result = select_an_image_source(); result.has_value()) {
            selected_source = result.value().source;
            pixel_density = result.value().pixel_density;
        }

        // 11. If selected source is null, then:
        if (!selected_source.has_value()) {
            // 1. Set the current request's state to broken,
            //    abort the image request for the current request and the pending request,
            //    and set pending request to null.
            m_current_request->set_state(ImageRequest::State::Broken);
            abort_the_image_request(realm(), m_current_request);
            abort_the_image_request(realm(), m_pending_request);
            m_pending_request = nullptr;

            // 2. Queue an element task on the DOM manipulation task source given the img element and the following steps:
            queue_an_element_task(HTML::Task::Source::DOMManipulation, [this, maybe_omit_events, previous_url] {
                // 1. Change the current request's current URL to the empty string.
                m_current_request->set_current_url(realm(), ""sv);

                // 2. If all of the following conditions are true:
                //    - the element has a src attribute or it uses srcset or picture; and
                //    - maybe omit events is not set or previousURL is not the empty string
                if (
                    (has_attribute(HTML::AttributeNames::src) || uses_srcset_or_picture())
                    && (!maybe_omit_events || m_current_request->current_url() != ""sv)) {
                    dispatch_event(DOM::Event::create(realm(), HTML::EventNames::error));
                }
            });

            // 3. Return.
            return;
        }

        // 12. Parse selected source, relative to the element's node document, and let urlString be the resulting URL string.
        auto url_string = document().parse_url(selected_source.value().url.to_byte_string());
        // If that is not successful, then:
        if (!url_string.is_valid()) {
            // 1. Abort the image request for the current request and the pending request.
            abort_the_image_request(realm(), m_current_request);
            abort_the_image_request(realm(), m_pending_request);

            // 2. Set the current request's state to broken.
            m_current_request->set_state(ImageRequest::State::Broken);

            // 3. Set pending request to null.
            m_pending_request = nullptr;

            // 4. Queue an element task on the DOM manipulation task source given the img element and the following steps:
            queue_an_element_task(HTML::Task::Source::DOMManipulation, [this, selected_source, maybe_omit_events, previous_url] {
                // 1. Change the current request's current URL to selected source.
                m_current_request->set_current_url(realm(), selected_source.value().url);

                // 2. If maybe omit events is not set or previousURL is not equal to selected source, then fire an event named error at the img element.
                if (!maybe_omit_events || previous_url != selected_source.value().url)
                    dispatch_event(DOM::Event::create(realm(), HTML::EventNames::error));
            });

            // 5. Return.
            return;
        }

        // 13. If the pending request is not null and urlString is the same as the pending request's current URL, then return.
        if (m_pending_request && url_string == m_pending_request->current_url())
            return;

        // 14. If urlString is the same as the current request's current URL and current request's state is partially available,
        //     then abort the image request for the pending request,
        //     queue an element task on the DOM manipulation task source given the img element
        //     to restart the animation if restart animation is set, and return.
        if (url_string == m_current_request->current_url() && m_current_request->state() == ImageRequest::State::PartiallyAvailable) {
            abort_the_image_request(realm(), m_pending_request);
            if (restart_animations) {
                queue_an_element_task(HTML::Task::Source::DOMManipulation, [this] {
                    restart_the_animation();
                });
            }
            return;
        }

        // 15. If the pending request is not null, then abort the image request for the pending request.
        abort_the_image_request(realm(), m_pending_request);

        // AD-HOC: At this point we start deviating from the spec in order to allow sharing ImageRequest between
        //         multiple image elements (as well as CSS background-images, etc.)

        // 16. Set image request to a new image request whose current URL is urlString.
        auto image_request = ImageRequest::create(realm(), document().page());
        image_request->set_current_url(realm(), url_string);

        // 17. If current request's state is unavailable or broken, then set the current request to image request.
        //     Otherwise, set the pending request to image request.
        if (m_current_request->state() == ImageRequest::State::Unavailable || m_current_request->state() == ImageRequest::State::Broken)
            m_current_request = image_request;
        else
            m_pending_request = image_request;

        // 23. Let delay load event be true if the img's lazy loading attribute is in the Eager state, or if scripting is disabled for the img, and false otherwise.
        auto delay_load_event = lazy_loading_attribute() == LazyLoading::Eager;

        // When delay load event is true, fetching the image must delay the load event of the element's node document
        // until the task that is queued by the networking task source once the resource has been fetched (defined below) has been run.
        if (delay_load_event)
            m_load_event_delayer.emplace(document());

        add_callbacks_to_image_request(*image_request, maybe_omit_events, url_string, previous_url);

        // AD-HOC: If the image request is already available or fetching, no need to start another fetch.
        if (image_request->is_available() || image_request->is_fetching())
            return;

        // 18. Let request be the result of creating a potential-CORS request given urlString, "image",
        //     and the current state of the element's crossorigin content attribute.
        auto request = create_potential_CORS_request(vm(), url_string, Fetch::Infrastructure::Request::Destination::Image, m_cors_setting);

        // 19. Set request's client to the element's node document's relevant settings object.
        request->set_client(&document().relevant_settings_object());

        // 20. If the element uses srcset or picture, set request's initiator to "imageset".
        if (uses_srcset_or_picture())
            request->set_initiator(Fetch::Infrastructure::Request::Initiator::ImageSet);

        // 21. Set request's referrer policy to the current state of the element's referrerpolicy attribute.
        request->set_referrer_policy(ReferrerPolicy::from_string(get_attribute_value(HTML::AttributeNames::referrerpolicy)).value_or(ReferrerPolicy::ReferrerPolicy::EmptyString));

        // 22. Set request's priority to the current state of the element's fetchpriority attribute.
        request->set_priority(Fetch::Infrastructure::request_priority_from_string(get_attribute_value(HTML::AttributeNames::fetchpriority)).value_or(Fetch::Infrastructure::Request::Priority::Auto));

        // 24. If the will lazy load element steps given the img return true, then:
        if (will_lazy_load_element()) {
            // 1. Set the img's lazy load resumption steps to the rest of this algorithm starting with the step labeled fetch the image.
            set_lazy_load_resumption_steps([this, request, image_request]() {
                image_request->fetch_image(realm(), request);
            });

            // 2. Start intersection-observing a lazy loading element for the img element.
            document().start_intersection_observing_a_lazy_loading_element(*this);

            // 3. Return.
            return;
        }

        image_request->fetch_image(realm(), request);
    }));
    return {};
}

void HTMLImageElement::add_callbacks_to_image_request(GC::Ref<ImageRequest> image_request, bool maybe_omit_events, URL::URL const& url_string, URL::URL const& previous_url)
{
    image_request->add_callbacks(
        [this, image_request, maybe_omit_events, url_string, previous_url]() {
            batching_dispatcher().enqueue(GC::create_function(realm().heap(), [this, image_request, maybe_omit_events, url_string, previous_url] {
                VERIFY(image_request->shared_resource_request());
                auto image_data = image_request->shared_resource_request()->image_data();
                image_request->set_image_data(image_data);

                ListOfAvailableImages::Key key;
                key.url = url_string;
                key.mode = m_cors_setting;
                key.origin = document().origin();

                // 1. If image request is the pending request, abort the image request for the current request,
                //    upgrade the pending request to the current request
                //    and prepare image request for presentation given the img element.
                if (image_request == m_pending_request) {
                    abort_the_image_request(realm(), m_current_request);
                    upgrade_pending_request_to_current_request();
                    image_request->prepare_for_presentation(*this);
                }

                // 2. Set image request to the completely available state.
                image_request->set_state(ImageRequest::State::CompletelyAvailable);

                // 3. Add the image to the list of available images using the key key, with the ignore higher-layer caching flag set.
                document().list_of_available_images().add(key, *image_data, true);

                set_needs_style_update(true);
                document().set_needs_layout();

                // 4. If maybe omit events is not set or previousURL is not equal to urlString, then fire an event named load at the img element.
                if (!maybe_omit_events || previous_url != url_string)
                    dispatch_event(DOM::Event::create(realm(), HTML::EventNames::load));

                if (image_data->is_animated() && image_data->frame_count() > 1) {
                    m_current_frame_index = 0;
                    m_animation_timer->set_interval(image_data->frame_duration(0));
                    m_animation_timer->start();
                }

                m_load_event_delayer.clear();
            }));
        },
        [this, image_request, maybe_omit_events, url_string, previous_url]() {
            // The image data is not in a supported file format;

            // the user agent must set image request's state to broken,
            image_request->set_state(ImageRequest::State::Broken);

            // abort the image request for the current request and the pending request,
            abort_the_image_request(realm(), m_current_request);
            abort_the_image_request(realm(), m_pending_request);

            // upgrade the pending request to the current request if image request is the pending request,
            if (image_request == m_pending_request)
                upgrade_pending_request_to_current_request();

            // and then, if maybe omit events is not set or previousURL is not equal to urlString,
            // queue an element task on the DOM manipulation task source given the img element
            // to fire an event named error at the img element.
            if (!maybe_omit_events || previous_url != url_string)
                dispatch_event(DOM::Event::create(realm(), HTML::EventNames::error));

            m_load_event_delayer.clear();
        });
}

void HTMLImageElement::did_set_viewport_rect(CSSPixelRect const& viewport_rect)
{
    if (viewport_rect.size() == m_last_seen_viewport_size)
        return;
    m_last_seen_viewport_size = viewport_rect.size();
    batching_dispatcher().enqueue(GC::create_function(realm().heap(), [this] {
        react_to_changes_in_the_environment();
    }));
}

// https://html.spec.whatwg.org/multipage/images.html#img-environment-changes
void HTMLImageElement::react_to_changes_in_the_environment()
{
    // FIXME: 1. Await a stable state.
    //           The synchronous section consists of all the remaining steps of this algorithm
    //           until the algorithm says the synchronous section has ended.
    //           (Steps in synchronous sections are marked with ⌛.)

    // 2. ⌛ If the img element does not use srcset or picture,
    //       its node document is not fully active,
    //       FIXME: has image data whose resource type is multipart/x-mixed-replace,
    //       or the pending request is not null,
    //       then return.
    if (!uses_srcset_or_picture() || !document().is_fully_active() || m_pending_request)
        return;

    // 3. ⌛ Let selected source and selected pixel density be the URL and pixel density
    //       that results from selecting an image source, respectively.
    Optional<String> selected_source;
    Optional<float> pixel_density;
    if (auto result = select_an_image_source(); result.has_value()) {
        selected_source = result.value().source.url;
        pixel_density = result.value().pixel_density;
    }

    // 4. ⌛ If selected source is null, then return.
    if (!selected_source.has_value())
        return;

    // 5. ⌛ If selected source and selected pixel density are the same
    //       as the element's last selected source and current pixel density, then return.
    if (selected_source == m_last_selected_source && pixel_density == m_current_request->current_pixel_density())
        return;

    // 6. ⌛ Parse selected source, relative to the element's node document,
    //       and let urlString be the resulting URL string. If that is not successful, then return.
    auto url_string = document().parse_url(selected_source.value());
    if (!url_string.is_valid())
        return;

    // 7. ⌛ Let corsAttributeState be the state of the element's crossorigin content attribute.
    auto cors_attribute_state = m_cors_setting;

    // 8. ⌛ Let origin be the img element's node document's origin.
    auto origin = document().origin();

    // 9. ⌛ Let client be the img element's node document's relevant settings object.
    auto& client = document().relevant_settings_object();

    // 10. ⌛ Let key be a tuple consisting of urlString, corsAttributeState, and, if corsAttributeState is not No CORS, origin.
    ListOfAvailableImages::Key key;
    key.url = url_string;
    key.mode = m_cors_setting;
    if (cors_attribute_state != CORSSettingAttribute::NoCORS)
        key.origin = document().origin();

    // 11. ⌛ Let image request be a new image request whose current URL is urlString
    auto image_request = ImageRequest::create(realm(), document().page());
    image_request->set_current_url(realm(), url_string);

    // 12. ⌛ Let the element's pending request be image request.
    m_pending_request = image_request;

    // FIXME: 13. End the synchronous section, continuing the remaining steps in parallel.

    auto step_15 = [this](String const& selected_source, GC::Ref<ImageRequest> image_request, ListOfAvailableImages::Key const& key, GC::Ref<DecodedImageData> image_data) {
        // 15. Queue an element task on the DOM manipulation task source given the img element and the following steps:
        queue_an_element_task(HTML::Task::Source::DOMManipulation, [this, selected_source, image_request, key, image_data] {
            // 1. FIXME: If the img element has experienced relevant mutations since this algorithm started, then let pending request be null and abort these steps.
            // AD-HOC: Check if we have a pending request still, otherwise we will crash when upgrading the request. This will happen if the image has experienced mutations,
            //        but since the pending request may be set by another task soon after it is cleared, this check is probably not sufficient.
            if (!m_pending_request)
                return;

            // 2. Let the img element's last selected source be selected source and the img element's current pixel density be selected pixel density.
            m_last_selected_source = selected_source;

            // 3. Set the image request's state to completely available.
            image_request->set_state(ImageRequest::State::CompletelyAvailable);

            // 4. Add the image to the list of available images using the key key, with the ignore higher-layer caching flag set.
            document().list_of_available_images().add(key, image_data, true);

            // 5. Upgrade the pending request to the current request.
            upgrade_pending_request_to_current_request();

            // 6. Prepare image request for presentation given the img element.
            image_request->prepare_for_presentation(*this);
            // FIXME: This is ad-hoc, updating the layout here should probably be handled by prepare_for_presentation().
            set_needs_style_update(true);
            document().set_needs_layout();

            // 7. Fire an event named load at the img element.
            dispatch_event(DOM::Event::create(realm(), HTML::EventNames::load));
        });
    };

    // 14. If the list of available images contains an entry for key, then set image request's image data to that of the entry.
    //     Continue to the next step.
    if (auto* entry = document().list_of_available_images().get(key)) {
        image_request->set_image_data(entry->image_data);
        step_15(selected_source.value(), *image_request, key, entry->image_data);
    }
    // Otherwise:
    else {
        // 1. Let request be the result of creating a potential-CORS request given urlString, "image", and corsAttributeState.
        auto request = create_potential_CORS_request(vm(), url_string, Fetch::Infrastructure::Request::Destination::Image, m_cors_setting);

        // 2. Set request's client to client, initiator to "imageset", and set request's synchronous flag.
        request->set_client(&client);
        request->set_initiator(Fetch::Infrastructure::Request::Initiator::ImageSet);

        // 3. Set request's referrer policy to the current state of the element's referrerpolicy attribute.
        request->set_referrer_policy(ReferrerPolicy::from_string(get_attribute_value(HTML::AttributeNames::referrerpolicy)).value_or(ReferrerPolicy::ReferrerPolicy::EmptyString));

        // FIXME: 4. Set request's priority to the current state of the element's fetchpriority attribute.

        // Set the callbacks to handle steps 6 and 7 before starting the fetch request.
        image_request->add_callbacks(
            [this, step_15, selected_source = selected_source.value(), image_request, key]() mutable {
                // 6. If response's unsafe response is a network error
                // NOTE: This is handled in the second callback below.

                // FIXME: or if the image format is unsupported (as determined by applying the image sniffing rules, again as mentioned earlier),

                // or if the user agent is able to determine that image request's image is corrupted in some
                // fatal way such that the image dimensions cannot be obtained,
                // NOTE: This is also handled in the other callback.

                // FIXME: or if the resource type is multipart/x-mixed-replace,

                // then let pending request be null and abort these steps.

                batching_dispatcher().enqueue(GC::create_function(realm().heap(), [step_15, selected_source = move(selected_source), image_request, key] {
                    // 7. Otherwise, response's unsafe response is image request's image data. It can be either CORS-same-origin
                    //    or CORS-cross-origin; this affects the image's interaction with other APIs (e.g., when used on a canvas).
                    VERIFY(image_request->shared_resource_request());
                    auto image_data = image_request->shared_resource_request()->image_data();
                    image_request->set_image_data(image_data);
                    step_15(selected_source, image_request, key, *image_data);
                }));
            },
            [this]() {
                // 6. If response's unsafe response is a network error
                //    or if the image format is unsupported (as determined by applying the image sniffing rules, again as mentioned earlier),
                //    ...
                //    or if the user agent is able to determine that image request's image is corrupted in some
                //    fatal way such that the image dimensions cannot be obtained,
                m_pending_request = nullptr;
            });

        // 5. Let response be the result of fetching request.
        image_request->fetch_image(realm(), request);
    }
}

// https://html.spec.whatwg.org/multipage/images.html#upgrade-the-pending-request-to-the-current-request
void HTMLImageElement::upgrade_pending_request_to_current_request()
{
    // 1. Let the img element's current request be the pending request.
    VERIFY(m_pending_request);
    m_current_request = m_pending_request;

    // 2. Let the img element's pending request be null.
    m_pending_request = nullptr;
}

void HTMLImageElement::handle_failed_fetch()
{
    // AD-HOC
    dispatch_event(DOM::Event::create(realm(), HTML::EventNames::error));
}

// https://html.spec.whatwg.org/multipage/rendering.html#restart-the-animation
void HTMLImageElement::restart_the_animation()
{
    m_current_frame_index = 0;

    auto image_data = m_current_request->image_data();
    if (image_data && image_data->frame_count() > 1) {
        m_animation_timer->start();
    } else {
        m_animation_timer->stop();
    }
}

static bool is_supported_image_type(String const& type)
{
    if (type.is_empty())
        return true;
    if (!type.starts_with_bytes("image/"sv, CaseSensitivity::CaseInsensitive))
        return false;
    // FIXME: These should be derived from ImageDecoder
    if (type.equals_ignoring_ascii_case("image/bmp"sv)
        || type.equals_ignoring_ascii_case("image/gif"sv)
        || type.equals_ignoring_ascii_case("image/vnd.microsoft.icon"sv)
        || type.equals_ignoring_ascii_case("image/x-icon"sv)
        || type.equals_ignoring_ascii_case("image/jpeg"sv)
        || type.equals_ignoring_ascii_case("image/jpg"sv)
        || type.equals_ignoring_ascii_case("image/pjpeg"sv)
        || type.equals_ignoring_ascii_case("image/jxl"sv)
        || type.equals_ignoring_ascii_case("image/png"sv)
        || type.equals_ignoring_ascii_case("image/apng"sv)
        || type.equals_ignoring_ascii_case("image/x-png"sv)
        || type.equals_ignoring_ascii_case("image/tiff"sv)
        || type.equals_ignoring_ascii_case("image/tinyvg"sv)
        || type.equals_ignoring_ascii_case("image/webp"sv)
        || type.equals_ignoring_ascii_case("image/svg+xml"sv))
        return true;

    return false;
}

// https://html.spec.whatwg.org/multipage/images.html#update-the-source-set
static void update_the_source_set(DOM::Element& element)
{
    // When asked to update the source set for a given img or link element el, user agents must do the following:
    VERIFY(is<HTMLImageElement>(element) || is<HTMLLinkElement>(element));

    // 1. Set el's source set to an empty source set.
    if (is<HTMLImageElement>(element))
        static_cast<HTMLImageElement&>(element).set_source_set(SourceSet {});
    else if (is<HTMLLinkElement>(element))
        TODO();

    // 2. Let elements be « el ».
    GC::RootVector<DOM::Element*> elements(element.heap());
    elements.append(&element);

    // 3. If el is an img element whose parent node is a picture element,
    //    then replace the contents of elements with el's parent node's child elements, retaining relative order.
    if (is<HTMLImageElement>(element) && element.parent() && is<HTMLPictureElement>(*element.parent())) {
        elements.clear();
        element.parent()->for_each_child_of_type<DOM::Element>([&](auto& child) {
            elements.append(&child);
            return IterationDecision::Continue;
        });
    }

    // 4. Let img be el if el is an img element, otherwise null.
    HTMLImageElement* img = nullptr;
    if (is<HTMLImageElement>(element))
        img = static_cast<HTMLImageElement*>(&element);

    // 5. For each child in elements:
    for (auto child : elements) {
        // 1. If child is el:
        if (child == &element) {
            // 1. Let default source be the empty string.
            String default_source;

            // 2. Let srcset be the empty string.
            String srcset;

            // 3. Let sizes be the empty string.
            String sizes;

            // 4. If el is an img element that has a srcset attribute, then set srcset to that attribute's value.
            if (is<HTMLImageElement>(element)) {
                if (auto srcset_value = element.attribute(HTML::AttributeNames::srcset); srcset_value.has_value())
                    srcset = srcset_value.release_value();
            }

            // 5. Otherwise, if el is a link element that has an imagesrcset attribute, then set srcset to that attribute's value.
            else if (is<HTMLLinkElement>(element)) {
                if (auto imagesrcset_value = element.attribute(HTML::AttributeNames::imagesrcset); imagesrcset_value.has_value())
                    srcset = imagesrcset_value.release_value();
            }

            // 6. If el is an img element that has a sizes attribute, then set sizes to that attribute's value.
            if (is<HTMLImageElement>(element)) {
                if (auto sizes_value = element.attribute(HTML::AttributeNames::sizes); sizes_value.has_value())
                    sizes = sizes_value.release_value();
            }

            // 7. Otherwise, if el is a link element that has an imagesizes attribute, then set sizes to that attribute's value.
            else if (is<HTMLLinkElement>(element)) {
                if (auto imagesizes_value = element.attribute(HTML::AttributeNames::imagesizes); imagesizes_value.has_value())
                    sizes = imagesizes_value.release_value();
            }

            // 8. If el is an img element that has a src attribute, then set default source to that attribute's value.
            if (is<HTMLImageElement>(element)) {
                if (auto src_value = element.attribute(HTML::AttributeNames::src); src_value.has_value())
                    default_source = src_value.release_value();
            }

            // 9. Otherwise, if el is a link element that has an href attribute, then set default source to that attribute's value.
            else if (is<HTMLLinkElement>(element)) {
                if (auto href_value = element.attribute(HTML::AttributeNames::href); href_value.has_value())
                    default_source = href_value.release_value();
            }

            // 10. Let el's source set be the result of creating a source set given default source, srcset, sizes, and img.
            if (is<HTMLImageElement>(element))
                static_cast<HTMLImageElement&>(element).set_source_set(SourceSet::create(element, default_source, srcset, sizes, img));
            else if (is<HTMLLinkElement>(element))
                TODO();

            // 11. Return.
            return;
        }
        // 2. If child is not a source element, then continue.
        if (!is<HTMLSourceElement>(child))
            continue;

        // 3. If child does not have a srcset attribute, continue to the next child.
        if (!child->has_attribute(HTML::AttributeNames::srcset))
            continue;

        // 4. Parse child's srcset attribute and let the returned source set be source set.
        auto source_set = parse_a_srcset_attribute(child->get_attribute_value(HTML::AttributeNames::srcset));

        // 5. If source set has zero image sources, continue to the next child.
        if (source_set.is_empty())
            continue;

        // 6. If child has a media attribute, and its value does not match the environment, continue to the next child.
        if (child->has_attribute(HTML::AttributeNames::media)) {
            auto media_query = parse_media_query(CSS::Parser::ParsingContext { element.document() },
                child->get_attribute_value(HTML::AttributeNames::media));
            if (!media_query || !element.document().window() || !media_query->evaluate(*element.document().window())) {
                continue;
            }
        }

        // 7. Parse child's sizes attribute with img, and let source set's source size be the returned value.
        source_set.m_source_size = parse_a_sizes_attribute(element, child->get_attribute_value(HTML::AttributeNames::sizes), img);

        // 8. If child has a type attribute, and its value is an unknown or unsupported MIME type, continue to the next child.
        if (child->has_attribute(HTML::AttributeNames::type)) {
            auto mime_type = child->get_attribute_value(HTML::AttributeNames::type);
            if (is<HTMLImageElement>(element)) {
                if (!is_supported_image_type(mime_type))
                    continue;
            }

            // FIXME: Implement this step for link elements
        }

        // FIXME: 9. If child has width or height attributes, set el's dimension attribute source to child.
        //           Otherwise, set el's dimension attribute source to el.

        // 10. Normalize the source densities of source set.
        source_set.normalize_source_densities(element);

        // 11. Let el's source set be source set.
        if (is<HTMLImageElement>(element))
            static_cast<HTMLImageElement&>(element).set_source_set(move(source_set));
        else if (is<HTMLLinkElement>(element))
            TODO();

        // 12. Return.
        return;
    }
}

// https://html.spec.whatwg.org/multipage/images.html#select-an-image-source
Optional<ImageSourceAndPixelDensity> HTMLImageElement::select_an_image_source()
{
    // 1. Update the source set for el.
    update_the_source_set(*this);

    // 2. If el's source set is empty, return null as the URL and undefined as the pixel density.
    if (m_source_set.is_empty())
        return {};

    // 3. Return the result of selecting an image from el's source set.
    return m_source_set.select_an_image_source();
}

void HTMLImageElement::set_source_set(SourceSet source_set)
{
    m_source_set = move(source_set);
}

void HTMLImageElement::animate()
{
    auto image_data = m_current_request->image_data();
    if (!image_data) {
        return;
    }

    m_current_frame_index = (m_current_frame_index + 1) % image_data->frame_count();
    auto current_frame_duration = image_data->frame_duration(m_current_frame_index);

    if (current_frame_duration != m_animation_timer->interval()) {
        m_animation_timer->restart(current_frame_duration);
    }

    if (m_current_frame_index == image_data->frame_count() - 1) {
        ++m_loops_completed;
        if (m_loops_completed > 0 && m_loops_completed == image_data->loop_count()) {
            m_animation_timer->stop();
        }
    }

    if (paintable())
        paintable()->set_needs_display();
}

bool HTMLImageElement::allows_auto_sizes() const
{
    // An img element allows auto-sizes if:
    // - its loading attribute is in the Lazy state, and
    // - its sizes attribute's value is "auto" (ASCII case-insensitive), or starts with "auto," (ASCII case-insensitive).
    if (lazy_loading_attribute() != LazyLoading::Lazy)
        return false;
    auto sizes = attribute(HTML::AttributeNames::sizes);
    return sizes.has_value()
        && (sizes->equals_ignoring_ascii_case("auto"sv)
            || sizes->starts_with_bytes("auto,"sv, AK::CaseSensitivity::CaseInsensitive));
}

}
