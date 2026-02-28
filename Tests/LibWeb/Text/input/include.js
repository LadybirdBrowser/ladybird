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

// Multi-origin server support for cross-origin testing
class MultiOriginTestServer {
    constructor(origins) {
        this.origins = origins;
    }

    // Get the number of available origins
    get count() {
        return this.origins.length;
    }

    // Get a specific origin by index
    getOrigin(index) {
        if (index < 0 || index >= this.origins.length) {
            throw new Error(`Origin index ${index} out of range (0-${this.origins.length - 1})`);
        }
        return this.origins[index];
    }

    // Create an echo endpoint on a specific origin
    async createEcho(originIndex, method, path, options = {}) {
        const origin = this.getOrigin(originIndex);
        const result = await fetch(`${origin.baseURL}/echo`, {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ ...options, method, path }),
        });
        if (!result.ok) {
            throw new Error(`Error creating echo on origin ${originIndex}: ${result.statusText}`);
        }
        return `${origin.baseURL}${path}`;
    }

    // Get static file URL for a specific origin
    getStaticURL(originIndex, path) {
        const origin = this.getOrigin(originIndex);
        return `${origin.baseURL}/static/${path}`;
    }

    // Helper: Create an iframe pointing to another origin
    createCrossOriginIframe(originIndex, path, options = {}) {
        const url = path.startsWith('http') ? path : this.getStaticURL(originIndex, path);
        const iframe = document.createElement('iframe');
        iframe.src = url;
        if (options.sandbox) iframe.setAttribute('sandbox', options.sandbox);
        if (options.id) iframe.id = options.id;
        if (options.style) iframe.style.cssText = options.style;
        return iframe;
    }

    // Helper: Create a cross-origin window.open
    openCrossOriginWindow(originIndex, path) {
        const url = path.startsWith('http') ? path : this.getStaticURL(originIndex, path);
        return window.open(url, '_blank');
    }

    // Helper: Check if two origin indices represent different origins
    areDifferentOrigins(index1, index2) {
        return this.getOrigin(index1).baseURL !== this.getOrigin(index2).baseURL;
    }

    // Get all origins as an array of base URLs
    getAllOrigins() {
        return this.origins.map(o => o.baseURL);
    }
}

const __multiOriginTestServer = (function () {
    if (!globalThis.internals || !globalThis.internals.getOriginServerCount)
        return null;

    const count = internals.getOriginServerCount();
    if (count === 0)
        return null;

    const origins = [];
    for (let i = 0; i < count; i++) {
        const port = internals.getOriginServerPort(i);
        origins.push({
            index: i,
            port: port,
            baseURL: `http://127.0.0.1:${port}`,
        });
    }

    return new MultiOriginTestServer(origins);
})();

function multiOriginTestServer() {
    if (!__multiOriginTestServer) {
        throw new Error("Multi-origin test server not available. Ensure window.internals is exposed and multi-origin server is running.");
    }
    return __multiOriginTestServer;
}

// Convenience function to check if multi-origin testing is available
function hasMultiOriginSupport() {
    return __multiOriginTestServer !== null && __multiOriginTestServer.count > 0;
}
