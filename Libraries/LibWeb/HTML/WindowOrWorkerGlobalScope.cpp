/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <AK/String.h>
#include <AK/Utf8View.h>
#include <AK/Vector.h>
#include <LibGC/Function.h>
#include <LibJS/Runtime/Array.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Crypto/Crypto.h>
#include <LibWeb/Fetch/FetchMethod.h>
#include <LibWeb/HTML/CanvasRenderingContext2D.h>
#include <LibWeb/HTML/ErrorEvent.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventSource.h>
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/HTML/Scripting/ClassicScript.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/Scripting/Fetching.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Timer.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/HighResolutionTime/Performance.h>
#include <LibWeb/HighResolutionTime/SupportedPerformanceTypes.h>
#include <LibWeb/IndexedDB/IDBFactory.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/PerformanceTimeline/EntryTypes.h>
#include <LibWeb/PerformanceTimeline/EventNames.h>
#include <LibWeb/PerformanceTimeline/PerformanceObserver.h>
#include <LibWeb/PerformanceTimeline/PerformanceObserverEntryList.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Platform/ImageCodecPlugin.h>
#include <LibWeb/ResourceTiming/PerformanceResourceTiming.h>
#include <LibWeb/UserTiming/PerformanceMark.h>
#include <LibWeb/UserTiming/PerformanceMeasure.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Types.h>
#include <LibWeb/WebSockets/WebSocket.h>

namespace Web::HTML {

WindowOrWorkerGlobalScopeMixin::~WindowOrWorkerGlobalScopeMixin() = default;

void WindowOrWorkerGlobalScopeMixin::initialize(JS::Realm&)
{
#define __ENUMERATE_SUPPORTED_PERFORMANCE_ENTRY_TYPES(entry_type, cpp_class) \
    m_performance_entry_buffer_map.set(entry_type,                           \
        PerformanceTimeline::PerformanceEntryTuple {                         \
            .performance_entry_buffer = {},                                  \
            .max_buffer_size = cpp_class::max_buffer_size(),                 \
            .available_from_timeline = cpp_class::available_from_timeline(), \
            .dropped_entries_count = 0,                                      \
        });
    ENUMERATE_SUPPORTED_PERFORMANCE_ENTRY_TYPES
#undef __ENUMERATE_SUPPORTED_PERFORMANCE_ENTRY_TYPES
}

void WindowOrWorkerGlobalScopeMixin::visit_edges(JS::Cell::Visitor& visitor)
{
    visitor.visit(m_performance);
    visitor.visit(m_supported_entry_types_array);
    visitor.visit(m_timers);
    visitor.visit(m_registered_performance_observer_objects);
    visitor.visit(m_indexed_db);
    for (auto& entry : m_performance_entry_buffer_map)
        entry.value.visit_edges(visitor);
    visitor.visit(m_registered_event_sources);
    visitor.visit(m_crypto);
    visitor.visit(m_resource_timing_secondary_buffer);
}

void WindowOrWorkerGlobalScopeMixin::finalize()
{
    clear_map_of_active_timers();
}

// https://html.spec.whatwg.org/multipage/webappapis.html#dom-origin
String WindowOrWorkerGlobalScopeMixin::origin() const
{
    // The origin getter steps are to return this's relevant settings object's origin, serialized.
    return relevant_settings_object(this_impl()).origin().serialize();
}

// https://html.spec.whatwg.org/multipage/webappapis.html#dom-issecurecontext
bool WindowOrWorkerGlobalScopeMixin::is_secure_context() const
{
    // The isSecureContext getter steps are to return true if this's relevant settings object is a secure context, or false otherwise.
    return HTML::is_secure_context(relevant_settings_object(this_impl()));
}

// https://html.spec.whatwg.org/multipage/webappapis.html#dom-crossoriginisolated
bool WindowOrWorkerGlobalScopeMixin::cross_origin_isolated() const
{
    // The crossOriginIsolated getter steps are to return this's relevant settings object's cross-origin isolated capability.
    return relevant_settings_object(this_impl()).cross_origin_isolated_capability() == CanUseCrossOriginIsolatedAPIs::Yes;
}

// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#dom-createimagebitmap
GC::Ref<WebIDL::Promise> WindowOrWorkerGlobalScopeMixin::create_image_bitmap(ImageBitmapSource image, Optional<ImageBitmapOptions> options) const
{
    return create_image_bitmap_impl(image, {}, {}, {}, {}, options);
}

// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#dom-createimagebitmap
GC::Ref<WebIDL::Promise> WindowOrWorkerGlobalScopeMixin::create_image_bitmap(ImageBitmapSource image, WebIDL::Long sx, WebIDL::Long sy, WebIDL::Long sw, WebIDL::Long sh, Optional<ImageBitmapOptions> options) const
{
    return create_image_bitmap_impl(image, sx, sy, sw, sh, options);
}

GC::Ref<WebIDL::Promise> WindowOrWorkerGlobalScopeMixin::create_image_bitmap_impl(ImageBitmapSource& image, Optional<WebIDL::Long> sx, Optional<WebIDL::Long> sy, Optional<WebIDL::Long> sw, Optional<WebIDL::Long> sh, Optional<ImageBitmapOptions>& options) const
{
    auto& realm = this_impl().realm();

    // 1. If either sw or sh is given and is 0, then return a promise rejected with a RangeError.
    if (sw == 0 || sh == 0) {
        auto error_message = MUST(String::formatted("{} is an invalid value for {}", sw == 0 ? *sw : *sh, sw == 0 ? "sw"sv : "sh"sv));
        auto error = JS::RangeError::create(realm, move(error_message));
        return WebIDL::create_rejected_promise(realm, move(error));
    }

    // FIXME:
    // 2. If either options's resizeWidth or options's resizeHeight is present and is 0, then return a promise rejected with an "InvalidStateError" DOMException.
    (void)options;

    // 3. Check the usability of the image argument. If this throws an exception or returns bad, then return a promise rejected with an "InvalidStateError" DOMException.
    auto error_promise = image.visit(
        [](GC::Root<FileAPI::Blob>&) -> Optional<GC::Ref<WebIDL::Promise>> {
            return {};
        },
        [](GC::Root<ImageData>&) -> Optional<GC::Ref<WebIDL::Promise>> {
            return {};
        },
        [&](auto& canvas_image_source) -> Optional<GC::Ref<WebIDL::Promise>> {
            // Note: "Check the usability of the image argument" is only defined for CanvasImageSource
            if (auto usability = check_usability_of_image(canvas_image_source); usability.is_error() or usability.value() == CanvasImageSourceUsability::Bad) {
                auto error = WebIDL::InvalidStateError::create(this_impl().realm(), "image argument is not usable"_string);
                return WebIDL::create_rejected_promise_from_exception(realm, error);
            }

            return {};
        });

    if (error_promise.has_value()) {
        return error_promise.release_value();
    }

    // 4. Let p be a new promise.
    auto p = WebIDL::create_promise(realm);

    // 5. Let imageBitmap be a new ImageBitmap object.
    auto image_bitmap = ImageBitmap::create(this_impl().realm());

    // 6. Switch on image:
    image.visit(
        [&](GC::Root<FileAPI::Blob>& blob) {
            // Run these step in parallel:
            Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [=]() {
                // 1. Let imageData be the result of reading image's data. If an error occurs during reading of the
                // object, then reject p with an "InvalidStateError" DOMException and abort these steps.
                // FIXME: I guess this is always fine for us as the data is already read.
                auto const image_data = blob->raw_bytes();

                // FIXME:
                // 2. Apply the image sniffing rules to determine the file format of imageData, with MIME type of
                // image (as given by image's type attribute) giving the official type.

                auto on_failed_decode = [p = GC::Root(*p)](Error&) {
                    // 3. If imageData is not in a supported image file format (e.g., it's not an image at all), or if
                    // imageData is corrupted in some fatal way such that the image dimensions cannot be obtained
                    // (e.g., a vector graphic with no natural size), then reject p with an "InvalidStateError" DOMException
                    // and abort these steps.
                    auto& realm = relevant_realm(p->promise());
                    TemporaryExecutionContext context { relevant_realm(p->promise()), TemporaryExecutionContext::CallbacksEnabled::Yes };
                    WebIDL::reject_promise(realm, *p, WebIDL::InvalidStateError::create(realm, "image does not contain a supported image format"_string));
                };

                auto on_successful_decode = [image_bitmap = GC::Root(*image_bitmap), p = GC::Root(*p)](Web::Platform::DecodedImage& result) -> ErrorOr<void> {
                    // 4. Set imageBitmap's bitmap data to imageData, cropped to the source rectangle with formatting.
                    // If this is an animated image, imageBitmap's bitmap data must only be taken from the default image
                    // of the animation (the one that the format defines is to be used when animation is not supported
                    // or is disabled), or, if there is no such image, the first frame of the animation.
                    image_bitmap->set_bitmap(result.frames.take_first().bitmap);

                    auto& realm = relevant_realm(p->promise());

                    // 5. Resolve p with imageBitmap.
                    TemporaryExecutionContext context { relevant_realm(*image_bitmap), TemporaryExecutionContext::CallbacksEnabled::Yes };
                    WebIDL::resolve_promise(realm, *p, image_bitmap);
                    return {};
                };

                (void)Web::Platform::ImageCodecPlugin::the().decode_image(image_data, move(on_successful_decode), move(on_failed_decode));
            }));
        },
        [&](auto&) {
            dbgln("(STUBBED) createImageBitmap() for non-blob types");
            (void)sx;
            (void)sy;
            auto error = JS::Error::create(realm, "Not Implemented: createImageBitmap() for non-blob types"sv);
            TemporaryExecutionContext context { relevant_realm(p->promise()), TemporaryExecutionContext::CallbacksEnabled::Yes };
            WebIDL::reject_promise(realm, *p, error);
        });

    // 7. Return p.
    return p;
}

