async function hasCookie(url, cookieName) {
    let response = await fetch(url, { credentials: "include", cache: "no-store" });
    let body = await response.json();
    return cookieHeaderContainsCookie(body["Cookie"], cookieName);
}

function cookieHeaderContainsCookie(cookieHeaderValues, cookieName) {
    let cookies = (cookieHeaderValues ?? []).flatMap(header => header.split(";")).map(cookie => cookie.trim());
    return cookies.some(cookie => cookie.startsWith(cookieName + "="));
}

async function waitForCookie(url, cookieName, timeoutMs = 10000) {
    return (await waitForCookies(url, [cookieName], timeoutMs)).get(cookieName);
}

async function waitForCookies(url, cookieNames, timeoutMs = 10000) {
    let deadline = performance.now() + timeoutMs;
    let observed = new Map(cookieNames.map(cookieName => [cookieName, false]));

    while (performance.now() < deadline) {
        for (let cookieName of cookieNames) {
            if (observed.get(cookieName)) continue;
            if (await hasCookie(url, cookieName)) observed.set(cookieName, true);
        }

        if ([...observed.values()].every(value => value)) break;

        await timeout(100);
    }

    return observed;
}

function anyCookiesObserved(observedCookies) {
    return [...observedCookies.values()].some(observed => observed);
}

async function waitForActivated(result, timeoutMs = 5000) {
    let deadline = performance.now() + timeoutMs;
    while (performance.now() < deadline) {
        if (result.activated) return true;
        await timeout(50);
    }
    return false;
}

function waitForMessage(expectedType, timeoutMs = 10000) {
    return new Promise((resolve, reject) => {
        let timeoutId = setTimeout(() => {
            window.removeEventListener("message", onMessage);
            reject(new Error(`Timed out waiting for message: ${expectedType}`));
        }, timeoutMs);

        function onMessage(event) {
            if (!event.data || event.data.type !== expectedType) return;
            clearTimeout(timeoutId);
            window.removeEventListener("message", onMessage);
            resolve(event.data);
        }

        window.addEventListener("message", onMessage);
    });
}
