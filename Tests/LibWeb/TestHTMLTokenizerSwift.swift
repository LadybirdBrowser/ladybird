/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

import AK
import LibWeb
import SwiftLibWeb
import Foundation

class StandardError: TextOutputStream {
    func write(_ string: Swift.String) {
        try! FileHandle.standardError.write(contentsOf: Data(string.utf8))
    }
}

@main
struct TestHTMLTokenizerSwift {

    static func testTokenTypes() {
        var standardError = StandardError()
        print("Testing HTMLToken types...", to: &standardError)

        let default_token = HTMLToken()
        default_token.type = .Character(codePoint: "a")
        precondition(default_token.isCharacter())

        print("HTMLToken types pass", to: &standardError)
    }

    static func testParserWhitespace() {
        var standardError = StandardError()
        print("Testing HTMLToken parser whitespace...", to: &standardError)

        for codePoint: Character in ["\t", "\n", "\r", "\u{000C}", " "] {
            let token = HTMLToken(type: .Character(codePoint: codePoint))
            precondition(token.isParserWhitespace())
        }

        for codePoint: Character in ["a", "b", "c", "d", "e", "f", "g", "h", "i", "j"] {
            let token = HTMLToken(type: .Character(codePoint: codePoint))
            precondition(!token.isParserWhitespace())
        }

        print("HTMLToken parser whitespace pass", to: &standardError)
    }

    static func main() {
        var standardError = StandardError()
        print("Starting test suite...", to: &standardError)

        testTokenTypes()
        testParserWhitespace()

        print("All tests pass", to: &standardError)
    }
}
