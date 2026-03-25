// Copyright 2017 the V8 project authors. All rights reserved.
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

test("harmony/regexp-dotall", () => {
    function toSlowMode(re) {
        re.exec = str => RegExp.prototype.exec.call(re, str);
        return re;
    }

    // Construction does not throw.
    {
        let re = /./s;
        re = RegExp(".", "s");
        re = new RegExp(".", "s");
        assertThrows(() => new RegExp(".", "wtf"), SyntaxError);
    }

    // The flags accessors.
    {
        let re = /./s;
        assertEquals("s", re.flags);
        assertFalse(re.global);
        assertFalse(re.ignoreCase);
        assertFalse(re.multiline);
        assertFalse(re.sticky);
        assertFalse(re.unicode);
        assertTrue(re.dotAll);

        re = toSlowMode(/./s);
        assertEquals("s", re.flags);
        assertFalse(re.global);
        assertFalse(re.ignoreCase);
        assertFalse(re.multiline);
        assertFalse(re.sticky);
        assertFalse(re.unicode);
        assertTrue(re.dotAll);

        re = /./gimsuy;
        assertEquals("gimsuy", re.flags);
        assertTrue(re.global);
        assertTrue(re.ignoreCase);
        assertTrue(re.multiline);
        assertTrue(re.sticky);
        assertTrue(re.unicode);
        assertTrue(re.dotAll);

        re = /./gimuy;
        assertEquals("gimuy", re.flags);
        assertTrue(re.global);
        assertTrue(re.ignoreCase);
        assertTrue(re.multiline);
        assertTrue(re.sticky);
        assertTrue(re.unicode);
        assertFalse(re.dotAll);
    }

    // Different construction variants with all flags.
    {
        assertEquals("gimsuy", new RegExp("", "yusmig").flags);
        assertEquals("gimsuy", new RegExp().compile("", "yusmig").flags);
    }

    // Default '.' behavior.
    {
        let re = /^.$/;
        assertTrue(re.test("a"));
        assertTrue(re.test("3"));
        assertTrue(re.test("π"));
        assertTrue(re.test("\u2027"));
        assertTrue(re.test("\u0085"));
        assertTrue(re.test("\v"));
        assertTrue(re.test("\f"));
        assertTrue(re.test("\u180E"));
        assertFalse(re.test("\u{10300}")); // Supplementary plane.
        assertFalse(re.test("\n"));
        assertFalse(re.test("\r"));
        assertFalse(re.test("\u2028"));
        assertFalse(re.test("\u2029"));
    }

    // Default '.' behavior (unicode).
    {
        let re = /^.$/u;
        assertTrue(re.test("a"));
        assertTrue(re.test("3"));
        assertTrue(re.test("π"));
        assertTrue(re.test("\u2027"));
        assertTrue(re.test("\u0085"));
        assertTrue(re.test("\v"));
        assertTrue(re.test("\f"));
        assertTrue(re.test("\u180E"));
        assertTrue(re.test("\u{10300}")); // Supplementary plane.
        assertFalse(re.test("\n"));
        assertFalse(re.test("\r"));
        assertFalse(re.test("\u2028"));
        assertFalse(re.test("\u2029"));
    }

    // DotAll '.' behavior.
    {
        let re = /^.$/s;
        assertTrue(re.test("a"));
        assertTrue(re.test("3"));
        assertTrue(re.test("π"));
        assertTrue(re.test("\u2027"));
        assertTrue(re.test("\u0085"));
        assertTrue(re.test("\v"));
        assertTrue(re.test("\f"));
        assertTrue(re.test("\u180E"));
        assertFalse(re.test("\u{10300}")); // Supplementary plane.
        assertTrue(re.test("\n"));
        assertTrue(re.test("\r"));
        assertTrue(re.test("\u2028"));
        assertTrue(re.test("\u2029"));
    }

    // DotAll '.' behavior (unicode).
    {
        let re = /^.$/su;
        assertTrue(re.test("a"));
        assertTrue(re.test("3"));
        assertTrue(re.test("π"));
        assertTrue(re.test("\u2027"));
        assertTrue(re.test("\u0085"));
        assertTrue(re.test("\v"));
        assertTrue(re.test("\f"));
        assertTrue(re.test("\u180E"));
        assertTrue(re.test("\u{10300}")); // Supplementary plane.
        assertTrue(re.test("\n"));
        assertTrue(re.test("\r"));
        assertTrue(re.test("\u2028"));
        assertTrue(re.test("\u2029"));
    }
});
