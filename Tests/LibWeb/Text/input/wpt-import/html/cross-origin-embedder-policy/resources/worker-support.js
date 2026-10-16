// Configures `url` such that the response carries a `COEP: ${value}` header.
//
// `url` must be a `URL` instance.
function setCoep(url, value) {
  url.searchParams
      .set("pipe", `header(cross-origin-embedder-policy,${value})`);
}

// Resolves the given `relativeUrl` relative to the current window's location.
//
// `options` can contain the following keys:
//
// - `coep`: value passed to `setCoep()`, if present.
// - `host`: overrides the host of the returned URL.
//
// Returns a `URL` instance.
function resolveUrl(relativeUrl, options) {
  const url = new URL(relativeUrl, window.location);

  if (options !== undefined) {
    const { coep, host } = options;
    if (coep !== undefined) {
      setCoep(url, coep);
    }
    if (host !== undefined) {
      url.host = host;
    }
  }

  return url;
}

// Adds an iframe loaded from `url` to the current document, waiting for it to
// load before returning.
//
// The returned iframe is removed from the document at the end of the test `t`.
async function withIframe(t, url) {
  const frame = document.createElement("iframe");
  frame.src = url;

  t.add_cleanup(() => frame.remove());

  const loadedPromise = new Promise(resolve => {
    frame.addEventListener('load', resolve, {once: true});
  });
  document.body.append(frame);
  await loadedPromise;

  return frame;
}

// Asynchronously waits for a single "message" event on the given `target`.
function waitForMessage(target) {
  return new Promise(resolve => {
    target.addEventListener('message', resolve, {once: true});
  });
}

// Fetches `url` from a document with COEP `creatorCoep`, then serializes it
// and returns a URL pointing to the fetched body with the given `scheme`.
//
// - `creatorCoep` is passed to `setCoep()`.
// - `scheme` may be one of: "blob", "data" or "filesystem".
//
// The returned URL is valid until the end of the test `t`.
async function createLocalUrl(t, { url, creatorCoep, scheme }) {
  const frameUrl = resolveUrl("resources/fetch-and-create-url.html", {
    coep: creatorCoep,
  });
  frameUrl.searchParams.set("url", url);
  frameUrl.searchParams.set("scheme", scheme);

  const messagePromise = waitForMessage(window);
  const frame = await withIframe(t, frameUrl);

  const evt = await messagePromise;
  const message = evt.data;
  assert_equals(message.error, undefined, "url creation error");

  return message.url;
}
