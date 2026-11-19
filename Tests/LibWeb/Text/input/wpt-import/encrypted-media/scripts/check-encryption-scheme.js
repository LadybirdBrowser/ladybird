function runTest(config, qualifier)
{
  function checkEncryptionScheme(encryptionScheme)
  {
    var simpleConfig = getSimpleConfiguration();
    assert_greater_than(simpleConfig[0].audioCapabilities.length, 0);
    simpleConfig[0].audioCapabilities.forEach(function(capability) {
      capability.encryptionScheme = encryptionScheme;
    });

    return navigator.requestMediaKeySystemAccess(config.keysystem, simpleConfig)
        .then(
            function(access) {
              var actualConfiguration = access.getConfiguration();
              for (let i = 0; i < actualConfiguration.audioCapabilities.length; i++) {
                const capability = actualConfiguration.audioCapabilities[i];

                // If "encryptionScheme" is not supported, fail.
                if (!('encryptionScheme' in capability)) {
                  return Promise.reject('Not implemented');
                }

                // If "encryptionScheme" is supported, it should be returned.
                assert_equals(capability.encryptionScheme, encryptionScheme);
              }
              return Promise.resolve('Supported');
            },
            function error() {
              // CDM does not support "encryptionScheme". Test should still pass.
              return Promise.resolve('Not supported');
            });
  }

  promise_test(
      () => checkEncryptionScheme('cenc'),
      testnamePrefix(qualifier, config.keysystem) + ' support for "cenc" encryption scheme.');

  promise_test(
      () => checkEncryptionScheme('cbcs'),
      testnamePrefix(qualifier, config.keysystem) + ' support for "cbcs" encryption scheme.');

  promise_test(
      () => checkEncryptionScheme('cbcs-1-9'),
      testnamePrefix(qualifier, config.keysystem) +
          ' support for "cbcs-1-9" encryption scheme.');
}
