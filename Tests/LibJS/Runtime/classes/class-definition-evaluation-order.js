describe("class definition evaluation order", () => {
    test("a computed key is converted to a property key before the next key is evaluated", () => {
        const log = [];
        const key = {
            toString() {
                log.push("first toString");
                return "first";
            },
        };

        class C {
            [key]() {}
            [(log.push("second key"), "second")]() {}
        }

        expect(log).toEqual(["first toString", "second key"]);
        expect(Object.getOwnPropertyNames(C.prototype)).toEqual(["constructor", "first", "second"]);
    });

    test("a later computed key can't change an earlier one", () => {
        function defineClass() {
            let key = "first";
            class C {
                [key]() {}
                static [((key = "changed"), "second")] = 1;
            }
            return C;
        }

        const C = defineClass();
        expect(Object.getOwnPropertyNames(C.prototype)).toEqual(["constructor", "first"]);
        expect(C.second).toBe(1);
    });

    test("the superclass is the value the heritage evaluated to", () => {
        function reassignToNonConstructor() {
            let Base = class {};
            const Original = Base;
            const C = class extends Base {
                [((Base = 1), "m")]() {}
            };
            return Object.getPrototypeOf(C) === Original;
        }

        function reassignToOtherConstructor() {
            class First {}
            class Second {}
            let Base = First;
            const C = class extends Base {
                [((Base = Second), "m")]() {}
            };
            return [Object.getPrototypeOf(C) === First, Object.getPrototypeOf(C.prototype) === First.prototype];
        }

        expect(reassignToNonConstructor()).toBeTrue();
        expect(reassignToOtherConstructor()).toEqual([true, true]);
    });

    test("the heritage is checked before computed keys are evaluated", () => {
        const log = [];
        expect(() => {
            class C extends (0, 1) {
                [(log.push("key"), "m")]() {}
            }
        }).toThrowWithMessage(TypeError, "Class extends value 1 is not a constructor or null");
        expect(log).toEqual([]);

        const Base = new Proxy(function () {}, {
            get(target, property) {
                log.push(`get ${String(property)}`);
                return target[property];
            },
        });
        class D extends Base {
            [(log.push("key"), "m")]() {}
        }
        expect(log).toEqual(["get prototype", "key"]);
    });

    test("a class named by a computed property key has its name while its static elements are evaluated", () => {
        const key = "named";
        const symbol = Symbol("description");
        const object = {
            [key]: class {
                static seenName = this.name;
            },
            [symbol]: class {
                static seenName = this.name;
            },
        };

        expect(object.named.seenName).toBe("named");
        expect(object.named.name).toBe("named");
        expect(object[symbol].seenName).toBe("[description]");
    });
});
