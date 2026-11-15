// META: global=window,worker
// META: timeout=long
// META: script=/resources/WebIDLParser.js
// META: script=/resources/idlharness.js
// META: script=/service-workers/service-worker/resources/test-helpers.sub.js
'use strict';

// https://cookiestore.spec.whatwg.org/

idl_test(
  ['cookiestore'],
  ['service-workers', 'html', 'dom'],
  async (idl_array, t) => {
    const isServiceWorker = 'ServiceWorkerGlobalScope' in self
      && self instanceof ServiceWorkerGlobalScope;

    if (isServiceWorker) {
      idl_array.add_objects({
        ExtendableCookieChangeEvent: [
            'new ExtendableCookieChangeEvent("cookiechange")'],
        ServiceWorkerGlobalScope: ['self'],
      });
    } else if (self.GLOBAL.isWindow()) {
      const registration = await service_worker_unregister_and_register(
          t, 'resources/empty_sw.js', 'resources/does/not/exist');
      t.add_cleanup(() => registration.unregister());

      // Global property referenced by idl_array.add_objects().
      self.registration = registration;

      idl_array.add_objects({
        CookieChangeEvent: ['new CookieChangeEvent("change")'],
        Window: ['self'],
      });
    }

    if (isServiceWorker || self.GLOBAL.isWindow()) {
      idl_array.add_objects({
        CookieStore: ['self.cookieStore'],
        CookieStoreManager: ['self.registration.cookies'],
        ServiceWorkerRegistration: ['self.registration'],
      });
    }
  }
);
