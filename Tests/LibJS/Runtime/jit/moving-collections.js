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

const compiled = [
    [sumOfPoint, () => sumOfPoint(holder.point), 3],
    [addZ, () => addZ({ x: 1, y: 2 }).z, 3],
    [sumOfArray, () => sumOfArray(holder.array), 6],
    [callCallback, () => callCallback(), 2],
    [callReadsCaptured, () => callReadsCaptured(), 42],
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

    test("calls of a known function start its frames with its environment after it moved", () => {
        holder.setCaptured(99);
        expect(callReadsCaptured()).toBe(100);
        holder.setCaptured(41);
    });
});
