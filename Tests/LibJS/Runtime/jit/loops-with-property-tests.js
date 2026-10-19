// Loops whose test reads the bound from a property, or runs other code, and that compiled code enters at that test.
// These tests run each loop long enough to be compiled with a low JIT threshold first, often from inside it.

const iterations = 100;

describe("Loops with tests that do more than compare", () => {
    test("bounds read from properties", () => {
        function Holder(values) {
            this.values = values;
            this.count = values.length;
        }
        Holder.prototype.sum = function () {
            let total = 0;
            for (let i = 0; i < this.count; ++i) total += this.values[i];
            return total;
        };
        const holder = new Holder(Array.from({ length: 1000 }, (_, i) => i));
        for (let i = 0; i < iterations; ++i) expect(holder.sum()).toBe(499500);
        expect(new Holder([]).sum()).toBe(0);
        expect(new Holder([1.5, 2]).sum()).toBe(3.5);
    });

    test("tests that run code", () => {
        let calls = 0;
        const object = {
            get length() {
                ++calls;
                return 300;
            },
        };
        function count(object) {
            let n = 0;
            for (let i = 0; i < object.length; ++i) ++n;
            return n;
        }
        for (let i = 0; i < iterations; ++i) {
            calls = 0;
            expect(count(object)).toBe(300);
            expect(calls).toBe(301);
        }
    });

    test("bounds that change in the loop", () => {
        function shrink(object) {
            let n = 0;
            for (let i = 0; i < object.length; ++i) {
                if (i === 100) object.length = 150;
                ++n;
            }
            return n;
        }
        for (let i = 0; i < iterations; ++i) expect(shrink({ length: 1000 })).toBe(150);
        expect(shrink({ length: 50 })).toBe(50);
        expect(shrink({ length: "80" })).toBe(80);
    });

    test("while loops and loops that never run", () => {
        function drain(queue) {
            let total = 0;
            while (queue.items.length > 0) total += queue.items.pop();
            return total;
        }
        for (let i = 0; i < iterations; ++i) {
            expect(drain({ items: Array.from({ length: 200 }, (_, j) => j) })).toBe(19900);
            expect(drain({ items: [] })).toBe(0);
        }
    });

    test("nested loops", () => {
        function total(rows) {
            let sum = 0;
            for (let r = 0; r < rows.length; ++r) {
                for (let c = 0; c < rows[r].length; ++c) sum += rows[r][c];
            }
            return sum;
        }
        const rows = Array.from({ length: 30 }, (_, r) => Array.from({ length: r }, (_, c) => c));
        let expected = 0;
        for (let r = 0; r < 30; ++r) expected += (r * (r - 1)) / 2;
        for (let i = 0; i < iterations; ++i) expect(total(rows)).toBe(expected);
        expect(total([[1], [], [2, 3]])).toBe(6);
    });

    test("stores into arrays made with a length", () => {
        function Signal(length, scale) {
            this.length = length;
            this.samples = new Array(length);
            this.scale = scale;
        }
        Signal.prototype.fill = function () {
            for (let i = 0; i < this.length; ++i) this.samples[i] = Math.round(i * this.scale) % 16;
            return this.samples;
        };
        for (let i = 0; i < 20; ++i) {
            const samples = new Signal(1000, 0.75).fill();
            expect(samples.length).toBe(1000);
            expect(Object.keys(samples).length).toBe(1000);
            expect(samples[999]).toBe(Math.round(999 * 0.75) % 16);
            expect(samples.indexOf(undefined)).toBe(-1);
        }
        const signal = new Signal(10, 1);
        signal.samples.length = 5;
        expect(signal.fill()).toEqual([0, 1, 2, 3, 4, 5, 6, 7, 8, 9]);
    });
});
