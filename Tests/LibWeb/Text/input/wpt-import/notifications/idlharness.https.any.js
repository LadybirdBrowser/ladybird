// META: global=window,worker
// META: script=/resources/WebIDLParser.js
// META: script=/resources/idlharness.js

'use strict';

// https://notifications.spec.whatwg.org/

idl_test(
  ['notifications'],
  ['service-workers', 'hr-time', 'html', 'dom'],
  idl_array => {
    if (self.ServiceWorkerGlobalScope) {
      idl_array.add_objects({
        ServiceWorkerGlobalScope: ['self'],
      });
      // NotificationEvent could be tested here, but the constructor requires
      // a Notification instance which cannot be created in a service worker,
      // see below.
    } else {
      // While the Notification interface is exposed in service workers, the
      // constructor (https://notifications.spec.whatwg.org/#dom-notification-notification)
      // is defined to throw a TypeError. Therefore, we only add the object in
      // the other scopes.
      idl_array.add_objects({
        Notification: ['notification'],
      });
      self.notification = new Notification('title');
      self.notification.close();
    }
  }
);
