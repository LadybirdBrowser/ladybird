// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

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
            fn = new Function(fn);
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

test("es6/unicode-regexp-backrefs", () => {
    function replace(string) {
        return string.replace(/L/g, "\ud800").replace(/l/g, "\ud801").replace(/T/g, "\udc00").replace(/\./g, "[^]");
    }

    function test(expectation, regexp_source, subject) {
        if (expectation !== null) expectation = expectation.map(replace);
        subject = replace(subject);
        regexp_source = replace(regexp_source);
        assertEquals(expectation, new RegExp(regexp_source, "u").exec(subject));
    }

    // Back reference does not end in the middle of a surrogate pair.
    test(null, "(L)\\1", "LLT");
    test(["LLTLl", "L", "l"], "(L).*\\1(.)", "LLTLl");
    test(null, "(aL).*\\1", "aLaLT");
    test(["aLaLTaLl", "aL", "l"], "(aL).*\\1(.)", "aLaLTaLl");

    var s = "TabcLxLTabcLxTabcLTyTabcLz";
    test([s, "TabcL", "z"], "([^x]+).*\\1(.)", s);

    // Back reference does not start in the middle of a surrogate pair.
    test(["TLTabTc", "T", "c"], "(T).*\\1(.)", "TLTabTc");

    // Lookbehinds.
    test(null, "(?<=\\1(T)x)", "LTTx");
    test(["", "b", "T"], "(?<=(.)\\2.*(T)x)", "bTaLTTx");
    test(null, "(?<=\\1.*(L)x)", "LTLx");
    test(["", "b", "L"], "(?<=(.)\\2.*(L)x)", "bLaLTLx");

    test(null, "([^x]+)x*\\1", "LxLT");
    test(null, "([^x]+)x*\\1", "TxLT");
    test(null, "([^x]+)x*\\1", "LTxL");
    test(null, "([^x]+)x*\\1", "LTxT");
    test(null, "([^x]+)x*\\1", "xLxLT");
    test(null, "([^x]+)x*\\1", "xTxLT");
    test(null, "([^x]+)x*\\1", "xLTxL");
    test(null, "([^x]+)x*\\1", "xLTxT");
    test(null, "([^x]+)x*\\1", "xxxLxxLTxx");
    test(null, "([^x]+)x*\\1", "xxxTxxLTxx");
    test(null, "([^x]+)x*\\1", "xxxLTxxLxx");
    test(null, "([^x]+)x*\\1", "xxxLTxxTxx");
    test(["LTTxxLTT", "LTT"], "([^x]+)x*\\1", "xxxLTTxxLTTxx");
});
