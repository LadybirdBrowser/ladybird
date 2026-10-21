
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("/resources/testharness.js");
self.META_TITLE = "EventSource: \"retry\" field (bogus)";
importScripts("/eventsource/format-field-retry-bogus.any.js");
done();
