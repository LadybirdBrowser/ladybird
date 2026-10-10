// JIT code loads methods from prototypes as the constants they were at
// compile time.
// These tests run every function many times, so that with a low JIT
// threshold they are compiled.

const iterations = 100;

describe("methods as constants in JIT code", () => {
    test("methods replaced on the prototype are called", () => {
        class Model {
            value() {
                return 1;
            }
            twice() {
                return this.value() + this.value();
            }
        }
        const model = new Model();
        for (let i = 0; i < iterations; ++i) expect(model.twice()).toBe(2);
        Model.prototype.value = function () {
            return 5;
        };
        for (let i = 0; i < iterations; ++i) expect(model.twice()).toBe(10);
        Model.prototype.value = 7;
        expect(() => model.twice()).toThrow(TypeError);
    });

    test("methods replaced while they run", () => {
        class Counter {
            step() {
                if (this.count === 50) {
                    Counter.prototype.step = function () {
                        return 100;
                    };
                }
                ++this.count;
                return 1;
            }
            run() {
                return this.step() + this.step();
            }
        }
        const counter = new Counter();
        counter.count = 0;
        let sum = 0;
        for (let i = 0; i < iterations; ++i) sum += counter.run();
        expect(sum).toBe(51 + 100 * (2 * iterations - 51));
    });
});
