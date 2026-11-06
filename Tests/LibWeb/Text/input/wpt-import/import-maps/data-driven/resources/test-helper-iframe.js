// Handle errors around fetching, parsing and registering import maps.
window.onScriptError = event => {
  window.registrationResult = {type: 'FetchError', error: event.error};
  return false;
};
window.windowErrorHandler = event => {
  window.registrationResult = {type: 'ParseError', error: event.error};
  return false;
};
window.addEventListener('error', window.windowErrorHandler);

// Handle specifier resolution requests from the parent frame.
// For failures, we post error names and messages instead of error
// objects themselves and re-create error objects later, to avoid
// issues around serializing error objects which is a quite new feature.
window.addEventListener('message', event => {
  if (event.data.action !== 'resolve') {
    parent.postMessage({
        type: 'Failure',
        result: 'Error',
        message: 'Invalid Action: ' + event.data.action}, '*');
    return;
  }

  // To respond to a resolution request, we:
  // 1. Save the specifier to resolve into a global.
  // 2. Update the document's base URL to the requested base URL.
  // 3. Create a new inline script, parsed with that base URL, which
  //    resolves the saved specifier using import.meta.resolve(), and
  //    sents the result to the parent window.
  window.specifierToResolve = event.data.specifier;
  document.querySelector('base').href = event.data.baseURL;

  const inlineScript = document.createElement('script');
  inlineScript.type = 'module';
  inlineScript.textContent = `
    try {
      const result = import.meta.resolve(window.specifierToResolve);
      parent.postMessage({type: 'ResolutionSuccess', result}, '*');
    } catch (e) {
      parent.postMessage(
          {type: 'Failure', result: e.name, message: e.message}, '*');
    }
  `;
  document.body.append(inlineScript);
});
