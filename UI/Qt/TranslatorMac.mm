/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AtomicRefCounted.h>
#include <AK/HashMap.h>
#include <AK/RefPtr.h>
#include <LibCore/EventLoop.h>
#include <UI/Qt/Conversions.h>
#include <UI/Qt/TranslationServiceMac.h>
#include <UI/Qt/TranslatorMac.h>

#import <AppKit/AppKit.h>
#import <NaturalLanguage/NaturalLanguage.h>

namespace Ladybird {

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

        [LadybirdTranslationService statusesFrom:string_to_ns_string(source_language)
                                              to:targets
                                      completion:^(NSArray<NSNumber*>* statuses) {
                                          Vector<TargetLanguage> target_languages;
                                          for (NSUInteger index = 0; index < statuses.count && index < targets.count; ++index) {
                                              auto status = static_cast<LadybirdTranslationLanguageStatus>(statuses[index].integerValue);
                                              if (status == LadybirdTranslationLanguageStatusUnsupported)
                                                  continue;
                                              target_languages.append({ ns_string_to_string(targets[index]), status == LadybirdTranslationLanguageStatusNeedsDownload });
                                          }
                                          auto strong_event_loop = event_loop->take();
                                          if (!strong_event_loop)
                                              return;
                                          strong_event_loop->deferred_invoke([pending, id, target_languages = move(target_languages)]() mutable {
                                              if (auto callback = pending->callbacks.take(id); callback.has_value())
                                                  (*callback)(move(target_languages));
                                          });
                                      }];
    }

    virtual SessionId create_session(String const& source_language, String const& target_language) override
    {
        auto session_id = ++m_last_session_id;
        m_sessions.set(session_id, [[LadybirdTranslationSession alloc] initWithSource:string_to_ns_string(source_language) target:string_to_ns_string(target_language)]);
        return session_id;
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

    // NB: Translation languages are downloaded from the Language & Region pane.
    virtual void open_language_download_settings() override
    {
        [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:@"x-apple.systempreferences:com.apple.Localization-Settings.extension"]];
    }

private:
    NonnullRefPtr<PendingTranslations> m_pending_translations { adopt_ref(*new PendingTranslations) };
    NonnullRefPtr<PendingTargetLanguages> m_pending_target_languages { adopt_ref(*new PendingTargetLanguages) };
    HashMap<SessionId, LadybirdTranslationSession*> m_sessions;
    SessionId m_last_session_id { 0 };
};

OwnPtr<WebView::Translator> create_platform_translator()
{
    if (@available(macOS 26.4, *))
        return make<MacTranslator>();
    return nullptr;
}

}
