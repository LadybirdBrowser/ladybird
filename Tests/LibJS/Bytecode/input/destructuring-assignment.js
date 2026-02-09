// Test that variables used in destructuring assignment patterns are correctly
// resolved as locals/globals after synthesize_binding_pattern re-parsing.

function array_destructuring_with_class_default() {
    var x;
    [x = class C {}] = [undefined];
    return x;
}

function object_destructuring_with_function_default() {
    var x;
    ({ x = function () {} } = {});
    return x;
}

function setter_parameter_resolution() {
    var setValue;
    [
        {
            set y(val) {
                setValue = val;
            },
        }.y,
    ] = [23];
    return setValue;
}

array_destructuring_with_class_default();
object_destructuring_with_function_default();
setter_parameter_resolution();
