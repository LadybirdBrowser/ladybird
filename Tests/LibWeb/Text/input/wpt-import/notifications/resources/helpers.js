function unregisterAllServiceWorker() {
  return navigator.serviceWorker.getRegistrations().then(registrations => {
    return Promise.all(registrations.map(r => r.unregister()));
  });
}

async function prepareActiveServiceWorker(script, options) {
  await unregisterAllServiceWorker();
  const reg = await navigator.serviceWorker.register(script, options);
  add_completion_callback(() => reg.unregister());
  await navigator.serviceWorker.ready;
  return reg;
}

async function prepareActiveServiceWorkerForTest(t, script, options) {
  const reg = await prepareActiveServiceWorker(script, options);
  t.add_cleanup(() => reg.unregister());
  return reg;
}

async function closeAllNotifications() {
  for (const n of await registration.getNotifications()) {
    n.close();
  }
}

async function trySettingPermission(perm) {
  try {
    await test_driver.set_permission({ name: "notifications" }, perm);
  } catch {
    // Not all implementations support this yet, but the permission may already be set to be able to continue
  }

  // Using Notification.permission instead of permissions.query() as
  // some implementation without set_permission support overrides
  // Notification.permission.
  const permission = Notification.permission === "default" ? "prompt" : Notification.permission;
  if (permission !== perm) {
    throw new Error(`Should have the permission ${perm} to continue but found ${permission}`);
  }
}

// Use this in service workers where activation is required e.g. when testing showNotification()
async function untilActivate() {
  if (registration.active) {
    return;
  }
  return new Promise(resolve => {
    addEventListener("activate", resolve, { once: true });
  });
}
