"use strict";

{
// private variables are defined with `const` so they don't leak outside this block statement
const IFRAME_DEFAULT_SIZE = "200";
const iframes = new WeakMap();

// helpers are defined with `var` so they are globally accessible
var createMQL = async t => {
    const iframe = await createIFrame(t);
    const mql = iframe.contentWindow.matchMedia(`(max-width: ${IFRAME_DEFAULT_SIZE}px)`);
    assert_true(mql.matches, "MQL should match on newly created <iframe>");
    iframes.set(mql, iframe);
    return mql;
};

var createIFrame = (t, width = IFRAME_DEFAULT_SIZE, height = width) => {
    assert_not_equals(document.body, null, "<body> element is missing");

    const iframe = document.createElement("iframe");
    iframe.srcdoc = "";
    iframe.width = String(width);
    iframe.height = String(height);
    iframe.style.border = "none";

    t.add_cleanup(() => {
        document.body.removeChild(iframe);
    });

    return new Promise(resolve => {
        iframe.addEventListener("load", () => {
            iframe.contentDocument.body.offsetWidth; // reflow
            resolve(iframe);
        });

        document.body.appendChild(iframe);
    });
};

var triggerMQLEvent = mql => {
    const iframe = iframes.get(mql);
    assert_not_equals(iframe, undefined, "Passed MQL instance was not created with createMQL");
    iframe.width = iframe.width === IFRAME_DEFAULT_SIZE ? "250" : IFRAME_DEFAULT_SIZE;
};

var getWindow = mql => {
    const iframe = iframes.get(mql);
    assert_not_equals(iframe, undefined, "Passed MQL instance was not created with createMQL");
    return iframe.contentWindow;
};

var waitForChangesReported = () => {
    return new Promise(resolve => {
        requestAnimationFrame(() => {
            requestAnimationFrame(resolve);
        });
    });
};

}
