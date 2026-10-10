// JIT code never allocates objects that nothing but their property accesses
// see; exits create them. These tests run every function many times, so that
// with a low JIT threshold they are compiled, and then make them exit while
// such objects are live.

const iterations = 100;

describe("objects replaced by their properties in JIT code", () => {
    test("properties updated in loops and branches", () => {
        function count(n, step) {
            const counter = { total: 0, steps: 0 };
            for (let i = 0; i < n; ++i) {
                if (i % 2) counter.total += step;
                else counter.total += 2 * step;
                counter.steps++;
            }
            return counter.total * 1000 + counter.steps;
        }
        for (let i = 0; i < iterations; ++i) expect(count(5, 1)).toBe(8005);
        expect(count(3, 0.5)).toBe(2503);
        expect(count(2, "x")).toBe(NaN);
    });

    test("exits create the objects, once each, with their current properties", () => {
        function live(a, b, reveal) {
            const point = { x: a, y: b };
            const box = { point, z: 1 };
            const sum = a + b;
            point.y = sum;
            // NB: This call never runs before the function is compiled, so it
            //     is an exit, which needs the objects.
            if (reveal) return reveal(box, point);
            return box.z + point.x + point.y;
        }
        function aliased(a, reveal) {
            const object = { x: a };
            const alias = object;
            const doubled = a * 2;
            if (reveal) return reveal(alias, object, doubled);
            return alias.x + object.x + doubled;
        }
        for (let i = 0; i < iterations; ++i) {
            expect(live(i, 1, null)).toBe(2 * i + 2);
            expect(aliased(i, null)).toBe(4 * i);
        }
        const [box, point] = live(2, 3, (...values) => values);
        expect(box.point).toBe(point);
        expect(point).toEqual({ x: 2, y: 5 });
        expect(Object.keys(box)).toEqual(["point", "z"]);
        expect(box.z).toBe(1);
        const [alias, object, doubled] = aliased(4, (...values) => values);
        expect(alias).toBe(object);
        expect(object.x).toBe(4);
        expect(doubled).toBe(8);
        expect(live("s", 1, null)).toBe("1ss1");
    });

    test("objects that escape stay allocated", () => {
        function make(a) {
            const object = { a };
            const total = a + 1;
            return total > 0 ? object : null;
        }
        const seen = new Set();
        for (let i = 0; i < iterations; ++i) {
            const object = make(i);
            expect(object.a).toBe(i);
            expect(seen.has(object)).toBeFalse();
            seen.add(object);
        }
        expect(make(-5)).toBeNull();
        expect(make(0.5).a).toBe(0.5);
    });

    test("slow paths and calls that leave compiled code create the objects", () => {
        function throwsCaught(a, target) {
            const point = { x: a, y: a + 1 };
            let caught = null;
            try {
                target.missing.property;
            } catch (error) {
                caught = error;
            }
            point.y += 10;
            if (caught) return [point.x, point.y, caught instanceof TypeError];
            return point.x + point.y;
        }
        function callsThrow(a, f) {
            const point = { x: a, y: 2 };
            try {
                point.x += f(a);
            } catch (error) {
                return [point.x, point.y, error];
            }
            return point.x * point.y;
        }
        function inlinedThrows(object) {
            return object.missing.property;
        }
        function throwsInInlinedCallee(a, target) {
            const point = { x: a, y: 3 };
            let caught = null;
            try {
                inlinedThrows(target);
            } catch (error) {
                caught = error;
            }
            return caught ? [point.x, point.y] : point.x * point.y;
        }
        for (let i = 0; i < iterations; ++i) {
            expect(throwsCaught(i, { missing: {} })).toBe(2 * i + 11);
            expect(callsThrow(i, value => value + 1)).toBe(2 * (2 * i + 1));
            expect(throwsInInlinedCallee(i, { missing: {} })).toBe(3 * i);
        }
        expect(throwsCaught(3, {})).toEqual([3, 14, true]);
        expect(
            callsThrow(5, () => {
                throw "thrown";
            })
        ).toEqual([5, 2, "thrown"]);
        expect(throwsInInlinedCallee(7, {})).toEqual([7, 3]);
    });

    test("calls and slow paths that continue keep the objects virtual", () => {
        function callsAcross(a, f) {
            const point = { x: a, y: 2 };
            const inner = { point, z: a };
            const result = f(a);
            point.x += result;
            return point.x * point.y + inner.z + inner.point.y;
        }
        for (let i = 0; i < iterations; ++i) expect(callsAcross(i, value => value + 1)).toBe(2 * (2 * i + 1) + i + 2);
        expect(
            callsAcross(3, value => {
                gc();
                return value;
            })
        ).toBe(17);
    });

    test("collections while objects are virtual", () => {
        function churn(n) {
            const state = { values: 0, last: null };
            for (let i = 0; i < n; ++i) {
                state.values += i;
                if (i === n - 1) gc();
            }
            return state.values;
        }
        for (let i = 0; i < iterations; ++i) expect(churn(10)).toBe(45);
        expect(churn(1.5)).toBe(1);
    });
});
