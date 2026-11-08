/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AtomicRefCounted.h>
#include <AK/HashMap.h>
#include <AK/RefPtr.h>
#include <LibCore/EventLoop.h>
#include <LibWebView/StringConversionsMacOS.h>
#include <UI/Qt/SpellCheckerMac.h>

#import <AppKit/AppKit.h>

// Inline predictions are private AppKit API. Each selector is checked with "respondsToSelector:" before use, so if
// AppKit drops one, predictions stop showing and nothing else changes.
@interface NSSpellChecker (LadybirdInlinePredictions)
@property (class, readonly, getter=isAutomaticInlineCompletionEnabled) BOOL automaticInlineCompletionEnabled;
- (NSTextCheckingResult*)completionCandidateFromCandidates:(NSArray<NSTextCheckingResult*>*)candidates;
@end

namespace Ladybird {

using namespace WebView;

// The private text checking type that asks for a single completion of the word at the caret.
static constexpr NSTextCheckingTypes text_checking_type_single_completion = 1ULL << 29;

// Answers come back on an arbitrary queue. The callbacks that take them stay on the main thread, keyed by request, so
// only this atomically counted holder crosses threads.
struct PendingChecks : public AtomicRefCounted<PendingChecks> {
    HashMap<u64, Function<void(Vector<Web::SpellingError>)>> callbacks;
    u64 next_id { 0 };
};

struct PendingSuggestions : public AtomicRefCounted<PendingSuggestions> {
    HashMap<u64, Function<void(Optional<Web::WritingSuggestion>)>> callbacks;
    u64 next_id { 0 };
};

class MacSpellChecker final : public WebView::SpellChecker {
public:
    virtual DocumentTag create_document_tag() override
    {
        return [NSSpellChecker uniqueSpellDocumentTag];
    }

    virtual void close_document_tag(DocumentTag tag) override
    {
        [[NSSpellChecker sharedSpellChecker] closeSpellDocumentWithTag:tag];
    }

    virtual void check(DocumentTag tag, Utf16String const& text, Function<void(Vector<Web::SpellingError>)> on_complete) override
    {
        auto id = m_pending_checks->next_id++;
        m_pending_checks->callbacks.set(id, move(on_complete));
        RefPtr<PendingChecks> pending_checks = m_pending_checks;
        RefPtr<Core::WeakEventLoopReference> event_loop = Core::EventLoop::current_weak();
        auto* string = utf16_string_to_ns_string(text);
        auto handler = ^(NSInteger, NSArray<NSTextCheckingResult*>* results, NSOrthography*, NSInteger) {
            Vector<Web::SpellingError> errors;
            for (NSTextCheckingResult* result in results) {
                if (result.resultType != NSTextCheckingTypeSpelling)
                    continue;
                errors.append({ static_cast<u32>(result.range.location), static_cast<u32>(result.range.length) });
            }
            auto strong_event_loop = event_loop->take();
            if (!strong_event_loop)
                return;
            strong_event_loop->deferred_invoke([pending_checks, id, errors = move(errors)]() mutable {
                if (auto callback = pending_checks->callbacks.take(id); callback.has_value())
                    (*callback)(move(errors));
            });
        };
        [[NSSpellChecker sharedSpellChecker] requestCheckingOfString:string
                                                               range:NSMakeRange(0, string.length)
                                                               types:NSTextCheckingTypeSpelling
                                                             options:nil
                                              inSpellDocumentWithTag:tag
                                                   completionHandler:handler];
    }

    virtual void guesses(DocumentTag tag, Utf16String const& word, Function<void(Vector<Utf16String>)> on_complete) override
    {
        auto* string = utf16_string_to_ns_string(word);
        NSArray<NSString*>* guesses = [[NSSpellChecker sharedSpellChecker] guessesForWordRange:NSMakeRange(0, string.length)
                                                                                      inString:string
                                                                                      language:nil
                                                                        inSpellDocumentWithTag:tag];
        Vector<Utf16String> result;
        for (NSString* guess in guesses)
            result.append(ns_string_to_utf16_string(guess));
        on_complete(move(result));
    }

    virtual void ignore(DocumentTag tag, Utf16String const& word) override
    {
        [[NSSpellChecker sharedSpellChecker] ignoreWord:utf16_string_to_ns_string(word) inSpellDocumentWithTag:tag];
    }

    // NB: The system dictionary learns the word for every app, so the tab's tag plays no part.
    virtual void learn(DocumentTag, Utf16String const& word) override
    {
        [[NSSpellChecker sharedSpellChecker] learnWord:utf16_string_to_ns_string(word)];
    }

    virtual bool can_suggest() const override
    {
        return [NSSpellChecker respondsToSelector:@selector(isAutomaticInlineCompletionEnabled)]
            && [[NSSpellChecker sharedSpellChecker] respondsToSelector:@selector(completionCandidateFromCandidates:)];
    }

    virtual void suggest(DocumentTag tag, Utf16String const& text, u32 caret_offset, Function<void(Optional<Web::WritingSuggestion>)> on_complete) override
    {
        // The user's "Show inline predictive text" setting, which can change at any time.
        if (!can_suggest() || !NSSpellChecker.isAutomaticInlineCompletionEnabled) {
            on_complete({});
            return;
        }

        auto id = m_pending_suggestions->next_id++;
        m_pending_suggestions->callbacks.set(id, move(on_complete));
        RefPtr<PendingSuggestions> pending_suggestions = m_pending_suggestions;
        RefPtr<Core::WeakEventLoopReference> event_loop = Core::EventLoop::current_weak();
        auto* string = utf16_string_to_ns_string(text);
        auto handler = ^(NSInteger, NSArray<NSTextCheckingResult*>* candidates) {
            auto strong_event_loop = event_loop->take();
            if (!strong_event_loop)
                return;
            // The candidates arrive on an arbitrary queue, and the spellchecker picks among them on the main thread.
            strong_event_loop->deferred_invoke([pending_suggestions, id, candidates]() mutable {
                Optional<Web::WritingSuggestion> suggestion;
                NSTextCheckingResult* candidate = [[NSSpellChecker sharedSpellChecker] completionCandidateFromCandidates:candidates];
                if (candidate && candidate.replacementString && candidate.range.location != NSNotFound) {
                    suggestion = Web::WritingSuggestion {
                        static_cast<u32>(candidate.range.location),
                        static_cast<u32>(candidate.range.length),
                        ns_string_to_utf16_string(candidate.replacementString),
                    };
                }
                if (auto callback = pending_suggestions->callbacks.take(id); callback.has_value())
                    (*callback)(move(suggestion));
            });
        };
        // The platform's own candidate types go along with the completion type, which is how the system asks.
        NSTextCheckingTypes types = NSTextCheckingTypeSpelling | NSTextCheckingTypeReplacement | NSTextCheckingTypeCorrection | text_checking_type_single_completion;
        [[NSSpellChecker sharedSpellChecker] requestCandidatesForSelectedRange:NSMakeRange(caret_offset, 0)
                                                                      inString:string
                                                                         types:types
                                                                       options:nil
                                                        inSpellDocumentWithTag:tag
                                                             completionHandler:handler];
    }

private:
    NonnullRefPtr<PendingChecks> m_pending_checks { adopt_ref(*new PendingChecks) };
    NonnullRefPtr<PendingSuggestions> m_pending_suggestions { adopt_ref(*new PendingSuggestions) };
};

NonnullOwnPtr<WebView::SpellChecker> create_platform_spell_checker()
{
    return make<MacSpellChecker>();
}

}
