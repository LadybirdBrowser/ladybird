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
});
