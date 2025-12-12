var __outputElement = null;
let __alreadyCalledTest = false;
let __originalURL = null;
function __preventMultipleTestFunctions() {
    if (__alreadyCalledTest) {
        throw new Error("You must only call test() or asyncTest() once per page");
    }
    __alreadyCalledTest = true;
}

if (globalThis.internals === undefined) {
    internals = {
        signalTestIsDone: function () {},
        spoofCurrentURL: function (url) {},
    };
}

function __finishTest() {
    if (__originalURL) {
        internals.spoofCurrentURL(__originalURL);
    }
    internals.signalTestIsDone(__outputElement.innerText);
}

function spoofCurrentURL(url) {
    if (__originalURL === null) {
        __originalURL = document.location.href;
    }
    internals.spoofCurrentURL(url);
}

function println(s) {
    __outputElement.appendChild(document.createTextNode(s + "\n"));
}

function printElement(e) {
    let element_string = `<${e.nodeName}`;
    if (e.id) element_string += ` id="${e.id}"`;
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

const __testErrorHandlerController = new AbortController();
window.addEventListener(
    "error",
    event => {
        println(`Uncaught Error In Test: ${event.message}`);
        __finishTest();
    },
    { signal: __testErrorHandlerController.signal }
);

function removeTestErrorHandler() {
    __testErrorHandlerController.abort();
}

document.addEventListener("DOMContentLoaded", function () {
    __outputElement = document.createElement("pre");
    __outputElement.setAttribute("id", "out");
    document.body.appendChild(__outputElement);
});

function test(f) {
    __preventMultipleTestFunctions();
    document.addEventListener("DOMContentLoaded", f);
    window.addEventListener("load", () => {
        __finishTest();
    });
}

function asyncTest(f) {
    const done = () => {
        __preventMultipleTestFunctions();
        __finishTest();
    };
    document.addEventListener("DOMContentLoaded", () => {
        Promise.resolve(f(done)).catch(error => {
            println(`Caught error while running async test: ${error}`);
            done();
        });
    });
}

function promiseTest(f) {
    document.addEventListener("DOMContentLoaded", () => {
        f().then(() => {
            __preventMultipleTestFunctions();
            __finishTest();
        });
    });
}

class HTTPTestServer {
    constructor(baseURL) {
        this.baseURL = baseURL;
    }
    async createEcho(method, path, options) {
        const result = await fetch(`${this.baseURL}/echo`, {
            method: "POST",
            headers: {
                "Content-Type": "application/json",
            },
            body: JSON.stringify({ ...options, method, path }),
        });
        if (!result.ok) {
            throw new Error("Error creating echo: " + result.statusText);
        }
        return `${this.baseURL}${path}`;
    }

    async createBinaryEcho(method, path, options) {
        const mappedOptions = Object.entries(options ?? {}).reduce((accumulator, [key, value]) => {
            if (key === "headers" || key === "body")
                return accumulator;

            const header = `X-Echo-${key.replaceAll("_", "-")}`;
            accumulator[header] = value;
            return accumulator;
        }, {});

        const mappedHeaders = Object.entries(options.headers ?? {}).reduce((accumulator, [key, value]) => {
            const header = `X-Echo-Header-${key}`;
            accumulator[header] = value;
            return accumulator;
        }, {});

        const baseHeaders = {
            "Content-Type": "application/octet-stream",
            "X-Echo-Method": method,
            "X-Echo-Path": path,
        };

        const finalHeaders = Object.assign(baseHeaders, mappedOptions, mappedHeaders);

        const result = await fetch(`${this.baseURL}/echo-binary-data`, {
            method: "POST",
            headers: finalHeaders,
            body: options.body,
        });
        if (!result.ok) {
            throw new Error("Error creating binary echo: " + result.statusText);
        }
        return `${this.baseURL}${path}`;
    }

    getStaticURL(path) {
        return `${this.baseURL}/static/${path}`;
    }
}

const __httpTestServer = (function () {
    if (globalThis.internals && globalThis.internals.getEchoServerPort)
        return new HTTPTestServer(`http://127.0.0.1:${internals.getEchoServerPort()}`);

    return null;
})();

function httpTestServer() {
    if (!__httpTestServer) {
        throw new Error("window.internals must be exposed to use HTTPTestServer");
    }
    return __httpTestServer;
}
