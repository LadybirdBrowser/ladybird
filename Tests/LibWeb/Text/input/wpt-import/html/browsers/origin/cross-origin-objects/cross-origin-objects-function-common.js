"use strict";

const crossOriginWindowMethods = [
    {key: "close", length: 0},
    {key: "focus", length: 0},
    {key: "blur", length: 0},
    {key: "postMessage", length: 1},
];

const crossOriginWindowAccessors = [
    "window",
    "self",
    "location",
    "closed",
    "frames",
    "length",
    "top",
    "opener",
    "parent",
].map(key => ({key}));

const makeCrossOriginWindow = t => {
    const iframe = document.createElement("iframe");
    const path = location.pathname.slice(0, location.pathname.lastIndexOf("/")) + "/frame.html";
    iframe.src = get_host_info().HTTP_REMOTE_ORIGIN + path;

    return new Promise((resolve, reject) => {
        iframe.onload = () => { resolve(iframe.contentWindow); };
        iframe.onerror = reject;

        document.body.append(iframe);
        t.add_cleanup(() => { iframe.remove(); });
    });
};
