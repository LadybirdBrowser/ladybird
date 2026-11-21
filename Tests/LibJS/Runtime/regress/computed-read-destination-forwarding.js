describe("computed property read destinations", () => {
    test("the destination can also supply the base or the property key", () => {
        function replaceBase(base, key) {
            base = base[key];
            return base;
        }

        function replaceKey(base, key) {
            key = base[key];
            return key;
        }

        function replaceLocalBase(base, key) {
            let value = base;
            value = value[key];
            return value;
        }

        function replaceLocalKey(base, key) {
            let value = key;
            value = base[value];
            return value;
        }

        const base = {
            get answer() {
                expect(this).toBe(base);
                return 42;
            },
        };
        const key = {
            [Symbol.toPrimitive](hint) {
                expect(hint).toBe("string");
                return "answer";
            },
        };

        for (const read of [replaceBase, replaceKey, replaceLocalBase, replaceLocalKey]) {
            expect(read(base, key)).toBe(42);
        }
    });

    test("exceptions leave the previous destination visible to catch", () => {
        function read(base, key) {
            let value = 17;
            try {
                value = base[key];
            } catch (error) {
                return [value, error];
            }
        }

        function readNamed(base) {
            let value = 17;
            try {
                value = base["answer"];
            } catch (error) {
                return [value, error];
            }
        }

        const failure = {};
        const throwingKey = {
            [Symbol.toPrimitive]() {
                throw failure;
            },
        };
        const throwingGetter = {
            get answer() {
                throw failure;
            },
        };

        expect(read({}, throwingKey)).toEqual([17, failure]);
        expect(read(throwingGetter, "answer")).toEqual([17, failure]);
        expect(readNamed(throwingGetter)).toEqual([17, failure]);
    });

    test("super reads keep the receiver and the previous destination on failure", () => {
        const failure = {};
        const prototype = {
            get answer() {
                return this.expected;
            },
            get length() {
                return this.expected;
            },
            get failing() {
                throw failure;
            },
        };
        const object = {
            __proto__: prototype,
            expected: 42,
            read(key) {
                const result = super[key];
                return result;
            },
            readNamed() {
                const result = super["answer"];
                return result;
            },
            readLength() {
                const result = super["length"];
                return result;
            },
            readFailure(key) {
                let result = 17;
                try {
                    result = super[key];
                } catch (error) {
                    return [result, error];
                }
            },
        };

        expect(object.read("answer")).toBe(42);
        expect(object.read("length")).toBe(42);
        expect(object.readNamed()).toBe(42);
        expect(object.readLength()).toBe(42);
        expect(object.readFailure("failing")).toEqual([17, failure]);
    });

    test("computed reads used by compound assignments preserve their references", () => {
        const log = [];
        const key = {
            [Symbol.toPrimitive]() {
                log.push("key");
                return "answer";
            },
        };
        const object = {
            value: 2,
            get answer() {
                log.push("get");
                return this.value;
            },
            set answer(value) {
                log.push("set");
                this.value = value;
            },
        };

        const result = (object[key] += (log.push("rhs"), 3));
        expect(result).toBe(5);
        expect(object.value).toBe(5);
        expect(log).toEqual(["key", "get", "rhs", "set"]);
    });
});
