var __outputElement = null;
let __alreadyCalledTest = false;
const testTimeoutMilliseconds = 10000;

function __preventMultipleTestFunctions() {
    if (__alreadyCalledTest) {
        throw new Error("You must only call test() or asyncTest() once per page");
    }
    __alreadyCalledTest = true;
}

if (globalThis.internals === undefined) {
    internals = {
        signalTextTestIsDone: function () {},
    };
}

function println(s) {
    __outputElement.appendChild(document.createTextNode(s + "\n"));
}

function printElement(e) {
    let element_string = `<${e.nodeName} `;
    if (e.id) element_string += `id="${e.id}" `;
    element_string += ">";
    println(element_string);
}

function animationFrame() {
    const { promise, resolve } = Promise.withResolvers();
    requestAnimationFrame(resolve);
    return promise;
}

function timeout(ms) {
    const { promise, resolve } = Promise.withResolvers();
    setTimeout(resolve, ms);
    return promise;
}

document.addEventListener("DOMContentLoaded", function () {
    __outputElement = document.createElement("pre");
    __outputElement.setAttribute("id", "out");
    document.body.appendChild(__outputElement);
});

function test(f) {
    __preventMultipleTestFunctions();
    setTimeout(() => {
        println(`Test timed out after ${testTimeoutMilliseconds}ms`);
        internals.signalTextTestIsDone();
    }, testTimeoutMilliseconds);
    document.addEventListener("DOMContentLoaded", f);
    window.addEventListener("load", () => {
        internals.signalTextTestIsDone();
    });
}

function asyncTest(f) {
    const done = () => {
        __preventMultipleTestFunctions();
        internals.signalTextTestIsDone();
    };
    setTimeout(() => {
        println(`Test timed out after ${testTimeoutMilliseconds}ms`);
        internals.signalTextTestIsDone();
    }, testTimeoutMilliseconds);
    document.addEventListener("DOMContentLoaded", () => {
        f(done);
    });
}

function promiseTest(f) {
    document.addEventListener("DOMContentLoaded", () => {
        f().then(() => {
            __preventMultipleTestFunctions();
            internals.signalTextTestIsDone();
        });
    });
}
