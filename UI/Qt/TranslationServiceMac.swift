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

@available(macOS 26.4, *)
@objc(LadybirdTranslationLanguageStatus)
public enum TranslationLanguageStatus: Int {
    case unsupported
    case needsDownload
    case ready
}

// The run's token, which the translation carries over to the words that translate the run.
@available(macOS 26.4, *)
enum TranslationRunTokenKey: CodableAttributedStringKey {
    typealias Value = UInt32
    static let name = "LadybirdTranslationRunToken"
}

@available(macOS 26.4, *)
private func status(from source: Locale.Language, to target: Locale.Language) async -> TranslationLanguageStatus
{
    var status = TranslationLanguageStatus.unsupported
    for strategy in [TranslationSession.Strategy.highFidelity, .lowLatency] {
        switch await LanguageAvailability(preferredStrategy: strategy).status(from: source, to: target) {
        case .installed:
            return .ready
        case .supported:
            status = .needsDownload
        default:
            break
        }
    }
    return status
}

@available(macOS 26.4, *)
@objc(LadybirdTranslationService)
public final class TranslationService: NSObject {
    // Calls completion with one status per target, in the order given, on an arbitrary thread.
    @objc public static func statuses(from source: String, to targets: [String], completion: @escaping @Sendable ([NSNumber]) -> Void)
    {
        Task.detached {
            let source_language = Locale.Language(identifier: source)
            var statuses: [NSNumber] = []
            for target in targets {
                let target_language = Locale.Language(identifier: target)
                if source_language.languageCode == target_language.languageCode {
                    statuses.append(NSNumber(value: TranslationLanguageStatus.unsupported.rawValue))
                    continue
                }
                statuses.append(NSNumber(value: await status(from: source_language, to: target_language).rawValue))
            }
            completion(statuses)
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

    @objc public init(source: String, target: String)
    {
        self.source = Locale.Language(identifier: source)
        self.target = Locale.Language(identifier: target)
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

    @objc public func cancel()
    {
        lock.withLock { session }?.cancel()
    }

    // NB: Units are translated one after another, so only one call at a time gets here.
    private func translation_session() async -> TranslationSession
    {
        if let session = lock.withLock({ self.session }) {
            return session
        }

        // The Apple Intelligence models translate more fluently, so they're used wherever they're installed.
        let high_fidelity_ready = await LanguageAvailability(preferredStrategy: .highFidelity).status(from: source, to: target) == .installed
        let session = TranslationSession(installedSource: source, target: target, preferredStrategy: high_fidelity_ready ? .highFidelity : .lowLatency)
        lock.withLock { self.session = session }
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

        let session = await translation_session()
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
