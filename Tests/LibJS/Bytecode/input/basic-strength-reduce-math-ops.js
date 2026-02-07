function multiply_by_one(x) {
    return x * 1;
}
function multiply_by_one_alt(x) {
    return 1 * x;
}
function multiply_by_negative_one(x) {
    return x * -1;
}
function multiply_by_negative_one_alt(x) {
    return -1 * x;
}

function divide_by_negative_one(x) {
    return x / -1;
}
function divide_by_one(x) {
    return x / 1;
}

function f() {
    return 2;
}
function square(x) {
    return x ** 2;
}
function square_func() {
    return f() ** 2;
}
function power_of_one(x) {
    return x ** 1;
}
function power_of_zero(x) {
    return x ** 0;
}

console.log(multiply_by_one(123));
console.log(multiply_by_one_alt(123));
console.log(multiply_by_negative_one(123));
console.log(multiply_by_negative_one_alt(123));

console.log(divide_by_one(123));
console.log(divide_by_negative_one(123));

console.log(square(10));
console.log(square_func(10));
console.log(power_of_one(10));
console.log(power_of_zero(10));
