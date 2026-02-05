# Build the image:
# $ podman build \
#       --tag fuzzillijs \
#       -f ./FuzzilliJs.dockerfile
# Run the container:
# $ podman run \
#       -it --rm \
#       -v ./path/to/fuzzilli-storage:/home/fuzzilli-storage:Z \
#       localhost/fuzzillijs
# To pass more options to fuzzilli, e.g. '--resume' (use '--help' to see all options):
# $ podman run \
#       -it --rm \
#       -v ./path/to/fuzzilli-storage:/home/fuzzilli-storage:Z \
#       -e FUZZILLI_CLI_OPTIONS='--resume' \
#       localhost/fuzzillijs
# Invocations with `docker` should be similar or even identical.
# NB: There are Dockerfiles & build scripts available for Fuzzilli-supported JS engines,
# but this doesn't use the same approach - that would require a fair amount of patching
# which is not worth it, unless we plan to add LibJS support to Fuzzilli upstream.

FROM fedora:39 AS serenity-build

WORKDIR /home
RUN dnf install -y clang cmake git-core ninja-build
RUN git clone --depth=1 https://github.com/LadybirdBrowser/ladybird

RUN cd ladybird/Meta/Lagom && ./BuildFuzzers.sh

FROM fedora:39 AS fuzzilli-build

WORKDIR /home
RUN dnf install -y git-core patch swift-lang
RUN git clone --depth=1 https://github.com/googleprojectzero/fuzzilli

WORKDIR /home/fuzzilli
RUN swift build -c release


FROM fedora:39

WORKDIR /home
# This is unfortunate, but we need libswiftCore.so (and possibly other files) from the
# Swift runtime. The "swift-lang-runtime" package doesn't seem to exist in Fedora :/
RUN dnf install -y swift-lang procps-ng
COPY --from=serenity-build /home/serenity/Meta/Lagom/Build/lagom-fuzzers/bin ./bin
COPY --from=serenity-build /home/serenity/Meta/Lagom/Build/lagom-fuzzers/lib64 ./lib64
COPY --from=fuzzilli-build /home/fuzzilli/.build/x86_64-unknown-linux-gnu/release/FuzzilliCli .
RUN mkdir fuzzilli-storage
ENV FUZZILLI_CLI_OPTIONS ""
CMD [ "sh", "-c", "./FuzzilliCli --profile=serenity --storagePath=fuzzilli-storage ${FUZZILLI_CLI_OPTIONS} ./bin/FuzzilliJs" ]
