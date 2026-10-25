        var test = async_test("XMLHttpRequest: The abort() method: Fire a progress event named loadend on the XMLHttpRequestUpload object");

        test.step(function()
        {
            var xhr = new XMLHttpRequest();

            xhr.onloadstart = function()
            {
                test.step(function ()
                {
                    if (xhr.readyState == 1)
                    {
                        xhr.abort();
                    }
                });
            };

            xhr.upload.onloadend = function(e)
            {
                test.step(function()
                {
                    assert_true(e instanceof ProgressEvent);
                    assert_equals(e.type, "loadend");
                    assert_equals(e.target, xhr.upload);
                    test.done();
                });
            };

            xhr.open("POST", "./resources/content.py", true);
            xhr.send("Test Message");
        });
