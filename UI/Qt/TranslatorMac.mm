/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AtomicRefCounted.h>
#include <AK/HashMap.h>
#include <AK/RefPtr.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <UI/Qt/Conversions.h>
#include <UI/Qt/TranslationServiceMac.h>
#include <UI/Qt/TranslatorMac.h>

#import <AppKit/AppKit.h>
#import <NaturalLanguage/NaturalLanguage.h>

// The public Translation API can't tell a language that's downloading from one that isn't installed, and never says
// when a download finishes. The framework's language statuses, which say both, are private API. The class and each
// selector are checked before use, so if the framework drops them, the browser just never shows a download under way.
@protocol LadybirdLanguageStatusObservation <NSObject>
@property (nonatomic, readonly) NSLocale* locale;
@property (nonatomic, readonly) NSInteger status;
@end

@protocol LadybirdLanguageStatus <NSObject>
- (instancetype)initWithTaskHint:(NSInteger)taskHint engineType:(NSInteger)engineType useDedicatedMachPort:(BOOL)useDedicatedMachPort observations:(void (^)(NSArray*))observations;
@end

namespace Ladybird {

// The values of a language status: not installed, being downloaded, and installed.
static constexpr NSInteger language_status_downloading = 1;
static constexpr NSInteger language_status_installed = 2;
// The task the statuses are for, the Translation API's own, and the kind of model, the classic ones.
static constexpr NSInteger language_status_task_translation_api = 8;
static constexpr NSInteger language_status_engine_classic = 1;
// The public API notices that a download finished a few seconds after the status does, so the pages ask again a few
// times after any change.
static constexpr auto language_status_follow_up_interval = AK::Duration::from_seconds(3);
static constexpr int language_status_follow_up_count = 4;
// 30 minutes at the follow-up interval.
static constexpr int language_poll_count = 600;

// A sample shorter than this says too little about its language, so the language the page declares decides.
static constexpr size_t minimum_detection_sample_length = 25;
// The detected language has to be more likely than this to be taken over the language the page declares.
static constexpr double minimum_detection_confidence = 0.7;

// Translations come back on an arbitrary queue. The callbacks that take them stay on the main thread, keyed by request,
// so only this atomically counted holder crosses threads.
struct PendingTranslations : public AtomicRefCounted<PendingTranslations> {
    struct Request {
        WebView::Translator::SessionId session_id { 0 };
        Function<void(Optional<Vector<Web::TranslationRun>>)> callback;
    };
    HashMap<u64, Request> requests;
    u64 next_id { 0 };
};

struct PendingTargetLanguages : public AtomicRefCounted<PendingTargetLanguages> {
    HashMap<u64, Function<void(Vector<WebView::Translator::TargetLanguage>)>> callbacks;
    u64 next_id { 0 };
};

struct LanguageStatuses : public AtomicRefCounted<LanguageStatuses> {
    // The status of each language, by its primary language subtag, e.g. "it".
    HashMap<String, NSInteger> statuses;
    Function<void()> on_change;
};

// Calls handler with every language's status on an arbitrary queue, now and whenever one changes, for as long as the
// object returned is alive. Returns nil if the private API isn't there.
static id start_observing_language_statuses(void (^handler)(HashMap<String, NSInteger>))
{
    Class status_class = NSClassFromString(@"_LTLanguageStatus");
    if (!status_class || ![status_class instancesRespondToSelector:@selector(initWithTaskHint:engineType:useDedicatedMachPort:observations:)])
        return nil;

    auto observations = ^(NSArray* observations) {
        HashMap<String, NSInteger> statuses;
        for (id<LadybirdLanguageStatusObservation> observation in observations) {
            if (![observation respondsToSelector:@selector(locale)] || ![observation respondsToSelector:@selector(status)])
                continue;
            NSLocale* locale = observation.locale;
            if (![locale isKindOfClass:NSLocale.class] || !locale.languageCode)
                continue;
            // A language is downloading if any of its locales is, e.g. "en_GB" as well as "en_US".
            auto language = ns_string_to_string(locale.languageCode);
            if (auto existing = statuses.get(language); !existing.has_value() || observation.status == language_status_downloading)
                statuses.set(language, observation.status);
        }
        handler(move(statuses));
    };

    // The dedicated port reports statuses without the download progress, which is all that apps without Apple's private
    // translation entitlement are allowed.
    id<LadybirdLanguageStatus> status = [status_class alloc];
    return [status initWithTaskHint:language_status_task_translation_api
                         engineType:language_status_engine_classic
               useDedicatedMachPort:YES
                       observations:observations];
}

API_AVAILABLE(macos(26.4))
static NSArray<LadybirdTranslationRun*>* ns_runs_from_runs(Vector<Web::TranslationRun> const& runs)
{
    auto* ns_runs = [NSMutableArray arrayWithCapacity:runs.size()];
    for (auto const& run : runs)
        [ns_runs addObject:[[LadybirdTranslationRun alloc] initWithText:utf16_string_to_ns_string(run.text) token:run.token keepAsIs:run.keep_as_is]];
    return ns_runs;
}

API_AVAILABLE(macos(26.4))
static Vector<Web::TranslationRun> runs_from_ns_runs(NSArray<LadybirdTranslationRun*>* ns_runs)
{
    Vector<Web::TranslationRun> runs;
    runs.ensure_capacity(ns_runs.count);
    for (LadybirdTranslationRun* run in ns_runs)
        runs.unchecked_append({ ns_string_to_utf16_string(run.text), run.token, static_cast<bool>(run.keepAsIs) });
    return runs;
}

class API_AVAILABLE(macos(26.4)) MacTranslator final : public WebView::Translator {
public:
    MacTranslator()
    {
        m_follow_up_timer = Core::Timer::create_repeating(language_status_follow_up_interval, [this] {
            if (--m_follow_ups_remaining <= 0)
                m_follow_up_timer->stop();
            if (on_languages_changed)
                on_languages_changed();
        });

        m_language_statuses->on_change = [this] { did_change_language_statuses(); };
        RefPtr<LanguageStatuses> language_statuses = m_language_statuses;
        RefPtr<Core::WeakEventLoopReference> event_loop = Core::EventLoop::current_weak();
        m_language_status_observer = start_observing_language_statuses(^(HashMap<String, NSInteger> statuses) {
            auto strong_event_loop = event_loop->take();
            if (!strong_event_loop)
                return;
            strong_event_loop->deferred_invoke([language_statuses, statuses = move(statuses)]() mutable {
                if (language_statuses->statuses == statuses)
                    return;
                language_statuses->statuses = move(statuses);
                if (language_statuses->on_change)
                    language_statuses->on_change();
            });
        });
    }

