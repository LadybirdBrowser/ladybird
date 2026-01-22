function foo() {
    function bar() {}
    bar.baz = "value on bar";

    return bar;
}

foo.bippity = "boppity";
const fooResult = foo();

export const passed = fooResult.baz === "value on bar" && foo.bippity === "boppity";
