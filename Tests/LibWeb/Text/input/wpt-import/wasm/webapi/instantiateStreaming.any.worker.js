
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("/resources/testharness.js");
importScripts("/wasm/jsapi/wasm-module-builder.js")
importScripts("/wasm/jsapi/assertions.js")
importScripts("/wasm/jsapi/instanceTestFactory.js")
importScripts("/wasm/webapi/instantiateStreaming.any.js");
done();
