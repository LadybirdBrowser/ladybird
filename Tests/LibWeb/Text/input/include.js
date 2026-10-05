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

async function waitForCondition(condition, attempts = 600, interval = 25) {
    while (!condition()) {
        if (attempts-- === 0) return false;
        await timeout(interval);
    }
    return true;
}

async function setSystemVisibilityState(state) {
    // Tests assume that the document reflects the UI process's system visibility state and that
    // no previous visibility state change is still pending.
    if (document.visibilityState === state) {
        return;
    }

    const visibilityChanged = new Promise(resolve =>
        document.addEventListener("visibilitychange", resolve, { once: true })
    );
    internals.setSystemVisibilityState(state);
    await visibilityChanged;
}

function withCollectedWrapper(makeAndMark, reacquire, verify) {
    let wasPreserved = false;
    (() => {
        const wrapper = makeAndMark();
        wasPreserved = internals.wrapperIsPreserved(wrapper);
    })();

    if (!wasPreserved) {
        throw new Error("Expected wrapper to be preserved before GC");
    }

    internals.gc();
    verify(reacquire());
}

function scrollendEvent(target) {
    const { promise, resolve } = Promise.withResolvers();
    target.addEventListener("scrollend", resolve, { once: true });
    return promise;
}

async function scrollSettled(target, action) {
    const scrollend = scrollendEvent(target);
    await action();
    await scrollend;
}

function __describeNodeOfInputHook(node, documentName = "document") {
    // A nested document comes from another realm, whose Document it is an instance of.
    if (node.nodeType === Node.DOCUMENT_NODE) return node === document ? documentName : `nested ${documentName}`;
    if (node.id) return `#${node.id}`;
    return node.nodeName.toLowerCase();
}

function __describeScrollerOfInputHook(result) {
    if (!result.scroller) return "";
    let description = ` scroller=${__describeNodeOfInputHook(result.scroller, "viewport")}`;
    if (result.scrollerPseudoElement) description += `::${result.scrollerPseudoElement}`;
    return description;
}

// Describes where internals.compositorWheelTargetAt() or internals.recordedDisplayListWheelTargetAt() says a wheel step
// goes.
function wheelTargetToString(target) {
    return `${target.decision}${__describeScrollerOfInputHook(target)}`;
}

// Whether the compositor, holding a frame of this document, would scroll the first step of a wheel gesture at the
// position itself.
function compositorScrollsWheelStepAt(x, y, deltaX, deltaY) {
    const target = internals.compositorWheelTargetAt(x, y, deltaX, deltaY);
    return target.document === document && target.decision === "latched";
}

// Prints what internals.takeInputTrace() returned, one routing decision per line, optionally only those of the given
// stages. Input events are numbered in the order they first appear, as the ids the UI gives them keep growing from test
// to test; input that did not come from the UI is marked "-".
function printInputTrace(trace, { stages } = {}) {
    const eventNumbers = new Map();
    for (const record of trace) {
        let eventNumber = "-";
        if (record.id !== 0) {
            if (!eventNumbers.has(record.id)) eventNumbers.set(record.id, eventNumbers.size + 1);
            eventNumber = `#${eventNumbers.get(record.id)}`;
        }
        if (stages && !stages.includes(record.stage)) continue;
        let line = `${eventNumber} ${record.stage}: ${record.decision}${__describeScrollerOfInputHook(record)}`;
        if (record.target) line += ` target=${__describeNodeOfInputHook(record.target)}`;
        println(line);
    }
}

async function scrollOffsetStopsChanging(readScrollOffset) {
    let previousScrollOffset = null;
    while (previousScrollOffset !== readScrollOffset()) {
        previousScrollOffset = readScrollOffset();
        for (let frame = 0; frame < 5; frame++) {
            await animationFrame();
        }
    }
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

let __pageEchoScope = null;

// The echo server, the HTTP cache and the cookie jar are shared by every test and every --repeat run, so echo paths
// are scoped to the page that creates them. A test that spans page loads names a scope of its own to share them.
class HTTPTestServer {
    constructor(baseURL, echoScope = null) {
        this.baseURL = baseURL;
        this.echoScope = echoScope;
    }
    withEchoScope(echoScope) {
        return new HTTPTestServer(this.baseURL, echoScope);
    }
    echoURL(path) {
        return `${this.baseURL}${this.#echoPath(path)}`;
    }
    async createEcho(method, path, options) {
        const echoPath = this.#echoPath(path);
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
    recordedRequestHeadersURL(echoURL) {
        return `${this.baseURL}/recorded-request-headers${new URL(echoURL).pathname}`;
    }
    getStaticURL(path) {
        return `${this.baseURL}/static/${path}`;
    }
    #echoPath(path) {
        const echoScope = this.echoScope ?? (__pageEchoScope ??= uniqueID());
        return `/echo/${echoScope}${path}`;
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

// A random identifier that a page which is not a secure context can make too.
function uniqueID() {
    return Array.from(crypto.getRandomValues(new Uint8Array(16)), byte => byte.toString(16).padStart(2, "0")).join("");
}

// Per-call unique loopback host, so tests that mutate global per-host state
// (e.g. HSTS) don't collide under the parallel runner or --repeat clones.
function uniqueLocalhostHostname(prefix) {
    return `${prefix}-${uniqueID()}.localhost`;
}

// Per-call unique loopback host whose origin is not potentially trustworthy.
function uniqueTestHostname(prefix) {
    return `${prefix}-${uniqueID()}.test`;
}

const remoteFrameCount = () => (internals.dumpSiteIsolationProcessTree().match(/remote/g) || []).length;
