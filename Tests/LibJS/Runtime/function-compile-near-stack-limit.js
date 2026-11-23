// Calls `callback` with less and less of the native stack in use, starting from where the stack runs out.
function callAtEveryStackDepth(callback) {
    function recurse() {
        try {
            Reflect.apply(recurse, null, []);
        } catch {}
        try {
            callback();
        } catch {}
    }
    recurse();
}

const deeplyNested = "(".repeat(1900) + "1" + ")".repeat(1900);

test("compiling a function that runs out of stack before the constructor of a class", () => {
    const lazy = new Function(`class C { static x = ${deeplyNested}; constructor() {} } return C;`);
    callAtEveryStackDepth(lazy);
    expect(lazy().x).toBe(1);
});
