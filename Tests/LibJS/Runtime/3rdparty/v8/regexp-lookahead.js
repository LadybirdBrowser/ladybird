// Copyright 2009 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// V8 assertion compatibility shim for Ladybird's test-js harness

function assertEquals(expected, actual, msg) {
    if (expected instanceof RegExp && actual instanceof RegExp) {
        expect(actual.source).toBe(expected.source);
        expect(actual.flags).toBe(expected.flags);
    } else if (Array.isArray(expected) && Array.isArray(actual)) {
        expect(actual).toEqual(expected);
    } else if (expected !== null && typeof expected === "object" && actual !== null && typeof actual === "object") {
        expect(actual).toEqual(expected);
    } else {
        expect(actual).toBe(expected);
    }
}

function assertTrue(val, msg) {
    expect(val).toBeTrue();
}

function assertFalse(val, msg) {
    expect(val).toBeFalse();
}

function assertNull(val, msg) {
    expect(val).toBeNull();
}

function assertNotNull(val, msg) {
    expect(val).not.toBeNull();
}

function assertThrows(fn, type_opt, msg_opt) {
    if (typeof fn === "string") {
        try {
            try {
                fn = new Function(fn);
            } catch (e) {
                return;
            }
        } catch (e) {
            return;
        }
    }
    if (typeof fn === "string") {
        try {
            try {
                fn = new Function(fn);
            } catch (e) {
                return;
            }
        } catch (e) {
            return;
        }
    }
    expect(fn).toThrow();
}

function assertDoesNotThrow(fn, msg) {
    fn();
}

function assertInstanceof(val, type, msg) {
    expect(val instanceof type).toBeTrue();
}

function assertUnreachable(msg) {
    expect().fail("unreachable" + (msg ? ": " + msg : ""));
}

function assertEarlyError(code) {
    assertThrows(() => new Function(code));
}

function assertThrowsAtRuntime(code, type_opt) {
    const f = new Function(code);
    assertThrows(f, type_opt);
}

function assertArrayEquals(expected, actual) {
    expect(actual).toEqual(expected);
}

test("regexp-lookahead", () => {
    function stringEscape(string) {
        // Converts string to source literal.
        return '"' + string.replace(/["\\]/g, "\\$1") + '"';
    }

    function testRE(re, input, expected_result) {
        var testName = re + ".test(" + stringEscape(input) + ")";
        if (expected_result) {
            assertTrue(re.test(input), testName);
        } else {
            assertFalse(re.test(input), testName);
        }
    }

    function execRE(re, input, expected_result) {
        var testName = re + ".exec('" + stringEscape(input) + "')";
        assertEquals(expected_result, re.exec(input), testName);
    }

    // Test of simple positive lookahead.

    var re = /^(?=a)/;
    testRE(re, "a", true);
    testRE(re, "b", false);
    execRE(re, "a", [""]);

    re = /^(?=\woo)f\w/;
    testRE(re, "foo", true);
    testRE(re, "boo", false);
    testRE(re, "fao", false);
    testRE(re, "foa", false);
    execRE(re, "foo", ["fo"]);

    re = /(?=\w).(?=\W)/;
    testRE(re, ".a! ", true);
    testRE(re, ".! ", false);
    testRE(re, ".ab! ", true);
    execRE(re, ".ab! ", ["b"]);

    re = /(?=f(?=[^f]o))../;
    testRE(re, ", foo!", true);
    testRE(re, ", fo!", false);
    testRE(re, ", ffo", false);
    execRE(re, ", foo!", ["fo"]);

    // Positive lookahead with captures.
    re = /^[^\'\"]*(?=([\'\"])).*\1(\w+)\1/;
    testRE(re, "  'foo' ", true);
    testRE(re, '  "foo" ', true);
    testRE(re, " \" 'foo' ", false);
    testRE(re, ' \' "foo" ', false);
    testRE(re, "  'foo\" ", false);
    testRE(re, "  \"foo' ", false);
    execRE(re, "  'foo' ", ["  'foo'", "'", "foo"]);
    execRE(re, '  "foo" ', ['  "foo"', '"', "foo"]);

    // Captures are cleared on backtrack past the look-ahead.
    re = /^(?:(?=(.))a|b)\1$/;
    testRE(re, "aa", true);
    testRE(re, "b", true);
    testRE(re, "bb", false);
    testRE(re, "a", false);
    execRE(re, "aa", ["aa", "a"]);
    execRE(re, "b", ["b", undefined]);

    re = /^(?=(.)(?=(.)\1\2)\2\1)\1\2/;
    testRE(re, "abab", true);
    testRE(re, "ababxxxxxxxx", true);
    testRE(re, "aba", false);
    execRE(re, "abab", ["ab", "a", "b"]);

    re = /^(?:(?=(.))a|b|c)$/;
    testRE(re, "a", true);
    testRE(re, "b", true);
    testRE(re, "c", true);
    testRE(re, "d", false);
    execRE(re, "a", ["a", "a"]);
    execRE(re, "b", ["b", undefined]);
    execRE(re, "c", ["c", undefined]);

    execRE(/^(?=(b))b/, "b", ["b", "b"]);
    execRE(/^(?:(?=(b))|a)b/, "ab", ["ab", undefined]);
    execRE(/^(?:(?=(b)(?:(?=(c))|d))|)bd/, "bd", ["bd", "b", undefined]);

    // Test of Negative Look-Ahead.

    re = /(?!x)./;
    testRE(re, "y", true);
    testRE(re, "x", false);
    execRE(re, "y", ["y"]);

    re = /(?!(\d))|\d/;
    testRE(re, "4", true);
    execRE(re, "4", ["4", undefined]);
    execRE(re, "x", ["", undefined]);

    // Test mixed nested look-ahead with captures.

    re = /^(?=(x)(?=(y)))/;
    testRE(re, "xy", true);
    testRE(re, "xz", false);
    execRE(re, "xy", ["", "x", "y"]);

    re = /^(?!(x)(?!(y)))/;
    testRE(re, "xy", true);
    testRE(re, "xz", false);
    execRE(re, "xy", ["", undefined, undefined]);

    re = /^(?=(x)(?!(y)))/;
    testRE(re, "xz", true);
    testRE(re, "xy", false);
    execRE(re, "xz", ["", "x", undefined]);

    re = /^(?!(x)(?=(y)))/;
    testRE(re, "xz", true);
    testRE(re, "xy", false);
    execRE(re, "xz", ["", undefined, undefined]);

    re = /^(?=(x)(?!(y)(?=(z))))/;
    testRE(re, "xaz", true);
    testRE(re, "xya", true);
    testRE(re, "xyz", false);
    testRE(re, "a", false);
    execRE(re, "xaz", ["", "x", undefined, undefined]);
    execRE(re, "xya", ["", "x", undefined, undefined]);

    re = /^(?!(x)(?=(y)(?!(z))))/;
    testRE(re, "a", true);
    testRE(re, "xa", true);
    testRE(re, "xyz", true);
    testRE(re, "xya", false);
    execRE(re, "a", ["", undefined, undefined, undefined]);
    execRE(re, "xa", ["", undefined, undefined, undefined]);
    execRE(re, "xyz", ["", undefined, undefined, undefined]);

    // Be sure the quick check is working right for lookahead.
    re = /(?=..)abcd/;
    testRE(re, "----abcd", true);
});
