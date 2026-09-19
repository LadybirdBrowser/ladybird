# How to use FuzzilliJs

1. Download a copy of the Fuzzilli repo from https://github.com/googleprojectzero/fuzzilli
2. Install Swift and make sure it's in your path environment variable.
3. Build FuzzilliJs with ```Meta/Fuzzers/BuildFuzzers.sh --fuzzilli```.
4. Build Fuzzilli with ```swift build -c release```.
5. Run Fuzzilli with ```swift run -c release FuzzilliCli --profile=serenity /path/to/FuzzilliJs```. See ```swift run FuzzilliCli --help``` for options.

Alternatively you can use `FuzzilliJs.dockerfile` to build & run Fuzzilli and FuzzilliJs with Docker or Podman.
