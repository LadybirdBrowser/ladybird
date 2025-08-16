// Returns a promise that asserts the "load" and "pageshow" events are not
// fired on |target|.
function assertNoLoadAndPageshowEvent(t, target) {
  target.addEventListener("load", t.unreached_func("load should not be fired"));
  target.addEventListener("pageshow", t.unreached_func("pageshow should not be fired"));
  return new Promise(resolve => {
    // Wait 50ms to ensure events fired after asynchronous navigations are
    // also captured.
    setTimeout(resolve, 50);
  });
}

const url204 = "/common/blank.html?pipe=status(204)";
const postMessageToOpenerOnLoad = `
    window.onload = () => {
      window.opener.postMessage("loaded", "*")
    }
  `;

// -- Start of helpers for iframe initial empty document tests.

// Creates an iframe with an unset src and appends it to the document.
window.insertIframe = (t) => {
  const iframe = document.createElement("iframe");
  t.add_cleanup(() => iframe.remove());
  document.body.append(iframe);
  return iframe;
};

// Creates an iframe with src set to a URL that doesn't commit a new document
// (results in a HTTP 204 response) and appends it to the document.
window.insertIframeWith204Src = (t) => {
  const iframe = document.createElement("iframe");
  iframe.src = url204;
  t.add_cleanup(() => iframe.remove());
  document.body.append(iframe);
  return iframe;
};

// Creates an iframe with src="about:blank" and appends it to the document.
window.insertIframeWithAboutBlankSrc = (t) => {
  const iframe = document.createElement("iframe");
  t.add_cleanup(() => iframe.remove());
  iframe.src = "about:blank";
  document.body.append(iframe);
  return iframe;
};

// Creates an iframe with src="about:blank", appends it to the document, and
// waits for the non-initial about:blank document finished loading.
window.insertIframeWithAboutBlankSrcWaitForLoad = async (t) => {
  const iframe = insertIframeWithAboutBlankSrc(t);
  const aboutBlankLoad = new Promise(resolve => {
    // In some browsers, the non-initial about:blank navigation commits
    // asynchronously, while in other browsers, it would commit synchronously.
    // This means we can't wait for the "load" event as it might have already
    // ran. Instead, just wait for 100ms before resolving, as the non-initial
    // about:blank navigation will most likely take less than 100 ms to commit.
    t.step_timeout(resolve, 100);
  });
  await aboutBlankLoad;
  return iframe;
};

// Waits for the "load" event for |urlRelativeToThisDocument| to run on
// |iframe|.
window.waitForLoad = (t, iframe, urlRelativeToThisDocument) => {
  return new Promise(resolve => {
    iframe.addEventListener("load", t.step_func(() => {
      assert_equals(iframe.contentWindow.location.href, (new URL(urlRelativeToThisDocument, location.href)).href);

      // Wait a bit longer to ensure all history stuff has settled, e.g. the document is "completely loaded"
      // (which happens from a queued task).
      setTimeout(resolve, 0);
    }), { once: true });
  });
};

// -- End of helpers for iframe initial empty document tests.

// -- Start of helpers for opened windows' initial empty document tests.

// window.open() to a URL that doesn't load a new document (results in a HTTP
// 204 response). This should create a new window that stays on the initial
// empty document.
window.windowOpen204 = (t) => {
  const openedWindow = window.open(url204);
  t.add_cleanup(() => openedWindow.close());
  return openedWindow;
};

// window.open() (no URL set). This should create a new window that stays on
// the initial empty document as it won't trigger a non-initial about:blank
// navigation.
window.windowOpenNoURL = (t) => {
  const openedWindow = window.open();
  t.add_cleanup(() => openedWindow.close());
  return openedWindow;
};

// window.open("about:blank"). This should create a new window that stays on
// the initial empty document as it won't trigger a non-initial about:blank
// navigation.
window.windowOpenAboutBlank = (t) => {
  const openedWindow = window.open("about:blank");
  t.add_cleanup(() => openedWindow.close());
  return openedWindow;
};

// Waits for a postMessage with data set to |message| is received on the current
// window.
window.waitForMessage = (t, message) => {
  return new Promise(resolve => {
    window.addEventListener("message", t.step_func((event) => {
      if (event.data == message)
        resolve();
    }));
  });
};

// -- End of helpers for opened windows' initial empty document tests.
