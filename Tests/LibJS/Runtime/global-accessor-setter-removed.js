function defineAccessor(name, setter) {
    Object.defineProperty(globalThis, name, {
        get() {
            return 1;
        },
        set: setter,
        configurable: true,
    });
}

test("a sloppy store to a global accessor whose setter was removed does nothing", () => {
    defineAccessor("accessorForAssignment", () => {});
    function assign() {
        accessorForAssignment = 2;
    }
    assign();
    assign();
    defineAccessor("accessorForAssignment", undefined);
    expect(assign).not.toThrow();
    expect(accessorForAssignment).toBe(1);

    defineAccessor("accessorForUpdate", () => {});
    function update() {
        accessorForUpdate++;
        accessorForUpdate += 2;
        [accessorForUpdate] = [3];
    }
    update();
    update();
    defineAccessor("accessorForUpdate", undefined);
    expect(update).not.toThrow();
    delete globalThis.accessorForAssignment;
    delete globalThis.accessorForUpdate;
});

test("a strict store to a global accessor whose setter was removed throws", () => {
    defineAccessor("accessorForStrictAssignment", () => {});
    function assign() {
        "use strict";
        accessorForStrictAssignment = 2;
    }
    assign();
    assign();
    defineAccessor("accessorForStrictAssignment", undefined);
    expect(assign).toThrowWithMessage(TypeError, "Object's [[Set]] method returned false");
    delete globalThis.accessorForStrictAssignment;
});

test("a store to a global accessor whose setter was replaced calls the setter that is there", () => {
    const calls = [];
    defineAccessor("accessorWithReplacedSetter", value => calls.push(["first", value]));
    function assign(value) {
        accessorWithReplacedSetter = value;
    }
    assign(1);
    assign(2);
    defineAccessor("accessorWithReplacedSetter", value => calls.push(["second", value]));
    assign(3);
    expect(calls).toEqual([
        ["first", 1],
        ["first", 2],
        ["second", 3],
    ]);
    delete globalThis.accessorWithReplacedSetter;
});
