"use strict";

// Appends an iframe with the given srcdoc to the document and resolves with it
// once it has loaded. The iframe is removed again during cleanup (removing an
// already-removed iframe is a no-op).
globalThis.addFrame = (t, srcdoc = "") => {
  const frame = document.createElement("iframe");
  frame.srcdoc = srcdoc;
  return appendAndWait(t, frame);
};

// As above, but navigating to a URL, so that the iframe can be navigated again
// later. (Setting src on an iframe with a srcdoc attribute has no effect.)
globalThis.addSrcFrame = (t, src) => {
  const frame = document.createElement("iframe");
  frame.src = src;
  return appendAndWait(t, frame);
};

// Navigates an already-loaded iframe and resolves once the new document has
// loaded.
globalThis.navigateFrame = (frame, url) => {
  const loaded = new Promise(resolve => {
    frame.addEventListener("load", resolve, { once: true });
  });
  frame.src = url;
  return loaded;
};

function appendAndWait(t, frame) {
  const loaded = new Promise(resolve => {
    frame.addEventListener("load", resolve, { once: true });
  });
  t.add_cleanup(() => frame.remove());
  document.body.append(frame);
  return loaded.then(() => frame);
}
