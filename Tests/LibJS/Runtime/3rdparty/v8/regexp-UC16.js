// Copyright 2008 the V8 project authors. All rights reserved.
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

test("regexp-UC16", () => {
    assertEquals("x\u03a3\u03c3x,\u03a3", String(/x(.)\1x/i.exec("x\u03a3\u03c3x")), "backref-UC16");
    assertFalse(/x(...)\1/i.test("x\u03a3\u03c2\u03c3\u03c2\u03c3"), "\\1 ASCII, string short");
    assertTrue(/\u03a3((?:))\1\1x/i.test("\u03c2x"), "backref-UC16-empty");
    assertTrue(/x(?:...|(...))\1x/i.test("x\u03a3\u03c2\u03c3x"), "backref-UC16-uncaptured");
    assertTrue(/x(?:...|(...))\1x/i.test("x\u03c2\u03c3\u039b\u03a3\u03c2\u03bbx"), "backref-UC16-backtrack");
    var longUC16String = "x\u03a3\u03c2\u039b\u03c2\u03c3\u03bb\u03c3\u03a3\u03bb";
    assertEquals(
        longUC16String + "," + longUC16String.substring(1, 4),
        String(/x(...)\1\1/i.exec(longUC16String)),
        "backref-UC16-twice"
    );

    assertFalse(/\xc1/i.test("fooA"), "quickcheck-uc16-pattern-ascii-subject");
    assertFalse(/[\xe9]/.test("i"), "charclass-uc16-pattern-ascii-subject");
    assertFalse(/\u5e74|\u6708/.test("t"), "alternation-uc16-pattern-ascii-subject");
});