GC::Ref<WebIDL::Promise> WindowOrWorkerGlobalScopeMixin::fetch(Fetch::RequestInfo const& input, Fetch::RequestInit const& init) const
{
    auto& vm = this_impl().vm();
    return Fetch::fetch(vm, input, init);
}

// https://html.spec.whatwg.org/multipage/timers-and-user-prompts.html#dom-settimeout
i32 WindowOrWorkerGlobalScopeMixin::set_timeout(TimerHandler handler, i32 timeout, GC::RootVector<JS::Value> arguments)
{
    return run_timer_initialization_steps(move(handler), timeout, move(arguments), Repeat::No);
}

// https://html.spec.whatwg.org/multipage/timers-and-user-prompts.html#dom-setinterval
i32 WindowOrWorkerGlobalScopeMixin::set_interval(TimerHandler handler, i32 timeout, GC::RootVector<JS::Value> arguments)
{
    return run_timer_initialization_steps(move(handler), timeout, move(arguments), Repeat::Yes);
}

// https://html.spec.whatwg.org/multipage/timers-and-user-prompts.html#dom-cleartimeout
void WindowOrWorkerGlobalScopeMixin::clear_timeout(i32 id)
{
    if (auto timer = m_timers.get(id); timer.has_value())
        timer.value()->stop();
    m_timers.remove(id);
}

// https://html.spec.whatwg.org/multipage/timers-and-user-prompts.html#dom-clearinterval
void WindowOrWorkerGlobalScopeMixin::clear_interval(i32 id)
{
    if (auto timer = m_timers.get(id); timer.has_value())
        timer.value()->stop();
    m_timers.remove(id);
}

void WindowOrWorkerGlobalScopeMixin::clear_map_of_active_timers()
{
    for (auto& it : m_timers)
        it.value->stop();
    m_timers.clear();
}

