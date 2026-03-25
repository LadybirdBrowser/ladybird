// Copyright 2023 the V8 project authors. All rights reserved.
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

test("regexp-modifiers", () => {
    function test_invalid(re) {
        assertEarlyError(`/${re}/`);
        assertThrowsAtRuntime(`new RegExp('${re}')`, SyntaxError);
    }

    test_invalid("(?-:.)");
    test_invalid("(?--:.)");
    test_invalid("(?mm:.)");
    test_invalid("(?ii:.)");
    test_invalid("(?ss:.)");
    test_invalid("(?-mm:.)");
    test_invalid("(?-ii:.)");
    test_invalid("(?-ss:.)");
    test_invalid("(?g-:.)");
    test_invalid("(?-u:.)");
    test_invalid("(?m-m:.)");
    test_invalid("(?i-i:.)");
    test_invalid("(?s-s:.)");
    test_invalid("(?msi-ims:.)");
    test_invalid("(?i--m:.)");
    test_invalid("(?i<)");
    test_invalid("(?i=)");
    test_invalid("(?i!)");
    test_invalid("(?m<)");
    test_invalid("(?m=)");
    test_invalid("(?m!)");
    test_invalid("(?s<)");
    test_invalid("(?s=)");
    test_invalid("(?s!)");
    test_invalid("(?-<)");
    test_invalid("(?-=)");
    test_invalid("(?-!)");

    function test(re, expectedMatch, expectedNoMatch = []) {
        for (const match of expectedMatch) {
            assertTrue(re.test(match), `${re}.test(${match})`);
        }
        for (const match of expectedNoMatch) {
            assertFalse(re.test(match), `${re}.test(${match})`);
        }
    }

    test(/(?i:ba)r/, ["bar", "Bar", "BAr"], ["BAR", "BaR"]);
    test(/(?-i:ba)r/i, ["bar", "baR"], ["Bar", "BAR"]);
    test(/F(?i:oo(?-i:b)a)r/, ["Foobar", "FoObAr"], ["FooBar", "FoobaR"]);
    test(/F(?i:oo(?i:b)a)r/, ["Foobar", "FoObAr", "FOOBAr"], ["FoobaR"]);
    test(/^[a-z](?-i:[a-z])$/i, ["ab", "Ab"], ["aB"]);
    test(/^(?i:[a-z])[a-z]$/, ["ab", "Ab"], ["aB"]);
    test(/(?i:foo|bar)/, ["FOO", "FOo", "Foo", "fOO", "BAR", "BAr", "Bar", "bAR"]);
    test(/(?i:foo|bar|baz)/, ["FOO", "FOo", "Foo", "fOO", "BAR", "BAr", "Bar", "bAR", "BAZ", "BAz", "Baz", "bAZ"]);
    test(/Foo(?i:B[\q{ĀĂĄ|AaA}--\q{āăą}])r/v, ["FooBaaar", "FoobAAAr"], ["FooBĀĂĄr", "FooBaaaR"]);

    test(/(?m:^foo$)/, ["foo", "\nfoo", "foo\n", "\nfoo\n"], ["xfoo", "foox"]);

    test(/(?s:^.$)/, ["a", "A", "0", "\n", "\r", "\u2028", "\u2029", "π"], ["\u{10300}"]);

    test(/(?ms-i:^f.o$)/i, ["foo", "\nf\ro", "f\no\n", "\nfπo\n"], ["Foo", "\nf\nO", "foO\n", "\nFOO\n"]);
    test(/(?m:^f(?si:.o)$)/, ["foo", "\nfoO", "f\no\n", "\nf\rO\n"], ["Foo", "F\no\n"]);
    test(/(?i:.oo)/, ["Foo", "FOO", "fOo", "foO"]);
});
