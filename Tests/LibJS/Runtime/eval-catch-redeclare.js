/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * https://github.com/tc39/test262/blob/main/test/annexB/language/eval-code/direct/var-env-lower-lex-catch-non-strict.js
 */

describe("eval catch block redeclaration", () => {
    test("eval function declaration in catch block does not throw", () => {
        try {
            throw null;
        } catch (err) {
            eval("function err() {}");
        }
    });

    test("eval function* declaration in catch block does not throw", () => {
        try {
            throw null;
        } catch (err) {
            eval("function* err() {}");
        }
    });

    test("eval async function declaration in catch block does not throw", () => {
        try {
            throw null;
        } catch (err) {
            eval("async function err() {}");
        }
    });

    test("eval async function* declaration in catch block does not throw", () => {
        try {
            throw null;
        } catch (err) {
            eval("async function* err() {}");
        }
    });

    test("eval var declaration in catch block does not throw", () => {
        try {
            throw null;
        } catch (err) {
            eval("var err;");
        }
    });

    test("eval var in for statement inside catch block does not throw", () => {
        try {
            throw null;
        } catch (err) {
            eval("for (var err; false; ) {}");
        }
    });

    test("eval var in for-in inside catch block does not throw", () => {
        try {
            throw null;
        } catch (err) {
            eval("for (var err in {}) {}");
        }
    });

    test("eval var in for-of inside catch block does not throw", () => {
        try {
            throw null;
        } catch (err) {
            eval("for (var err of []) {}");
        }
    });
});