// https://html.spec.whatwg.org/multipage/timers-and-user-prompts.html#timer-initialisation-steps
// With no active script fix from https://github.com/whatwg/html/pull/9712
i32 WindowOrWorkerGlobalScopeMixin::run_timer_initialization_steps(TimerHandler handler, i32 timeout, GC::RootVector<JS::Value> arguments, Repeat repeat, Optional<i32> previous_id)
{
    // 1. Let thisArg be global if that is a WorkerGlobalScope object; otherwise let thisArg be the WindowProxy that corresponds to global.

    // 2. If previousId was given, let id be previousId; otherwise, let id be an implementation-defined integer that is greater than zero and does not already exist in global's map of setTimeout and setInterval IDs.
    auto id = previous_id.has_value() ? previous_id.value() : m_timer_id_allocator.allocate();

    // FIXME: 3. If the surrounding agent's event loop's currently running task is a task that was created by this algorithm, then let nesting level be the task's timer nesting level. Otherwise, let nesting level be zero.

    // 4. If timeout is less than 0, then set timeout to 0.
    if (timeout < 0)
        timeout = 0;

    // FIXME: 5. If nesting level is greater than 5, and timeout is less than 4, then set timeout to 4.
    // FIXME: 6. Let realm be global's relevant realm.

    // 7. Let initiating script be the active script.
    auto const* initiating_script = Web::Bindings::active_script();

    auto& vm = this_impl().vm();

    // FIXME 8. Let uniqueHandle be null.

    // 9. Let task be a task that runs the following substeps:
    auto task = GC::create_function(vm.heap(), Function<void()>([this, handler = move(handler), timeout, arguments = move(arguments), repeat, id, initiating_script, previous_id]() {
        // FIXME: 1. Assert: uniqueHandle is a unique internal value, not null.

        // 2. If id does not exist in global's map of setTimeout and setInterval IDs, then abort these steps.
        if (!m_timers.contains(id))
            return;

        // FIXME: 3. If global's map of setTimeout and setInterval IDs[id] does not equal uniqueHandle, then abort these steps.
        // FIXME: 4. Record timing info for timer handler given handler, global's relevant settings object, and repeat.

        handler.visit(
            // 5. If handler is a Function, then invoke handler given arguments and "report", and with callback this value set to thisArg.
            [&](GC::Root<WebIDL::CallbackType> const& callback) {
                (void)WebIDL::invoke_callback(*callback, &this_impl(), WebIDL::ExceptionBehavior::Report, arguments);
            },
            // 6. Otherwise:
            [&](String const& source) {
                // 1. If previousId was not given:
                if (!previous_id.has_value()) {
                    // 1. Let globalName be "Window" if global is a Window object; "WorkerGlobalScope" otherwise.
                    auto global_name = is<Window>(this_impl()) ? "Window"sv : "WorkerGlobalScope"sv;

                    // 2. Let methodName be "setInterval" if repeat is true; "setTimeout" otherwise.
                    auto method_name = repeat == Repeat::Yes ? "setInterval"sv : "setTimeout"sv;

                    // 3. Let sink be a concatenation of globalName, U+0020 SPACE, and methodName.
                    [[maybe_unused]] auto sink = String::formatted("{} {}", global_name, method_name);

                    // FIXME: 4. Set handler to the result of invoking the Get Trusted Type compliant string algorithm with TrustedScript, global, handler, sink, and "script".
                }

                // FIXME: 2. Assert: handler is a string.
                // FIXME: 3. Perform EnsureCSPDoesNotBlockStringCompilation(realm, « », handler, handler, timer, « », handler). If this throws an exception, catch it, report it for global, and abort these steps.

                // 4. Let settings object be global's relevant settings object.
                auto& settings_object = relevant_settings_object(this_impl());

                // 5. Let fetch options be the default classic script fetch options.
                ScriptFetchOptions options {};

                // 6. Let base URL be settings object's API base URL.
                auto base_url = settings_object.api_base_url();

                // 7. If initiating script is not null, then:
                if (initiating_script) {
                    // FIXME: 1. Set fetch options to a script fetch options whose cryptographic nonce is initiating script's fetch options's cryptographic nonce,
                    //           integrity metadata is the empty string, parser metadata is "not-parser-inserted", credentials mode is initiating script's fetch
                    //           options's credentials mode, referrer policy is initiating script's fetch options's referrer policy, and fetch priority is "auto".

                    // 2. Set base URL to initiating script's base URL.
                    base_url = initiating_script->base_url().value();

                    // Spec Note: The effect of these steps ensures that the string compilation done by setTimeout() and setInterval() behaves equivalently to that
                    //            done by eval(). That is, module script fetches via import() will behave the same in both contexts.
                }

                // 8. Let script be the result of creating a classic script given handler, realm, base URL, and fetch options.
                // FIXME: Pass fetch options.
                auto basename = base_url.basename();
                auto script = ClassicScript::create(basename, source, this_impl().realm(), move(base_url));

                // 9. Run the classic script script.
                (void)script->run();
            });

        // 7. If id does not exist in global's map of setTimeout and setInterval IDs, then abort these steps.
        if (!m_timers.contains(id))
            return;

        // FIXME: 8. If global's map of setTimeout and setInterval IDs[id] does not equal uniqueHandle, then abort these steps.

        switch (repeat) {
        // 9. If repeat is true, then perform the timer initialization steps again, given global, handler, timeout, arguments, true, and id.
        case Repeat::Yes:
            run_timer_initialization_steps(handler, timeout, move(arguments), repeat, id);
            break;

        // 10. Otherwise, remove global's map of active timers[id].
        case Repeat::No:
            m_timers.remove(id);
            break;
        }
    }));

    // FIXME: 10. Increment nesting level by one.
    // FIXME: 11. Set task's timer nesting level to nesting level.

    // 12. Let completionStep be an algorithm step which queues a global task on the timer task source given global to run task.
    Function<void()> completion_step = [this, task = move(task)]() mutable {
        queue_global_task(Task::Source::TimerTask, this_impl(), GC::create_function(this_impl().heap(), [this, task] {
            HTML::TemporaryExecutionContext execution_context { this_impl().realm(), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
            task->function()();
        }));
    };

    // 13. Set uniqueHandle to the result of running steps after a timeout given global, "setTimeout/setInterval", timeout, completionStep.
    //     FIXME: run_steps_after_a_timeout() needs to be updated to return a unique internal value that can be used here.
    run_steps_after_a_timeout_impl(timeout, move(completion_step), id);

    // FIXME: 14. Set global's map of setTimeout and setInterval IDs[id] to uniqueHandle.

    // 15. Return id.
    return id;
}

// 1. https://www.w3.org/TR/performance-timeline/#dfn-relevant-performance-entry-tuple
PerformanceTimeline::PerformanceEntryTuple& WindowOrWorkerGlobalScopeMixin::relevant_performance_entry_tuple(FlyString const& entry_type)
{
    // 1. Let map be the performance entry buffer map associated with globalObject.
    // 2. Return the result of getting the value of an entry from map, given entryType as the key.
    auto tuple = m_performance_entry_buffer_map.get(entry_type);

    // This shouldn't be called with entry types that aren't in `ENUMERATE_SUPPORTED_PERFORMANCE_ENTRY_TYPES`.
    VERIFY(tuple.has_value());
    return tuple.value();
}

// https://www.w3.org/TR/performance-timeline/#dfn-queue-a-performanceentry
void WindowOrWorkerGlobalScopeMixin::queue_performance_entry(GC::Ref<PerformanceTimeline::PerformanceEntry> new_entry)
{
    // 1. Let interested observers be an initially empty set of PerformanceObserver objects.
    Vector<GC::Root<PerformanceTimeline::PerformanceObserver>> interested_observers;

    // 2. Let entryType be newEntry’s entryType value.
    auto const& entry_type = new_entry->entry_type();

    // 3. Let relevantGlobal be newEntry's relevant global object.
    // NOTE: Already is `this`.

    // 4. For each registered performance observer regObs in relevantGlobal's list of registered performance observer
    //    objects:
    for (auto const& registered_observer : m_registered_performance_observer_objects) {
        // 1. If regObs's options list contains a PerformanceObserverInit options whose entryTypes member includes entryType
        //    or whose type member equals to entryType:
        auto iterator = registered_observer->options_list().find_if([&entry_type](PerformanceTimeline::PerformanceObserverInit const& entry) {
            if (entry.entry_types.has_value())
                return entry.entry_types->contains_slow(entry_type.to_string());

            VERIFY(entry.type.has_value());
            return entry.type.value() == entry_type;
        });

        if (!iterator.is_end()) {
            // 1. If should add entry with newEntry and options returns true, append regObs's observer to interested observers.
            if (new_entry->should_add_entry(*iterator) == PerformanceTimeline::ShouldAddEntry::Yes)
                interested_observers.append(registered_observer);
        }
    }

    // 5. For each observer in interested observers:
    for (auto const& observer : interested_observers) {
        // 1. Append newEntry to observer's observer buffer.
        observer->append_to_observer_buffer({}, new_entry);
    }

    // AD-HOC: Steps 6-9 are not here because other engines do not add to the performance entry buffer when queuing
    //         the performance observer task. The users of the Performance Timeline specification also do not expect
    //         this function to add to the entry buffer, instead queuing the observer task, then adding to the entry
    //         buffer separately.

    // 10. Queue the PerformanceObserver task with relevantGlobal as input.
    queue_the_performance_observer_task();
}

// https://www.w3.org/TR/performance-timeline/#dfn-queue-a-performanceentry
// AD-HOC: This is a separate function because the users of this specification queues PerformanceObserver tasks and add
//         to the entry buffer separately.
void WindowOrWorkerGlobalScopeMixin::add_performance_entry(GC::Ref<PerformanceTimeline::PerformanceEntry> new_entry, CheckIfPerformanceBufferIsFull check_if_performance_buffer_is_full)
{
    // 6. Let tuple be the relevant performance entry tuple of entryType and relevantGlobal.
    auto& tuple = relevant_performance_entry_tuple(new_entry->entry_type());

    // AD-HOC: We have a custom flag to always append to the buffer by default, as other performance specs do this by default
    //         (either they don't have a limit, or they check the limit themselves). This flag allows compatibility for specs
    //         that rely do and don't rely on this.
    bool is_buffer_full = false;
    auto should_add = PerformanceTimeline::ShouldAddEntry::Yes;

    if (check_if_performance_buffer_is_full == CheckIfPerformanceBufferIsFull::Yes) {
        // 7. Let isBufferFull be the return value of the determine if a performance entry buffer is full algorithm with tuple
        //    as input.
        is_buffer_full = tuple.is_full();

        // 8. Let shouldAdd be the result of should add entry with newEntry as input.
        should_add = new_entry->should_add_entry();
    }

    // 9. If isBufferFull is false and shouldAdd is true, append newEntry to tuple's performance entry buffer.
    if (!is_buffer_full && should_add == PerformanceTimeline::ShouldAddEntry::Yes)
        tuple.performance_entry_buffer.append(new_entry);
}

void WindowOrWorkerGlobalScopeMixin::clear_performance_entry_buffer(Badge<HighResolutionTime::Performance>, FlyString const& entry_type)
{
    auto& tuple = relevant_performance_entry_tuple(entry_type);
    tuple.performance_entry_buffer.clear();
}

void WindowOrWorkerGlobalScopeMixin::remove_entries_from_performance_entry_buffer(Badge<HighResolutionTime::Performance>, FlyString const& entry_type, String entry_name)
{
    auto& tuple = relevant_performance_entry_tuple(entry_type);
    tuple.performance_entry_buffer.remove_all_matching([&entry_name](GC::Root<PerformanceTimeline::PerformanceEntry> const& entry) {
        return entry->name() == entry_name;
    });
}

// https://www.w3.org/TR/performance-timeline/#dfn-filter-buffer-map-by-name-and-type
ErrorOr<Vector<GC::Root<PerformanceTimeline::PerformanceEntry>>> WindowOrWorkerGlobalScopeMixin::filter_buffer_map_by_name_and_type(Optional<String> name, Optional<String> type) const
{
    // 1. Let result be an initially empty list.
    Vector<GC::Root<PerformanceTimeline::PerformanceEntry>> result;

    // 2. Let map be the performance entry buffer map associated with the relevant global object of this.
    auto const& map = m_performance_entry_buffer_map;

    // 3. Let tuple list be an empty list.
    Vector<PerformanceTimeline::PerformanceEntryTuple const&> tuple_list;

    // 4. If type is not null, append the result of getting the value of entry on map given type as key to tuple list.
    //    Otherwise, assign the result of get the values on map to tuple list.
    if (type.has_value()) {
        auto maybe_tuple = map.get(type.value());
        if (maybe_tuple.has_value())
            TRY(tuple_list.try_append(maybe_tuple.release_value()));
    } else {
        for (auto const& it : map)
            TRY(tuple_list.try_append(it.value));
    }

    // 5. For each tuple in tuple list, run the following steps:
    for (auto const& tuple : tuple_list) {
        // 1. Let buffer be tuple's performance entry buffer.
        auto const& buffer = tuple.performance_entry_buffer;

        // 2. If tuple's availableFromTimeline is false, continue to the next tuple.
        if (tuple.available_from_timeline == PerformanceTimeline::AvailableFromTimeline::No)
            continue;

        // 3. Let entries be the result of running filter buffer by name and type with buffer, name and type as inputs.
        auto entries = TRY(filter_buffer_by_name_and_type(buffer, name, type));

        // 4. For each entry in entries, append entry to result.
        TRY(result.try_extend(entries));
    }

    // 6. Sort results's entries in chronological order with respect to startTime
    quick_sort(result, [](auto const& left_entry, auto const& right_entry) {
        return left_entry->start_time() < right_entry->start_time();
    });

    // 7. Return result.
    return result;
}

void WindowOrWorkerGlobalScopeMixin::register_performance_observer(Badge<PerformanceTimeline::PerformanceObserver>, GC::Ref<PerformanceTimeline::PerformanceObserver> observer)
{
    m_registered_performance_observer_objects.set(observer, AK::HashSetExistingEntryBehavior::Keep);
}

void WindowOrWorkerGlobalScopeMixin::unregister_performance_observer(Badge<PerformanceTimeline::PerformanceObserver>, GC::Ref<PerformanceTimeline::PerformanceObserver> observer)
{
    m_registered_performance_observer_objects.remove(observer);
}

bool WindowOrWorkerGlobalScopeMixin::has_registered_performance_observer(GC::Ref<PerformanceTimeline::PerformanceObserver> observer)
{
    return m_registered_performance_observer_objects.contains(observer);
}

// https://w3c.github.io/performance-timeline/#dfn-queue-the-performanceobserver-task
void WindowOrWorkerGlobalScopeMixin::queue_the_performance_observer_task()
{
    // 1. If relevantGlobal's performance observer task queued flag is set, terminate these steps.
    if (m_performance_observer_task_queued)
        return;

    // 2. Set relevantGlobal's performance observer task queued flag.
    m_performance_observer_task_queued = true;

    // 3. Queue a task that consists of running the following substeps. The task source for the queued task is the performance
    //    timeline task source.
    queue_global_task(Task::Source::PerformanceTimeline, this_impl(), GC::create_function(this_impl().heap(), [this]() {
        auto& realm = this_impl().realm();
        HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

        // 1. Unset performance observer task queued flag of relevantGlobal.
        m_performance_observer_task_queued = false;

        // 2. Let notifyList be a copy of relevantGlobal's list of registered performance observer objects.
        auto notify_list = m_registered_performance_observer_objects;

        // 3. For each registered performance observer object registeredObserver in notifyList, run these steps:
        for (auto& registered_observer : notify_list) {
            // 1. Let po be registeredObserver's observer.
            // 2. Let entries be a copy of po’s observer buffer.
            // 4. Empty po’s observer buffer.
            auto entries = registered_observer->take_records();

            // 3. If entries is empty, return.
            // FIXME: Do they mean `continue`?
            if (entries.is_empty())
                continue;

            Vector<GC::Ref<PerformanceTimeline::PerformanceEntry>> entries_as_gc_ptrs;
            for (auto& entry : entries)
                entries_as_gc_ptrs.append(*entry);

            // 5. Let observerEntryList be a new PerformanceObserverEntryList, with its entry list set to entries.
            auto observer_entry_list = realm.create<PerformanceTimeline::PerformanceObserverEntryList>(realm, move(entries_as_gc_ptrs));

            // 6. Let droppedEntriesCount be null.
            Optional<u64> dropped_entries_count;

            // 7. If po's requires dropped entries is set, perform the following steps:
            if (registered_observer->requires_dropped_entries()) {
                // 1. Set droppedEntriesCount to 0.
                dropped_entries_count = 0;

                // 2. For each PerformanceObserverInit item in registeredObserver's options list:
                for (auto const& item : registered_observer->options_list()) {
                    // 1. For each DOMString entryType that appears either as item's type or in item's entryTypes:
                    auto increment_dropped_entries_count = [this, &dropped_entries_count](FlyString const& type) {
                        // 1. Let map be relevantGlobal's performance entry buffer map.
                        auto const& map = m_performance_entry_buffer_map;

                        // 2. Let tuple be the result of getting the value of entry on map given entryType as key.
                        auto const& tuple = map.get(type);
                        VERIFY(tuple.has_value());

                        // 3. Increase droppedEntriesCount by tuple's dropped entries count.
                        dropped_entries_count.value() += tuple->dropped_entries_count;
                    };

                    if (item.type.has_value()) {
                        increment_dropped_entries_count(item.type.value());
                    } else {
                        VERIFY(item.entry_types.has_value());
                        for (auto const& type : item.entry_types.value())
                            increment_dropped_entries_count(type);
                    }
                }

                // 3. Set po's requires dropped entries to false.
                registered_observer->unset_requires_dropped_entries({});
            }

            // 8. Let callbackOptions be a PerformanceObserverCallbackOptions with its droppedEntriesCount set to
            //    droppedEntriesCount if droppedEntriesCount is not null, otherwise unset.
            auto callback_options = JS::Object::create(realm, realm.intrinsics().object_prototype());
            if (dropped_entries_count.has_value())
                MUST(callback_options->create_data_property("droppedEntriesCount"_fly_string, JS::Value(dropped_entries_count.value())));

            // 9. Call po’s observer callback with observerEntryList as the first argument, with po as the second
            //    argument and as callback this value, and with callbackOptions as the third argument.
            //    If this throws an exception, report the exception.
            auto completion = WebIDL::invoke_callback(registered_observer->callback(), registered_observer, observer_entry_list, registered_observer, callback_options);
            if (completion.is_abrupt())
                HTML::report_exception(completion, realm);
        }
    }));
}

// https://w3c.github.io/resource-timing/#dfn-add-a-performanceresourcetiming-entry
void WindowOrWorkerGlobalScopeMixin::add_resource_timing_entry(Badge<ResourceTiming::PerformanceResourceTiming>, GC::Ref<ResourceTiming::PerformanceResourceTiming> entry)
{
    // 1. If can add resource timing entry returns true and resource timing buffer full event pending flag is false,
    //    run the following substeps:
    if (can_add_resource_timing_entry() && !m_resource_timing_buffer_full_event_pending) {
        // a. Add new entry to the performance entry buffer.
        // b. Increase resource timing buffer current size by 1.
        add_performance_entry(entry);

        // c. Return.
        return;
    }

    // 2. If resource timing buffer full event pending flag is false, run the following substeps:
    if (!m_resource_timing_buffer_full_event_pending) {
        // a. Set resource timing buffer full event pending flag to true.
        m_resource_timing_buffer_full_event_pending = true;

        // b. Queue a task on the performance timeline task source to run fire a buffer full event.
        HTML::queue_a_task(HTML::Task::Source::PerformanceTimeline, nullptr, nullptr, GC::create_function(this_impl().heap(), [this] {
            fire_resource_timing_buffer_full_event();
        }));
    }

    // 3. Add new entry to the resource timing secondary buffer.
    // 4. Increase resource timing secondary buffer current size by 1.
    m_resource_timing_secondary_buffer.append(entry);
}

// https://w3c.github.io/resource-timing/#dfn-can-add-resource-timing-entry
bool WindowOrWorkerGlobalScopeMixin::can_add_resource_timing_entry()
{
    // 1. If resource timing buffer current size is smaller than resource timing buffer size limit, return true.
    // 2. Return false.
    return resource_timing_buffer_current_size() < m_resource_timing_buffer_size_limit;
}

// https://w3c.github.io/resource-timing/#dfn-resource-timing-buffer-current-size
size_t WindowOrWorkerGlobalScopeMixin::resource_timing_buffer_current_size()
{
    // A resource timing buffer current size which is initially 0.
    auto resource_timing_tuple = relevant_performance_entry_tuple(PerformanceTimeline::EntryTypes::resource);
    return resource_timing_tuple.performance_entry_buffer.size();
}

// https://w3c.github.io/resource-timing/#dfn-fire-a-buffer-full-event
void WindowOrWorkerGlobalScopeMixin::fire_resource_timing_buffer_full_event()
{
    // 1. While resource timing secondary buffer is not empty, run the following substeps:
    while (!m_resource_timing_secondary_buffer.is_empty()) {
        // 1. Let number of excess entries before be resource timing secondary buffer current size.
        auto number_of_excess_entries_before = m_resource_timing_secondary_buffer.size();

        // 2. If can add resource timing entry returns false, then fire an event named resourcetimingbufferfull at the Performance object.
        if (!can_add_resource_timing_entry()) {
            auto full_event = DOM::Event::create(this_impl().realm(), PerformanceTimeline::EventNames::resourcetimingbufferfull);
            performance()->dispatch_event(full_event);
        }

        // 3. Run copy secondary buffer.
        copy_resource_timing_secondary_buffer();

        // 4. Let number of excess entries after be resource timing secondary buffer current size.
        auto number_of_excess_entries_after = m_resource_timing_secondary_buffer.size();

        // 5. If number of excess entries before is lower than or equals number of excess entries after, then remove
        //    all entries from resource timing secondary buffer, set resource timing secondary buffer current size to
        //    0, and abort these steps.
        if (number_of_excess_entries_before <= number_of_excess_entries_after) {
            m_resource_timing_secondary_buffer.clear();
            break;
        }
    }

    // 2. Set resource timing buffer full event pending flag to false.
    m_resource_timing_buffer_full_event_pending = false;
}

// https://w3c.github.io/resource-timing/#dfn-copy-secondary-buffer
void WindowOrWorkerGlobalScopeMixin::copy_resource_timing_secondary_buffer()
{
    // 1. While resource timing secondary buffer is not empty and can add resource timing entry returns true,
    //    run the following substeps:
    while (!m_resource_timing_secondary_buffer.is_empty() && can_add_resource_timing_entry()) {
        // 1. Let entry be the oldest PerformanceResourceTiming in resource timing secondary buffer.
        // 2. Add entry to the end of performance entry buffer.
        // 3. Increment resource timing buffer current size by 1.
        // 4. Remove entry from resource timing secondary buffer.
        // 5. Decrement resource timing secondary buffer current size by 1.
        auto entry = m_resource_timing_secondary_buffer.take_first();
        auto& resource_tuple = relevant_performance_entry_tuple(PerformanceTimeline::EntryTypes::resource);
        resource_tuple.performance_entry_buffer.append(entry);
    }
}

void WindowOrWorkerGlobalScopeMixin::register_event_source(Badge<EventSource>, GC::Ref<EventSource> event_source)
{
    m_registered_event_sources.set(event_source);
}

void WindowOrWorkerGlobalScopeMixin::unregister_event_source(Badge<EventSource>, GC::Ref<EventSource> event_source)
{
    m_registered_event_sources.remove(event_source);
}

void WindowOrWorkerGlobalScopeMixin::forcibly_close_all_event_sources()
{
    for (auto event_source : m_registered_event_sources)
        event_source->forcibly_close();
}

void WindowOrWorkerGlobalScopeMixin::register_web_socket(Badge<WebSockets::WebSocket>, GC::Ref<WebSockets::WebSocket> web_socket)
{
    m_registered_web_sockets.append(web_socket);
}

void WindowOrWorkerGlobalScopeMixin::unregister_web_socket(Badge<WebSockets::WebSocket>, GC::Ref<WebSockets::WebSocket> web_socket)
{
    m_registered_web_sockets.remove(web_socket);
}

WindowOrWorkerGlobalScopeMixin::AffectedAnyWebSockets WindowOrWorkerGlobalScopeMixin::make_disappear_all_web_sockets()
{
    auto affected_any_web_sockets = AffectedAnyWebSockets::No;

    for (auto& web_socket : m_registered_web_sockets) {
        web_socket.make_disappear();
        affected_any_web_sockets = AffectedAnyWebSockets::Yes;
    }

    return affected_any_web_sockets;
}

// https://html.spec.whatwg.org/multipage/timers-and-user-prompts.html#run-steps-after-a-timeout
void WindowOrWorkerGlobalScopeMixin::run_steps_after_a_timeout(i32 timeout, Function<void()> completion_step)
{
    return run_steps_after_a_timeout_impl(timeout, move(completion_step));
}

void WindowOrWorkerGlobalScopeMixin::run_steps_after_a_timeout_impl(i32 timeout, Function<void()> completion_step, Optional<i32> timer_key)
{
    // 1. Assert: if timerKey is given, then the caller of this algorithm is the timer initialization steps. (Other specifications must not pass timerKey.)
    // Note: This is enforced by the caller.

    // 2. If timerKey is not given, then set it to a new unique non-numeric value.
    if (!timer_key.has_value())
        timer_key = m_timer_id_allocator.allocate();

    // FIXME: 3. Let startTime be the current high resolution time given global.
    auto timer = Timer::create(this_impl(), timeout, move(completion_step), timer_key.value());

    // FIXME: 4. Set global's map of active timers[timerKey] to startTime plus milliseconds.
    m_timers.set(timer_key.value(), timer);

    // FIXME: 5. Run the following steps in parallel:
    // FIXME:    1. If global is a Window object, wait until global's associated Document has been fully active for a further milliseconds milliseconds (not necessarily consecutively).
    //              Otherwise, global is a WorkerGlobalScope object; wait until milliseconds milliseconds have passed with the worker not suspended (not necessarily consecutively).
    // FIXME:    2. Wait until any invocations of this algorithm that had the same global and orderingIdentifier, that started before this one, and whose milliseconds is equal to or less than this one's, have completed.
    // FIXME:    3. Optionally, wait a further implementation-defined length of time.
    // FIXME:    4. Perform completionSteps.
    // FIXME:    5. If timerKey is a non-numeric value, remove global's map of active timers[timerKey].

    timer->start();
}

// https://w3c.github.io/hr-time/#dom-windoworworkerglobalscope-performance
GC::Ref<HighResolutionTime::Performance> WindowOrWorkerGlobalScopeMixin::performance()
{
    auto& realm = this_impl().realm();
    if (!m_performance)
        m_performance = realm.create<HighResolutionTime::Performance>(realm);
    return GC::Ref { *m_performance };
}

GC::Ref<IndexedDB::IDBFactory> WindowOrWorkerGlobalScopeMixin::indexed_db()
{
    auto& realm = this_impl().realm();

    if (!m_indexed_db)
        m_indexed_db = realm.create<IndexedDB::IDBFactory>(realm);
    return *m_indexed_db;
}

// https://w3c.github.io/performance-timeline/#dfn-frozen-array-of-supported-entry-types
GC::Ref<JS::Object> WindowOrWorkerGlobalScopeMixin::supported_entry_types() const
{
    // Each global object has an associated frozen array of supported entry types, which is initialized to the
    // FrozenArray created from the sequence of strings among the registry that are supported for the global
    // object, in alphabetical order.
    auto& vm = this_impl().vm();
    auto& realm = this_impl().realm();

    if (!m_supported_entry_types_array) {
        GC::RootVector<JS::Value> supported_entry_types(vm.heap());

#define __ENUMERATE_SUPPORTED_PERFORMANCE_ENTRY_TYPES(entry_type, cpp_class) \
    supported_entry_types.append(JS::PrimitiveString::create(vm, entry_type));
        ENUMERATE_SUPPORTED_PERFORMANCE_ENTRY_TYPES
#undef __ENUMERATE_SUPPORTED_PERFORMANCE_ENTRY_TYPES

        m_supported_entry_types_array = JS::Array::create_from(realm, supported_entry_types);
        MUST(m_supported_entry_types_array->set_integrity_level(JS::Object::IntegrityLevel::Frozen));
    }

    return *m_supported_entry_types_array;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#dom-reporterror
void WindowOrWorkerGlobalScopeMixin::report_error(JS::Value e)
{
    // The reportError(e) method steps are to report an exception e for this.
    report_an_exception(e);
}

// https://html.spec.whatwg.org/multipage/webappapis.html#extract-error
struct ErrorInformation {
    String message;
    String filename;
    JS::Value error;
    size_t lineno { 0 };
    size_t colno { 0 };
};

// https://html.spec.whatwg.org/multipage/webappapis.html#extract-error
static ErrorInformation extract_error_information(JS::VM& vm, JS::Value exception)
{
    // 1. Let attributes be an empty map keyed by IDL attributes.
    ErrorInformation attributes;

    // 2. Set attributes[error] to exception.
    attributes.error = exception;

    // 3. Set attributes[message], attributes[filename], attributes[lineno], and attributes[colno] to
    //    implementation-defined values derived from exception.
    attributes.message = [&] {
        if (exception.is_object()) {
            auto& object = exception.as_object();
            if (MUST(object.has_own_property(vm.names.message))) {
                auto message = object.get_without_side_effects(vm.names.message);
                return message.to_string_without_side_effects();
            }
        }

        return MUST(String::formatted("Uncaught exception: {}", exception.to_string_without_side_effects()));
    }();

    // FIXME: This offset is relative to the javascript source. Other browsers appear to do it relative
    //        to the entire source document! Calculate that somehow.

    // If we got an Error object, then try and extract the information from the location the object was made.
    if (exception.is_object() && is<JS::Error>(exception.as_object())) {
        auto const& error = static_cast<JS::Error&>(exception.as_object());
        for (auto const& frame : error.traceback()) {
            auto source_range = frame.source_range();
            if (source_range.start.line != 0 || source_range.start.column != 0) {
                attributes.filename = MUST(String::from_byte_string(source_range.filename()));
                attributes.lineno = source_range.start.line;
                attributes.colno = source_range.start.column;
                break;
            }
        }
    }
    // Otherwise, we fall back to try and find the location of the invocation of the function itself.
    else {
        for (ssize_t i = vm.execution_context_stack().size() - 1; i >= 0; --i) {
            auto& frame = vm.execution_context_stack()[i];
            if (frame->executable && frame->program_counter.has_value()) {
                auto source_range = frame->executable->source_range_at(frame->program_counter.value()).realize();
                attributes.filename = MUST(String::from_byte_string(source_range.filename()));
                attributes.lineno = source_range.start.line;
                attributes.colno = source_range.start.column;
                break;
            }
        }
    }

    // 4. Return attributes.
    return attributes;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#report-an-exception
void WindowOrWorkerGlobalScopeMixin::report_an_exception(JS::Value exception, OmitError omit_error)
{
    auto& target = static_cast<DOM::EventTarget&>(this_impl());
    auto& realm = relevant_realm(target);
    auto& vm = realm.vm();

    // 1. Let notHandled be true.
    bool not_handled = true;

    // 2. Let errorInfo be the result of extracting error information from exception.
    auto error_info = extract_error_information(vm, exception);

    // 3. Let script be a script found in an implementation-defined way, or null. This should usually be the
    //    running script (most notably during run a classic script).
    auto script_or_module = vm.get_active_script_or_module();

    // 4. If script is a classic script and script's muted errors is true, then set errorInfo[error] to null,
    //    errorInfo[message] to "Script error.", errorInfo[filename] to the empty string, errorInfo[lineno] to
    //    0, and errorInfo[colno] to 0.
    script_or_module.visit(
        [&](GC::Ref<JS::Script> const& js_script) {
            if (as<ClassicScript>(js_script->host_defined())->muted_errors() == ClassicScript::MutedErrors::Yes) {
                error_info.error = JS::js_null();
                error_info.message = "Script error."_string;
                error_info.filename = String {};
                error_info.lineno = 0;
                error_info.colno = 0;
            }
        },
        [](auto const&) {});

    // 5. If omitError is true, then set errorInfo[error] to null.
    if (omit_error == OmitError::Yes)
        error_info.error = JS::js_null();

    // 6. If global is not in error reporting mode, then:
    if (!m_error_reporting_mode) {
        // 1. Set global's in error reporting mode to true.
        m_error_reporting_mode = true;

        // 2. If global implements EventTarget, then set notHandled to the result of firing an event named
        //    error at global, using ErrorEvent, with the cancelable attribute initialized to true, and
        //    additional attributes initialized according to errorInfo.
        ErrorEventInit event_init = {};
        event_init.cancelable = true;
        event_init.message = error_info.message;
        event_init.filename = error_info.filename;
        event_init.lineno = error_info.lineno;
        event_init.colno = error_info.colno;
        event_init.error = error_info.error;

        not_handled = target.dispatch_event(ErrorEvent::create(realm, EventNames::error, event_init));

        // 3. Set global's in error reporting mode to false.
        m_error_reporting_mode = false;
    }

    // 7. If notHandled is true, then:
    if (not_handled) {
        // 1. Set errorInfo[error] to null.
        error_info.error = JS::js_null();

        // FIXME: 2. If global implements DedicatedWorkerGlobalScope, queue a global task on the DOM manipulation
        //        task source with the global's associated Worker's relevant global object to run these steps:
        if (false) {
            // FIXME: 1. Let workerObject be the Worker object associated with global.

            // FIXME: 2. Set notHandled be the result of firing an event named error at workerObject, using ErrorEvent,
            //    with the cancelable attribute initialized to true, and additional attributes initialized
            //    according to errorInfo.

            // FIXME: 3. If notHandled is true, then report exception for workerObject's relevant global object with
            //    omitError set to true.
        }
        // 3. Otherwise, the user agent may report exception to a developer console.
        else {
            report_exception_to_console(exception, realm, ErrorInPromise::No);
        }
    }
}

// https://w3c.github.io/webcrypto/#dom-windoworworkerglobalscope-crypto
GC::Ref<Crypto::Crypto> WindowOrWorkerGlobalScopeMixin::crypto()
{
    auto& platform_object = this_impl();
    auto& realm = platform_object.realm();

    if (!m_crypto)
        m_crypto = realm.create<Crypto::Crypto>(realm);
    return GC::Ref { *m_crypto };
}

}
