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

test("compiling a function that runs out of stack inside a property with a string key", () => {
    const lazy = new Function(`return { "a-string-key": ${deeplyNested} };`);
    callAtEveryStackDepth(lazy);
    expect(lazy()).toEqual({ "a-string-key": 1 });
});

test("compiling a function that runs out of stack before the constructor of a class", () => {
    const lazy = new Function(`class C { static x = ${deeplyNested}; constructor() {} } return C;`);
    callAtEveryStackDepth(lazy);
    expect(lazy().x).toBe(1);
});

test("compiling a function that runs out of stack inside an arrow function", () => {
    const lazy = new Function(`return (a => ${deeplyNested})();`);
    callAtEveryStackDepth(lazy);
    expect(lazy()).toBe(1);
});
