
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("/resources/testharness.js");
importScripts("/wasm/jsapi/wasm-module-builder.js")
importScripts("/wasm/jsapi/bad-imports.js")
importScripts("/wasm/webapi/instantiateStreaming-bad-imports.any.js");
done();
