/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// The Translation framework has no Objective-C API, so this exposes the parts the browser uses to Objective-C++. Its
// Objective-C interface is declared by hand in TranslationServiceMac.h, which must match the names given here.

import Foundation
import Translation

@available(macOS 26.4, *)
@objc(LadybirdTranslationRun)
public final class TranslationRun: NSObject {
    @objc public let text: String
    @objc public let token: UInt32
    @objc public let keepAsIs: Bool

    @objc public init(text: String, token: UInt32, keepAsIs: Bool)
    {
        self.text = text
        self.token = token
        self.keepAsIs = keepAsIs
    }
}


// The run's token, which the translation carries over to the words that translate the run.
@available(macOS 26.4, *)
enum TranslationRunTokenKey: CodableAttributedStringKey {
    typealias Value = UInt32
    static let name = "LadybirdTranslationRunToken"
}

// The bits of LadybirdTranslationModels, which TranslationServiceMac.h declares.
private let fast_models_supported = 1 << 0
private let fast_models_installed = 1 << 1
private let higher_quality_models_supported = 1 << 2
private let higher_quality_models_installed = 1 << 3

@available(macOS 26.4, *)
private func pair_models(from source: Locale.Language, to target: Locale.Language) async -> Int
{
    var models = 0
    switch await LanguageAvailability(preferredStrategy: .lowLatency).status(from: source, to: target) {
    case .installed:
        models |= fast_models_supported | fast_models_installed
    case .supported:
        models |= fast_models_supported
    default:
        break
    }
    switch await LanguageAvailability(preferredStrategy: .highFidelity).status(from: source, to: target) {
    case .installed:
        models |= higher_quality_models_supported | higher_quality_models_installed
    case .supported:
        models |= higher_quality_models_supported
    default:
        break
    }
    return models
}

@available(macOS 26.4, *)
@objc(LadybirdTranslationService)
public final class TranslationService: NSObject {
    // Calls completion with the models of each pair, in the order of the targets given, on an arbitrary thread.
    @objc public static func models(from source: String, to targets: [String], completion: @escaping @Sendable ([NSNumber]) -> Void)
    {
        Task.detached {
            let source_language = Locale.Language(identifier: source)
            var all_models: [NSNumber] = []
            for target in targets {
                let target_language = Locale.Language(identifier: target)
                if source_language.languageCode == target_language.languageCode {
                    all_models.append(NSNumber(value: 0))
                    continue
                }
                all_models.append(NSNumber(value: await pair_models(from: source_language, to: target_language)))
            }
            completion(all_models)
        }
    }
}

// Translates from one language to another, one unit at a time, in the order the units are given.
@available(macOS 26.4, *)
@objc(LadybirdTranslationSession)
public final class TranslationSessionWrapper: NSObject {
    private let source: Locale.Language
    private let target: Locale.Language
    private let lock = NSLock()
    private var previous: Task<Void, Never>?
    private var session: TranslationSession?
    // The session translating the current unit, which may be one made for an earlier choice of model.
    private var active_session: TranslationSession?
    private var cancelled = false
    private var prefer_higher_quality: Bool
    // Counts the times the model is chosen again, so a session made for an earlier choice isn't kept.
    private var model_choice = 0

    @objc public init(source: String, target: String, preferHigherQuality: Bool)
    {
        self.source = Locale.Language(identifier: source)
        self.target = Locale.Language(identifier: target)
        self.prefer_higher_quality = preferHigherQuality
    }

    // Calls completion with the translated runs, or with nil if the unit couldn't be translated, on an arbitrary thread.
    @objc public func translate(_ runs: [TranslationRun], completion: @escaping @Sendable ([TranslationRun]?) -> Void)
    {
        lock.withLock {
            let previous = self.previous
            self.previous = Task.detached { [self] in
                await previous?.value
                completion(await translate_unit(runs))
            }
        }
    }

    // Cancels the unit being translated, and makes the units queued after it fail at once.
    @objc public func cancel()
    {
        let sessions = lock.withLock {
            cancelled = true
            return [session, active_session]
        }
        for session in sessions {
            session?.cancel()
        }
    }

    // The unit being translated keeps its model; the ones after it get the model chosen now.
    @objc public func chooseModelAgain(preferHigherQuality: Bool)
    {
        lock.withLock {
            prefer_higher_quality = preferHigherQuality
            model_choice += 1
            session = nil
        }
    }

    // Units are translated one after another, so only one call at a time gets here.
    private func translation_session() async -> TranslationSession
    {
        let (existing_session, prefer_higher_quality, model_choice) = lock.withLock { (self.session, self.prefer_higher_quality, self.model_choice) }
        if let existing_session {
            return existing_session
        }

        // The classic models are several times faster than the Apple Intelligence ones, so they're used if installed,
        // unless the user prefers the Apple Intelligence ones.
        let low_latency_ready = await LanguageAvailability(preferredStrategy: .lowLatency).status(from: source, to: target) == .installed
        var strategy: TranslationSession.Strategy = low_latency_ready ? .lowLatency : .highFidelity
        if prefer_higher_quality, await LanguageAvailability(preferredStrategy: .highFidelity).status(from: source, to: target) == .installed {
            strategy = .highFidelity
        }
        let session = TranslationSession(installedSource: source, target: target, preferredStrategy: strategy)
        lock.withLock {
            if self.model_choice == model_choice {
                self.session = session
            }
        }
        return session
    }

    private func translate_unit(_ runs: [TranslationRun]) async -> [TranslationRun]?
    {
        var source_text = AttributedString()
        for run in runs {
            var attributed_run = AttributedString(run.text)
            attributed_run[TranslationRunTokenKey.self] = run.token
            if run.keepAsIs {
                attributed_run.translation.skipsTranslation = true
            }
            source_text += attributed_run
        }

        if lock.withLock({ cancelled }) {
            return nil
        }
        let session = await translation_session()
        let started = lock.withLock {
            if cancelled {
                return false
            }
            active_session = session
            return true
        }
        guard started else {
            return nil
        }
        defer { lock.withLock { active_session = nil } }
        guard let response = try? await session.translate(source_text), let translated_text = response.attributedTargetText else {
            return nil
        }

        var translated_runs: [TranslationRun] = []
        for run in translated_text.runs {
            let text = String(translated_text[run.range].characters)
            translated_runs.append(TranslationRun(text: text, token: run[TranslationRunTokenKey.self] ?? 0, keepAsIs: run.translation.skipsTranslation ?? false))
        }
        return translated_runs
    }
}
