#!/usr/bin/env bash

set -eo pipefail

SCRIPT_DIR="$(realpath "$(dirname "${BASH_SOURCE[0]}")")"

if [ -z "$LADYBIRD_SOURCE_DIR" ]
then
    LADYBIRD_SOURCE_DIR="$(realpath "${SCRIPT_DIR}/../../../")"
    export LADYBIRD_SOURCE_DIR
fi


: "${WEBDRIVER_BINARY:=$(env PATH="${LADYBIRD_SOURCE_DIR}/Build/ladybird/bin/Ladybird.app/Contents/MacOS:${LADYBIRD_SOURCE_DIR}/Build/ladybird/bin:${LADYBIRD_SOURCE_DIR}/Build/bin:${PATH}" \
                         which WebDriver)}"
update_expectations_metadata=false
remove_wpt_repository=false
# Generate name for file with wpt run log
wpt_run_log_filename="$(mktemp)"

for arg in "$@"; do
  case $arg in
    --webdriver-binary=*)
        WEBDRIVER_BINARY="$(realpath "${arg#*=}")"
        shift
        ;;
    --update-expectations-metadata)
        update_expectations_metadata=true
        shift
        ;;
    --remove-wpt-repository)
        remove_wpt_repository=true
        shift
        ;;
    --log-file=*)
        wpt_run_log_filename="$(realpath "${arg#*=}")"
        shift
        ;;
    *)
        echo "Unknown argument ${arg}"
        exit 1
        ;;
    esac
done

if [ -z "$WEBDRIVER_BINARY" ]; then
    echo "Unable to find WebDriver binary, did you build Ladybird?"
    exit 1
fi

pushd "${SCRIPT_DIR}"

if [ ! -d "${SCRIPT_DIR}/wpt" ]; then
    # Clone patched web-platform-tests repository
    mkdir wpt
    git -C wpt init
    git -C wpt remote add origin https://github.com/web-platform-tests/wpt.git

    # Switch to the commit that was used to generate tests expectations. Requires periodic updates.
    git -C wpt fetch --depth 1 origin 5930e386a5e1e59456dc810c9b21adf18bc1b6fe
    git -C wpt checkout FETCH_HEAD

    git apply 0001-tools-Pass-product-name-to-update-metadata-fallback-.patch

    # Update hosts file if needed
    if [ "$(comm -13 <(sort -u /etc/hosts) <(python3 ./wpt/wpt make-hosts-file | sort -u) | wc -l)" -gt 0 ]; then
        echo "Enter superuser password to append wpt hosts to /etc/hosts"
        python3 "./wpt/wpt" make-hosts-file | sudo tee -a /etc/hosts
    fi
fi

# Extract metadata.txt into directory with expectation files expected by WPT runner
python3 ./concat-extract-metadata.py --extract metadata.txt metadata

# Run tests.
python3 ./wpt/wpt run ladybird \
                  --webdriver-binary "${WEBDRIVER_BINARY}" \
                  --no-fail-on-unexpected \
                  --no-fail-on-unexpected-pass \
                  --skip-timeout \
                  --include-manifest include.ini \
                  --metadata ./metadata \
                  --manifest ./MANIFEST.json \
                  --webdriver-arg="--certificate=${PWD}/wpt/tools/certs/cacert.pem" \
                  --webdriver-arg="--certificate=${LADYBIRD_SOURCE_DIR}/Build/lagom/cacert.pem" \
                  --webdriver-arg="--enable-qt-networking" \
                  --log-raw "${wpt_run_log_filename}"

# Update expectations metadata files if requested
if [[ $update_expectations_metadata == true ]]; then
    python3 ./wpt/wpt update-expectations --product ladybird --metadata ./metadata --manifest ./MANIFEST.json "${wpt_run_log_filename}"
    python3 ./concat-extract-metadata.py --concat ./metadata > metadata.txt
fi

if [[ $remove_wpt_repository == true ]]; then
    rm -rf wpt
fi

popd
