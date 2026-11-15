/**
 * Promise based helper function who's return promise will resolve
 * once the iframe src has been loaded
 * @param {string} url the url to set the iframe src
 * @param {test} t a test object to add a cleanup function to
 * @return {Promise} when resolved, will return the iframe
 */
self.createIframe = (url, t) => new Promise(resolve => {
  const iframe = document.createElement('iframe');
  iframe.addEventListener('load', () => {resolve(iframe);}, {once: true});
  iframe.src = url;
  document.documentElement.appendChild(iframe);
  t.add_cleanup(() => iframe.remove());
});

/**
 * @description - Function unregisters any service workers in this scope
 *                and then creates a new registration. The function returns
 *                a promise that resolves when the registered service worker
 *                becomes activated. The resolved promise yields the
 *                service worker registration
 * @param {testCase} t - test case to add cleanup functions to
 */
self.createServiceWorker = async (t, sw_registration_name, scope_url) => {
  let registration = await navigator.serviceWorker.getRegistration(scope_url);
  if (registration)
    await registration.unregister();

  registration = await navigator.serviceWorker.register(sw_registration_name,
      {scope_url});
  t.add_cleanup(() => registration.unregister());

  return new Promise(resolve => {
    const serviceWorker = registration.installing || registration.active ||
        registration.waiting;
    serviceWorker.addEventListener('statechange', event => {
      if (event.target.state === 'activated') {
        resolve(serviceWorker);
      }
    });
  })
}

/**
 * Function that will return a promise that resolves when a message event
 * is fired. Returns a promise that resolves to the message that was received
 */
self.waitForMessage = () => new Promise(resolve => {
  window.addEventListener('message', event => {
    resolve(event.data);
  }, {once: true});
});

/**
 * Sends a message via MessageChannel and waits for the response
 * @param {*} message
 * @returns {Promise} resolves with the response payload
 */
self.sendMessageOverChannel = (message, target) => {
  return new Promise(function(resolve, reject) {
    const messageChannel = new MessageChannel();
    messageChannel.port1.onmessage = event => {
      if (event.data.error) {
        reject(event.data.error);
      } else {
        resolve(event.data);
      }
    };

    target.postMessage(message, [messageChannel.port2]);
  })
};