    virtual ~MacTranslator() override
    {
        m_language_statuses->on_change = nullptr;
    }

    virtual void detect_language(Web::TranslationLanguageSample const& sample, Function<void(Optional<String>)> on_complete) override
    {
        Optional<String> detected_language;
        if (sample.text.length_in_code_units() >= minimum_detection_sample_length) {
            auto* recognizer = [[NLLanguageRecognizer alloc] init];
            [recognizer processString:utf16_string_to_ns_string(sample.text)];
            NSDictionary<NLLanguage, NSNumber*>* hypotheses = [recognizer languageHypothesesWithMaximum:1];
            for (NLLanguage language in hypotheses) {
                if (hypotheses[language].doubleValue > minimum_detection_confidence && ![language isEqualToString:NLLanguageUndetermined])
                    detected_language = ns_string_to_string(language);
            }
        }
        on_complete(detected_language.has_value() ? move(detected_language) : sample.declared_language);
    }

    virtual void target_languages(String const& source_language, Vector<String> const& candidates, Function<void(Vector<TargetLanguage>)> on_complete) override
    {
        auto id = m_pending_target_languages->next_id++;
        m_pending_target_languages->callbacks.set(id, move(on_complete));
        RefPtr<PendingTargetLanguages> pending = m_pending_target_languages;
        RefPtr<Core::WeakEventLoopReference> event_loop = Core::EventLoop::current_weak();

        auto* targets = [NSMutableArray arrayWithCapacity:candidates.size()];
        for (auto const& candidate : candidates)
            [targets addObject:string_to_ns_string(candidate)];

        RefPtr<LanguageStatuses> language_statuses = m_language_statuses;
        auto source_language_code = String::from_utf8_without_validation(WebView::primary_language_subtag(source_language).bytes());

        [LadybirdTranslationService modelsFrom:string_to_ns_string(source_language)
                                            to:targets
                                    completion:^(NSArray<NSNumber*>* all_models) {
                                        Vector<String> target_language_tags;
                                        Vector<LadybirdTranslationModels> target_models;
                                        for (NSUInteger index = 0; index < all_models.count && index < targets.count; ++index) {
                                            target_language_tags.append(ns_string_to_string(targets[index]));
                                            target_models.append(static_cast<LadybirdTranslationModels>(all_models[index].integerValue));
                                        }
                                        auto strong_event_loop = event_loop->take();
                                        if (!strong_event_loop)
                                            return;
                                        // The statuses are only touched on the main thread.
                                        strong_event_loop->deferred_invoke([pending, id, language_statuses, source_language_code, target_language_tags = move(target_language_tags), target_models = move(target_models)]() mutable {
                                            auto callback = pending->callbacks.take(id);
                                            if (!callback.has_value())
                                                return;
                                            auto status_of = [&](StringView language) {
                                                return language_statuses->statuses.get(WebView::primary_language_subtag(language));
                                            };
                                            Vector<TargetLanguage> target_languages;
                                            for (size_t index = 0; index < target_language_tags.size(); ++index) {
                                                auto target_language = target_language_from_models(target_language_tags[index], target_models[index]);
                                                if (!target_language.has_value() || (!target_language->needs_download && !target_language->faster_after_download)) {
                                                    if (target_language.has_value())
                                                        target_languages.append(target_language.release_value());
                                                    continue;
                                                }
                                                for (auto const& language : { source_language_code, target_language_tags[index] }) {
                                                    auto status = status_of(language);
                                                    if (!status.has_value() || *status == language_status_installed)
                                                        continue;
                                                    target_language->languages_to_download.append(language);
                                                    if (*status == language_status_downloading)
                                                        target_language->downloading = true;
                                                }
                                                target_languages.append(target_language.release_value());
                                            }
                                            (*callback)(move(target_languages));
                                        });
                                    }];
    }

