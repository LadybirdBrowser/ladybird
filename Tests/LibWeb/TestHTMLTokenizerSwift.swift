/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

import AK
import Testing
import Web

@Suite
struct TestHTMLTokenizerSwift {

    @Test func tokenTypes() {
        let default_token = HTMLToken()
        default_token.type = .Character(codePoint: "a")
        #expect(default_token.isCharacter())

        #expect("\(default_token)" == "HTMLToken(type: Character(codePoint: a))")
    }

    @Test func parserWhitespace() {
        for codePoint: Character in ["\t", "\n", "\r", "\u{000C}", " "] {
            let token = HTMLToken(type: .Character(codePoint: codePoint))
            #expect(token.isParserWhitespace())
        }

        for codePoint: Character in ["a", "b", "c", "d", "e", "f", "g", "h", "i", "j"] {
            let token = HTMLToken(type: .Character(codePoint: codePoint))
            #expect(!token.isParserWhitespace())
        }
    }

    @Test func dataStateNoInput() {
        let tokenizer = HTMLTokenizer()
        #expect(tokenizer.state == HTMLTokenizer.State.Data)  // initial state

        let token = tokenizer.nextToken()
        #expect(token?.type == .EndOfFile)

        let token2 = tokenizer.nextToken()
        #expect(token2 == nil)
        #expect(tokenizer.state == HTMLTokenizer.State.Data)
    }

    @Test func dataStateSingleChar() {
        guard let tokenizer = HTMLTokenizer(input: "X") else {
            Issue.record("Failed to create tokenizer for 'X'")
            return
        }
        #expect(tokenizer.state == HTMLTokenizer.State.Data)  // initial state

        let token = tokenizer.nextToken()
        #expect(token?.type == .Character(codePoint: "X"))

        let token2 = tokenizer.nextToken()
        #expect(token2?.type == .EndOfFile)

        let token3 = tokenizer.nextToken()
        #expect(token3 == nil)
        #expect(tokenizer.state == HTMLTokenizer.State.Data)
    }

    @Test func dataStateAmpersand() {
        guard let tokenizer = HTMLTokenizer(input: "&") else {
            Issue.record("Failed to create tokenizer for '&'")
            return
        }
        #expect(tokenizer.state == HTMLTokenizer.State.Data)  // initial state

        let token = tokenizer.nextToken()
        #expect(token?.type == .EndOfFile)
        #expect(tokenizer.state == HTMLTokenizer.State.CharacterReference)

        let token2 = tokenizer.nextToken()
        #expect(token2 == nil)
    }

    @Test func dataStateTagOpen() {
        guard let tokenizer = HTMLTokenizer(input: "<") else {
            Issue.record("Failed to create tokenizer for '<'")
            return
        }
        #expect(tokenizer.state == HTMLTokenizer.State.Data)  // initial state

        let token = tokenizer.nextToken()
        #expect(token?.type == .EndOfFile)
        #expect(tokenizer.state == HTMLTokenizer.State.TagOpen)

        let token2 = tokenizer.nextToken()
        #expect(token2 == nil)
    }

    @Test func dataStateNulChar() {
        guard let tokenizer = HTMLTokenizer(input: "H\0I") else {
            Issue.record("Failed to create tokenizer for 'H\\0I'")
            return
        }
        #expect(tokenizer.state == HTMLTokenizer.State.Data)  // initial state

        let token = tokenizer.nextToken()
        #expect(token?.type == .Character(codePoint: "H"))

        let token2 = tokenizer.nextToken()
        #expect(token2?.type == .Character(codePoint: "\u{FFFD}"))

        let token3 = tokenizer.nextToken()
        #expect(token3?.type == .Character(codePoint: "I"))

        let token4 = tokenizer.nextToken()
        #expect(token4?.type == .EndOfFile)

        #expect(tokenizer.state == HTMLTokenizer.State.Data)
    }
}
