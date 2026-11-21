function forward(fn, args) {
    return fn(...args);
}

function rest(fn, ...args) {
    return fn(...args);
}

function method(object, args) {
    return object.fn(...args);
}

function forwardArguments(fn) {
    return fn(...arguments);
}

function callee(...args) {
    return args.length;
}

forward(callee, [1, 2]);
rest(callee, 1, 2);
method({ fn: callee }, [1, 2]);
forwardArguments(callee, 1, 2);
