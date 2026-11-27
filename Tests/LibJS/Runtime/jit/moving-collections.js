// Compiled code holds the shapes, prototypes, functions and other cells it was compiled for. A collection that moves
// them has the code follow them, so that the code keeps running without exits on the cells that moved.

const holder = {
    point: { x: 1, y: 2 },
    array: [1, 2, 3],
    callback: value => value + 1,
};

function sumOfPoint(point) {
    return point.x + point.y;
}

function addZ(point) {
    point.z = 3;
    return point;
}

function sumOfArray(array) {
    let sum = 0;
    for (const element of array) sum += element;
    return sum;
}

function callCallback() {
    return holder.callback(1);
}

const prototype = {
    method() {
        return 1;
    },
};
const instance = Object.create(prototype);

function callMethod() {
    return instance.method();
}

// After the call, the code reads the properties without checking the shape again, as long as no object left it.
function makeTriple() {
    return { first: 1, second: 2, third: 3 };
}
holder.triple = makeTriple();

function readAroundCall(object, call) {
    const first = object.first;
    call(object);
    return [first, object.second, object.third];
}

// Calls of a known function start its frames with its environment, which only the function holds.
// Changing the binding after the move tells the environment from a stale copy of it.
let keptBlock;
[holder.readsCaptured, holder.setCaptured] = (() => {
    let captured = 41;
    return [
        // NB: The block environment keeps the call from being inlined.
        function readsCaptured() {
            const result = captured + 1;
            {
                let inBlock = result;
                keptBlock = () => inBlock;
            }
            return result;
        },
        value => (captured = value),
    ];
})();

function callReadsCaptured() {
    return holder.readsCaptured();
}

// Exits in an inlined call make the frame of the callee from its executable, which the code holds.
function inlinedReader(point, throws) {
    if (throws) throw new Error("thrown");
    return point.x;
}

function callsInlinedReader(point, throws) {
    return inlinedReader(point, throws) + 1;
}

// Exits make the objects the code never created from the shapes it holds, which may have moved: a number that is not
// an int32 exits while the object is live.
function exitsWithUncreatedObject(n) {
    const object = { a: n, b: 2 };
    const doubled = n * 2;
    return object.a + object.b + doubled;
}

// NB: Many callees make the call generic, so that the code calls whatever it is given.
const stays = Array.from({ length: 8 }, () => () => {});
let stayIndex = 0;

const compiled = [
    [sumOfPoint, () => sumOfPoint(holder.point), 3],
    [addZ, () => addZ({ x: 1, y: 2 }).z, 3],
    [sumOfArray, () => sumOfArray(holder.array), 6],
    [callCallback, () => callCallback(), 2],
    [callMethod, () => callMethod(), 1],
    [readAroundCall, () => readAroundCall(makeTriple(), stays[stayIndex++ % stays.length]).join(), "1,2,3"],
    [callReadsCaptured, () => callReadsCaptured(), 42],
    [callsInlinedReader, () => callsInlinedReader(holder.point, false), 2],
    [exitsWithUncreatedObject, () => exitsWithUncreatedObject(1), 5],
];
for (const [f, run] of compiled) {
    jit.prepare(f);
    for (let i = 0; i < 10; ++i) run();
    jit.compile(f);
}
const exitCounts = compiled.map(([f, run, expected]) => {
    expect(run()).toBe(expected);
    return jit.exitCount(f);
});
const pointBefore = addressOf(holder.point);
const prototypeBefore = addressOf(prototype);

runAfterMovingCollection(() => {
    test("compiled code runs on cells that moved without exits", () => {
        expect(addressOf(holder.point)).not.toBe(pointBefore);
        for (const [index, [f, run, expected]] of compiled.entries()) {
            for (let i = 0; i < 10; ++i) expect(run()).toBe(expected);
            // NB: Code that exits too often is dropped.
            if (jit.forcesExits) continue;
            expect(jit.isCompiled(f)).toBe(jit.enabled);
            expect(jit.exitCount(f)).toBe(exitCounts[index]);
        }
    });

    test("compiled code that depends on cells that moved stops running when they change", () => {
        expect(addressOf(prototype)).not.toBe(prototypeBefore);
        prototype.method = () => 2;
        expect(callMethod()).toBe(2);
        const remove = object => {
            delete object.first;
        };
        expect(readAroundCall(holder.triple, remove)).toEqual([1, 2, 3]);
        // NB: test-js may run the test again.
        holder.triple = makeTriple();
    });

    test("exits in inlined calls make frames of executables that moved", () => {
        expect(callsInlinedReader({ y: 0, x: 5 }, false)).toBe(6);
        let error;
        try {
            callsInlinedReader(holder.point, true);
        } catch (e) {
            error = e;
        }
        expect(error.stack.includes("at inlinedReader (")).toBeTrue();
        expect(error.stack.includes("at callsInlinedReader (")).toBeTrue();
    });

    test("exits create objects with shapes that moved", () => {
        expect(exitsWithUncreatedObject(1.5)).toBe(6.5);
    });

    test("calls of a known function start its frames with its environment after it moved", () => {
        holder.setCaptured(99);
        expect(callReadsCaptured()).toBe(100);
        holder.setCaptured(41);
    });
});
