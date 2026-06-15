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

async function waitForImageAnimationState(url, predicate, targetWindow = window) {
    return new Promise(async resolve => {
        while (true) {
            try {
                const state = targetWindow.internals.imageAnimationStateForURL(url);
                if (predicate(state)) return resolve(state);
            } catch {
                // The image hasn't loaded yet.
            }

            await animationFrame();
        }
    });
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
        const echoPath = `/echo${path}`;
        const result = await fetch(`${this.baseURL}/echo`, {
            method: "POST",
            headers: {
                "Content-Type": "application/json",
            },
            body: JSON.stringify({ ...options, method, path: echoPath }),
        });
        if (!result.ok) {
            throw new Error("Error creating echo: " + result.statusText);
        }
        return `${this.baseURL}${echoPath}`;
    }
    getStaticURL(path) {
        return `${this.baseURL}/static/${path}`;
    }
}

const __httpTestServer = (function () {
    if (globalThis.internals && globalThis.internals.getEchoServerPort) {
        const echoServerPort = internals.getEchoServerPort();
        const isLoadedFromEchoServer = location.protocol === "http:" && location.port === String(echoServerPort);

        // Tests loaded through the echo server should create echo URLs on their current origin,
        // so same-origin iframe/fetch checks keep working with unique localhost hostnames.
        const baseURL = isLoadedFromEchoServer ? location.origin : `http://localhost:${echoServerPort}`;
        return new HTTPTestServer(baseURL);
    }

    return null;
})();

function httpTestServer() {
    if (!__httpTestServer) {
        throw new Error("window.internals must be exposed to use HTTPTestServer");
    }
    return __httpTestServer;
}

// Per-call unique loopback host, so tests that mutate global per-host state
// (e.g. HSTS) don't collide under the parallel runner or --repeat clones.
function uniqueLocalhostHostname(prefix) {
    return `${prefix}-${crypto.randomUUID()}.localhost`;
}
