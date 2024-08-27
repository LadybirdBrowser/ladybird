/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

import AK
import Web
import Testing

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
}