    virtual SessionId create_session(String const& source_language, String const& target_language, bool prefer_higher_quality) override
    {
        auto session_id = ++m_last_session_id;
        m_sessions.set(session_id, [[LadybirdTranslationSession alloc] initWithSource:string_to_ns_string(source_language) target:string_to_ns_string(target_language) preferHigherQuality:prefer_higher_quality]);
        return session_id;
    }

    virtual void choose_session_model_again(SessionId session_id, bool prefer_higher_quality) override
    {
        if (auto session = m_sessions.get(session_id); session.has_value())
            [*session chooseModelAgainWithPreferHigherQuality:prefer_higher_quality];
    }

    virtual void close_session(SessionId session_id) override
    {
        if (auto it = m_sessions.find(session_id); it != m_sessions.end()) {
            [it->value cancel];
            m_sessions.remove(it);
        }
        m_pending_translations->requests.remove_all_matching([&](auto, auto const& request) {
            return request.session_id == session_id;
        });
    }

    virtual void translate(SessionId session_id, Web::TranslationUnit unit, Function<void(Optional<Vector<Web::TranslationRun>>)> on_complete) override
    {
        auto session = m_sessions.find(session_id);
        if (session == m_sessions.end()) {
            on_complete({});
            return;
        }

        auto id = m_pending_translations->next_id++;
        m_pending_translations->requests.set(id, { session_id, move(on_complete) });
        RefPtr<PendingTranslations> pending = m_pending_translations;
        RefPtr<Core::WeakEventLoopReference> event_loop = Core::EventLoop::current_weak();

        [session->value translate:ns_runs_from_runs(unit.runs)
                       completion:^(NSArray<LadybirdTranslationRun*>* ns_runs) {
                           Optional<Vector<Web::TranslationRun>> runs;
                           if (ns_runs)
                               runs = runs_from_ns_runs(ns_runs);
                           auto strong_event_loop = event_loop->take();
                           if (!strong_event_loop)
                               return;
                           strong_event_loop->deferred_invoke([pending, id, runs = move(runs)]() mutable {
                               if (auto request = pending->requests.take(id); request.has_value())
                                   request->callback(move(runs));
                           });
                       }];
    }

    // Translation languages are downloaded from a sheet of the Language & Region pane. This anchor opens that sheet,
    // with a notice about translation and privacy on top; the pane's other anchors don't open it at all.
    virtual void open_language_download_settings() override
    {
        [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:@"x-apple.systempreferences:com.apple.Localization-Settings.extension?translation-privacy"]];

        // Without the private statuses, nothing says when the download is done, so the languages are checked again
        // every few seconds for as long as a download might take.
        if (!m_language_status_observer) {
            m_follow_ups_remaining = language_poll_count;
            m_follow_up_timer->restart();
        }
    }

    virtual Optional<StringView> higher_quality_models_name() const override { return "Apple Intelligence"sv; }

private:
    static Optional<TargetLanguage> target_language_from_models(String const& language, LadybirdTranslationModels models)
    {
        bool fast_supported = models & LadybirdTranslationModelsFastSupported;
        bool fast_installed = models & LadybirdTranslationModelsFastInstalled;
        bool higher_quality_supported = models & LadybirdTranslationModelsHigherQualitySupported;
        bool higher_quality_installed = models & LadybirdTranslationModelsHigherQualityInstalled;
        if (!fast_supported && !higher_quality_supported)
            return {};
        return TargetLanguage {
            .language = language,
            .needs_download = !fast_installed && !higher_quality_installed,
            .faster_after_download = higher_quality_installed && fast_supported && !fast_installed,
            .higher_quality_available = higher_quality_installed,
        };
    }

    void did_change_language_statuses()
    {
        if (on_languages_changed)
            on_languages_changed();
        m_follow_ups_remaining = language_status_follow_up_count;
        m_follow_up_timer->restart();
    }

    NonnullRefPtr<PendingTranslations> m_pending_translations { adopt_ref(*new PendingTranslations) };
    NonnullRefPtr<PendingTargetLanguages> m_pending_target_languages { adopt_ref(*new PendingTargetLanguages) };
    HashMap<SessionId, LadybirdTranslationSession*> m_sessions;
    SessionId m_last_session_id { 0 };

    NonnullRefPtr<LanguageStatuses> m_language_statuses { adopt_ref(*new LanguageStatuses) };
    id m_language_status_observer { nil };
    RefPtr<Core::Timer> m_follow_up_timer;
    int m_follow_ups_remaining { 0 };
};

OwnPtr<WebView::Translator> create_platform_translator()
{
    if (@available(macOS 26.4, *))
        return make<MacTranslator>();
    return nullptr;
}

}
