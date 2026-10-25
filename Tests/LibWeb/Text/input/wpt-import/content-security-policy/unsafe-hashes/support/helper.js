// Typical CSP hashes are:
// 'sha256-N5bidCKdNO1nSPa1G7MdL6S7Y7MKZ7UMIS/40JBMSe4=' ==> javascript:opener.navigated();
// 'sha256-l0Wxf12cHMZT6UQ2zsQ7AcFSb6Y198d37Ki8zWITecM=' ==> javascript:navigated();

function runTest(navigationShouldAllowed, navigationMethod, description) {
  const t1 = async_test(
    'javascript: navigation using ' + navigationMethod + ' should be ' +
    (navigationShouldAllowed ? 'allowed' : 'refused') + description);

  if (navigationShouldAllowed) {
    window.navigated = () => t1.done();
    window.addEventListener('securitypolicyviolation',
        t1.unreached_func('Should have not raised any event'));
  } else {
    window.navigated =
        t1.unreached_func('Should not have run javascript: URL');
    window.addEventListener('securitypolicyviolation',
        t1.step_func_done(function(e) {
            assert_equals(e.violatedDirective, 'script-src-elem');
            assert_equals(e.blockedURI, 'inline');
        }));
  }

  if (navigationMethod === '<a href target=_blank>') {
    const a = document.createElement('a');
    a.setAttribute('target', '_blank');
    a.setAttribute('rel', 'opener');
    a.setAttribute('href', 'javascript:opener.navigated();');
    document.body.appendChild(a);
    a.click();
  }
  else if (navigationMethod === '<a href>') {
    const a = document.createElement('a');
    a.setAttribute('href', 'javascript:navigated();');
    document.body.appendChild(a);
    a.click();
  } else {
    t1.unreached_func('Invalid navigationMethod: ' + navigationMethod)();
  }
}
