describe("tagged template literal errors", () => {
    test("undefined variables in template expression throw a ReferenceError", () => {
        expect(() => {
            foo`bar${baz}`;
        }).toThrowWithMessage(ReferenceError, "'foo' is not defined");

        expect(() => {
            function foo() {}
            foo`bar${baz}`;
        }).toThrowWithMessage(ReferenceError, "'baz' is not defined");
    });

    test("cannot tag a non-function", () => {
        expect(() => {
            undefined``;
        }).toThrowWithMessage(TypeError, "undefined is not a function");
    });
});

describe("tagged template literal functionality", () => {
    test("empty template tag", () => {
        function test1(strings) {
            expect(strings).toBeInstanceOf(Array);
            expect(strings).toHaveLength(1);
            expect(strings[0]).toBe("");
            return 42;
        }
        expect(test1``).toBe(42);
    });

    test("tagging a template literal", () => {
        function test2(s) {
            return function (strings) {
                expect(strings).toBeInstanceOf(Array);
                expect(strings).toHaveLength(1);
                expect(strings[0]).toBe("bar");
                return s + strings[0];
            };
        }
        expect(test2("foo")`bar`).toBe("foobar");
    });

    test("tagging an object function key", () => {
        var test3 = {
            foo(strings, p1) {
                expect(strings).toBeInstanceOf(Array);
                expect(strings).toHaveLength(2);
                expect(strings[0]).toBe("");
                expect(strings[1]).toBe("");
                expect(p1).toBe("bar");
            },
        };
        test3.foo`${"bar"}`;
    });

    test("tagging with a variable in a template expression", () => {
        function test4(strings, p1) {
            expect(strings).toBeInstanceOf(Array);
            expect(strings).toHaveLength(2);
            expect(strings[0]).toBe("foo");
            expect(strings[1]).toBe("");
            expect(p1).toBe(42);
        }
        var bar = 42;
        test4`foo${bar}`;
    });

    test("template tag result of another template tag", () => {
        function test5(strings, p1, p2) {
            expect(strings).toBeInstanceOf(Array);
            expect(strings).toHaveLength(3);
            expect(strings[0]).toBe("foo");
            expect(strings[1]).toBe("baz");
            expect(strings[2]).toBe("");
            expect(p1).toBe(42);
            expect(p2).toBe("qux");
            return (strings, value) => `${value}${strings[0]}`;
        }
        var bar = 42;
        expect(test5`foo${bar}baz${"qux"}``test${123}`).toBe("123test");
    });

    test("general test", () => {
        function review(strings, name, rating) {
            return `${strings[0]}**${name}**${strings[1]}_${rating}_${strings[2]}`;
        }
        var name = "SerenityOS";
        var rating = "great";
        expect(review`${name} is a ${rating} project!`).toBe("**SerenityOS** is a _great_ project!");
    });

    test("template object structure", () => {
        const getTemplateObject = (...rest) => rest;
        const getRawTemplateStrings = arr => arr.raw;

        let o = getTemplateObject`foo\nbar`;
        expect(Object.getOwnPropertyNames(o[0])).toContain("raw");

        let raw = getRawTemplateStrings`foo${1 + 3}\nbar`;
        expect(Object.getOwnPropertyNames(raw)).not.toContain("raw");
        expect(raw).toHaveLength(2);
        expect(raw[0]).toBe("foo");
        expect(raw[1]).toHaveLength(5);
        expect(raw[1]).toBe("\\nbar");
    });

    test("invalid escapes give undefined cooked values but can be accessed in raw form", () => {
        let calls = 0;
        let lastValue = null;
        function noCookedButRaw(values) {
            ++calls;
            expect(values).not.toBeNull();
            expect(values.raw).toHaveLength(1);
            expect(values.raw[0].length).toBeGreaterThan(0);
            expect(values.raw[0].charAt(0)).toBe("\\");
            expect(values[0]).toBeUndefined();
            lastValue = values.raw[0];
        }
        // Invalid unicode escapes.
        noCookedButRaw`\u`;
        expect(calls).toBe(1);
        expect(lastValue).toBe("\\u");

        // Octal escapes (always invalid in templates).
        noCookedButRaw`\01`;
        expect(calls).toBe(2);
        expect(lastValue).toBe("\\01");

        // Unicode code point out of range.
        noCookedButRaw`\u{10FFFFF}`;
        expect(calls).toBe(3);
        expect(lastValue).toBe("\\u{10FFFFF}");

        // Invalid hex escapes.
        noCookedButRaw`\xZZ`;
        expect(calls).toBe(4);
        expect(lastValue).toBe("\\xZZ");

        noCookedButRaw`\x`;
        expect(calls).toBe(5);
        expect(lastValue).toBe("\\x");

        // Digit escapes (1-9).
        noCookedButRaw`\1`;
        expect(calls).toBe(6);
        expect(lastValue).toBe("\\1");

        noCookedButRaw`\9`;
        expect(calls).toBe(7);
        expect(lastValue).toBe("\\9");

        noCookedButRaw`\8`;
        expect(calls).toBe(8);
        expect(lastValue).toBe("\\8");

        // \0 followed by another digit.
        noCookedButRaw`\00`;
        expect(calls).toBe(9);
        expect(lastValue).toBe("\\00");

        noCookedButRaw`\08`;
        expect(calls).toBe(10);
        expect(lastValue).toBe("\\08");

        noCookedButRaw`\09`;
        expect(calls).toBe(11);
        expect(lastValue).toBe("\\09");
    });

    test("valid escapes in tagged templates have correct cooked values", () => {
        function getCooked(values) {
            return values[0];
        }
        // Simple escapes should still work.
        expect(getCooked`\n`).toBe("\n");
        expect(getCooked`\r`).toBe("\r");
        expect(getCooked`\t`).toBe("\t");
        expect(getCooked`\b`).toBe("\b");
        expect(getCooked`\f`).toBe("\f");
        expect(getCooked`\v`).toBe("\v");
        expect(getCooked`\0`).toBe("\0");
        expect(getCooked`\\`).toBe("\\");
        expect(getCooked`\x41`).toBe("A");
        expect(getCooked`\u0041`).toBe("A");
        expect(getCooked`\u{41}`).toBe("A");
    });

    test("for multiple values gives undefined only for invalid strings", () => {
        let restValue = null;
        let stringsValue = null;
        let calls = 0;

        function extractArguments(value, ...arguments) {
            ++calls;
            restValue = arguments;
            stringsValue = value;
        }
        extractArguments`valid${1}invalid\u`;

        expect(calls).toBe(1);
        expect(restValue).toHaveLength(1);
        expect(restValue[0]).toBe(1);
        expect(stringsValue).toHaveLength(2);
        expect(stringsValue[0]).toBe("valid");
        expect(stringsValue[1]).toBeUndefined();
        expect(stringsValue.raw).toHaveLength(2);
        expect(stringsValue.raw[0]).toBe("valid");
        expect(stringsValue.raw[1]).toBe("invalid\\u");
    });

    test("string value gets cached per AST node", () => {
        function call(func, val) {
            return func`template${val}second`;
        }

        let firstResult = call(value => value, 1);
        let secondResult = call(value => value, 2);
        expect(firstResult).toBe(secondResult);
    });

    test("template object property attributes", () => {
        let templateObj = null;
        function capture(strings) {
            templateObj = strings;
        }
        capture`a${1}b`;

        expect(Object.isFrozen(templateObj)).toBeTrue();

        const desc0 = Object.getOwnPropertyDescriptor(templateObj, 0);
        expect(desc0.writable).toBeFalse();
        expect(desc0.enumerable).toBeTrue();
        expect(desc0.configurable).toBeFalse();

        const desc1 = Object.getOwnPropertyDescriptor(templateObj, 1);
        expect(desc1.writable).toBeFalse();
        expect(desc1.enumerable).toBeTrue();
        expect(desc1.configurable).toBeFalse();

        const rawDesc = Object.getOwnPropertyDescriptor(templateObj, "raw");
        expect(rawDesc.writable).toBeFalse();
        expect(rawDesc.enumerable).toBeFalse();
        expect(rawDesc.configurable).toBeFalse();

        expect(Object.isFrozen(templateObj.raw)).toBeTrue();

        const rawDesc0 = Object.getOwnPropertyDescriptor(templateObj.raw, 0);
        expect(rawDesc0.writable).toBeFalse();
        expect(rawDesc0.enumerable).toBeTrue();
        expect(rawDesc0.configurable).toBeFalse();

        const rawDesc1 = Object.getOwnPropertyDescriptor(templateObj.raw, 1);
        expect(rawDesc1.writable).toBeFalse();
        expect(rawDesc1.enumerable).toBeTrue();
        expect(rawDesc1.configurable).toBeFalse();
    });

    test("this value of call comes from reference for non-computed, non-super property", () => {
        let thisValue = null;
        const obj = {
            func() {
                thisValue = this;
            },
        };

        obj.func``;

        expect(thisValue).toBe(obj);
    });

    test("this value of call comes from reference for computed, non-super property", () => {
        let thisValue = null;
        const obj = {
            func() {
                thisValue = this;
            },
        };

        obj["func"]``;

        expect(thisValue).toBe(obj);
    });

    test("this value of call comes from reference for private property", () => {
        let thisValue = null;
        const obj = new (class A {
            #func = () => {
                thisValue = this;
            };

            constructor() {
                this.#func``;
            }
        })();

        expect(thisValue).toBe(obj);
    });

    test("this value of call comes from reference for non-computed super call property", () => {
        let thisValue = null;

        class A {
            func() {
                thisValue = this;
            }
        }

        const obj = new (class B extends A {
            constructor() {
                super().func``;
            }
        })();

        expect(thisValue).toBe(obj);
    });

    test("this value of call comes from reference for computed super call property", () => {
        let thisValue = null;

        class A {
            func() {
                thisValue = this;
            }
        }

        const obj = new (class B extends A {
            constructor() {
                super()["func"]``;
            }
        })();

        expect(thisValue).toBe(obj);
    });

    test("this value of call comes from reference for non-computed super property", () => {
        let thisValue = null;

        class A {
            func() {
                thisValue = this;
            }
        }

        const obj = new (class B extends A {
            constructor() {
                super();
                super.func``;
            }
        })();

        expect(thisValue).toBe(obj);
    });

    test("this value of call comes from reference for computed super property", () => {
        let thisValue = null;

        class A {
            func() {
                thisValue = this;
            }
        }

        const obj = new (class B extends A {
            constructor() {
                super();
                super["func"]``;
            }
        })();

        expect(thisValue).toBe(obj);
    });
});
