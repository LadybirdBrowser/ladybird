// JIT code inlines the setters of accessors in prototypes that property sets
// call. These tests run every function many times, so that with a low JIT
// threshold they are compiled.

const iterations = 100;

class Temperature {
    constructor() {
        this._celsius = 0;
        this.writes = 0;
    }

    get celsius() {
        return this._celsius;
    }

    set celsius(value) {
        this._celsius = value;
        ++this.writes;
        return "ignored";
    }

    set fahrenheit(value) {
        this.celsius = ((value - 32) * 5) / 9;
    }
}

function setCelsius(temperature, value) {
    temperature.celsius = value;
}

function setFahrenheit(temperature, value) {
    temperature.fahrenheit = value;
}

describe("Inlined setters in JIT code", () => {
    test("setters of a class", () => {
        for (let i = 0; i < iterations; ++i) {
            const temperature = new Temperature();
            setCelsius(temperature, i);
            expect(temperature.celsius).toBe(i);
            setFahrenheit(temperature, 212);
            expect(temperature.celsius).toBe(100);
            expect(temperature.writes).toBe(2);
        }
    });

    test("the value of an assignment is the assigned value", () => {
        function assign(temperature, value) {
            const result = (temperature.celsius = value);
            const other = value + 1;
            return [result, other];
        }
        for (let i = 0; i < iterations; ++i) expect(assign(new Temperature(), i)).toEqual([i, i + 1]);
    });

    test("values the setter did not see before", () => {
        const temperature = new Temperature();
        for (let i = 0; i < iterations; ++i) setFahrenheit(temperature, 32 + 9 * i);
        expect(temperature.celsius).toBe(5 * (iterations - 1));
        setFahrenheit(temperature, 33.8);
        expect(temperature.celsius).toBeCloseTo(1);
        setFahrenheit(temperature, "50");
        expect(temperature.celsius).toBe(10);
    });

    test("setters that throw", () => {
        class Guarded {
            set value(value) {
                if (value < 0) throw new RangeError("negative");
                this._value = value;
            }
        }
        function set(object, value) {
            object.value = value;
        }
        const guarded = new Guarded();
        for (let i = 0; i < iterations; ++i) set(guarded, i);
        expect(guarded._value).toBe(iterations - 1);
        expect(() => set(guarded, -1)).toThrowWithMessage(RangeError, "negative");
        set(guarded, 7);
        expect(guarded._value).toBe(7);
    });

    test("setters that change", () => {
        class Box {
            set value(value) {
                this.stored = value;
            }
        }
        function set(box, value) {
            box.value = value;
        }
        for (let i = 0; i < iterations; ++i) {
            const box = new Box();
            set(box, i);
            expect(box.stored).toBe(i);
        }
        Object.defineProperty(Box.prototype, "value", {
            set(value) {
                this.stored = value * 2;
            },
            configurable: true,
        });
        const box = new Box();
        set(box, 5);
        expect(box.stored).toBe(10);

        Object.defineProperty(Box.prototype, "value", { value: 1, writable: true, configurable: true });
        const plain = new Box();
        set(plain, 3);
        expect(plain.stored).toBeUndefined();
        expect(Object.hasOwn(plain, "value")).toBeTrue();
    });

    test("setters that change to other closures of one function", () => {
        class Box {}
        function makeSetter(scale) {
            return function (value) {
                this.stored = value * scale;
            };
        }
        function set(box, value) {
            box.value = value;
        }
        for (let scale = 1; scale <= 4; ++scale) {
            Object.defineProperty(Box.prototype, "value", { set: makeSetter(scale), configurable: true });
            for (let i = 0; i < iterations; ++i) {
                const box = new Box();
                set(box, i);
                expect(box.stored).toBe(i * scale);
            }
        }
    });

    test("setters with a getter only", () => {
        class ReadOnly {
            get value() {
                return 1;
            }
        }
        function set(object, value) {
            "use strict";
            object.value = value;
        }
        expect(() => set(new ReadOnly(), 2)).toThrow(TypeError);
    });
});
