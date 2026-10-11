/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AtomicRefCounted.h>
#include <AK/HashMap.h>
#include <AK/RefPtr.h>
#include <LibCore/EventLoop.h>
#include <UI/Qt/SpellCheckerMac.h>

#import <AppKit/AppKit.h>

namespace Ladybird {

// NSString and our strings hold the same UTF-16 code units, lone surrogates included, so offsets carry over unchanged.
static NSString* ns_string_from_utf16(Utf16String const& string)
{
    auto view = string.utf16_view();
    Vector<unichar> code_units;
    code_units.ensure_capacity(view.length_in_code_units());
    for (size_t index = 0; index < view.length_in_code_units(); ++index)
        code_units.unchecked_append(view.code_unit_at(index));
    return [[NSString alloc] initWithCharacters:code_units.data() length:code_units.size()];
}

static Utf16String utf16_from_ns_string(NSString* string)
{
    char const* utf8 = [string UTF8String];
    return Utf16String::from_utf8(StringView { utf8, strlen(utf8) });
}

// Answers come back on an arbitrary queue. The callbacks that take them stay on the main thread, keyed by request, so
// only this atomically counted holder crosses threads.
struct PendingChecks : public AtomicRefCounted<PendingChecks> {
    HashMap<u64, Function<void(Vector<Web::SpellingError>)>> callbacks;
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
        auto* string = ns_string_from_utf16(text);
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

    virtual Vector<Utf16String> guesses(DocumentTag tag, Utf16String const& word) override
    {
        auto* string = ns_string_from_utf16(word);
        NSArray<NSString*>* guesses = [[NSSpellChecker sharedSpellChecker] guessesForWordRange:NSMakeRange(0, string.length)
                                                                                      inString:string
                                                                                      language:nil
                                                                        inSpellDocumentWithTag:tag];
        Vector<Utf16String> result;
        for (NSString* guess in guesses)
            result.append(utf16_from_ns_string(guess));
        return result;
    }

    virtual void ignore(DocumentTag tag, Utf16String const& word) override
    {
        [[NSSpellChecker sharedSpellChecker] ignoreWord:ns_string_from_utf16(word) inSpellDocumentWithTag:tag];
    }

    // NB: The system dictionary learns the word for every app, so the tab's tag plays no part.
    virtual void learn(DocumentTag, Utf16String const& word) override
    {
        [[NSSpellChecker sharedSpellChecker] learnWord:ns_string_from_utf16(word)];
    }

private:
    NonnullRefPtr<PendingChecks> m_pending_checks { adopt_ref(*new PendingChecks) };
};

NonnullOwnPtr<WebView::SpellChecker> create_platform_spell_checker()
{
    return make<MacSpellChecker>();
}

}
