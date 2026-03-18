#!/usr/bin/env bash
set -euo pipefail

if [[ -z "${IDF_PATH:-}" ]]; then
    echo "IDF_PATH is not set. Source the ESP-IDF environment first." >&2
    exit 1
fi

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UNIT_APP_DIR="${IDF_PATH}/tools/unit-test-app"
TEST_SDKCONFIG_DEFAULTS="${ROOT_DIR}/components/embed_claw/test/sdkconfig.defaults"
TEST_PARTITION_TABLE="${ROOT_DIR}/components/embed_claw/test/partitions.csv"
UNIT_APP_SDKCONFIG_DEFAULTS="${UNIT_APP_DIR}/sdkconfig.defaults"
TARGET="${IDF_TARGET:-}"
IDF_ARGS=()

if [[ -z "${TARGET}" && -f "${ROOT_DIR}/sdkconfig" ]]; then
    TARGET="$(sed -n 's/^CONFIG_IDF_TARGET="\([^"]*\)"$/\1/p' "${ROOT_DIR}/sdkconfig" | head -n 1)"
fi

if [[ -n "${TARGET}" ]]; then
    IDF_ARGS+=("-DIDF_TARGET=${TARGET}")
fi

BUILD_DIR="${ROOT_DIR}/build/unit-test-app${TARGET:+-${TARGET}}"
SDKCONFIG_PATH="${BUILD_DIR}/sdkconfig"
DEPENDENCIES_LOCK_PATH="${BUILD_DIR}/dependencies.lock"
DEFAULTS_STAMP_PATH="${BUILD_DIR}/.sdkconfig_defaults"
GENERATED_DEFAULTS_PATH="${BUILD_DIR}/embed_claw_unit_test.defaults"
SDKCONFIG_DEFAULTS_LIST=("${UNIT_APP_SDKCONFIG_DEFAULTS}")
ROOT_TARGET_DEFAULTS="${ROOT_DIR}/sdkconfig.defaults${TARGET:+.${TARGET}}"

mkdir -p "${BUILD_DIR}"

if [[ -f "${ROOT_TARGET_DEFAULTS}" ]]; then
    SDKCONFIG_DEFAULTS_LIST+=("${ROOT_TARGET_DEFAULTS}")
fi

cat > "${GENERATED_DEFAULTS_PATH}" <<EOF
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_FILENAME="${TEST_PARTITION_TABLE}"
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="${TEST_PARTITION_TABLE}"
EOF

SDKCONFIG_DEFAULTS_LIST+=("${GENERATED_DEFAULTS_PATH}")
SDKCONFIG_DEFAULTS_LIST+=("${TEST_SDKCONFIG_DEFAULTS}")
SDKCONFIG_DEFAULTS_VALUE="$(IFS=';'; echo "${SDKCONFIG_DEFAULTS_LIST[*]}")"

if [[ -f "${SDKCONFIG_PATH}" && ! -f "${DEFAULTS_STAMP_PATH}" ]]; then
    rm -f "${SDKCONFIG_PATH}"
fi

if [[ -f "${SDKCONFIG_PATH}" && -f "${DEFAULTS_STAMP_PATH}" && "$(cat "${DEFAULTS_STAMP_PATH}")" != "${SDKCONFIG_DEFAULTS_VALUE}" ]]; then
    rm -f "${SDKCONFIG_PATH}"
fi

if [[ -f "${SDKCONFIG_PATH}" && "${TEST_SDKCONFIG_DEFAULTS}" -nt "${SDKCONFIG_PATH}" ]]; then
    rm -f "${SDKCONFIG_PATH}"
fi

for defaults_file in "${SDKCONFIG_DEFAULTS_LIST[@]}"; do
    if [[ -f "${SDKCONFIG_PATH}" && "${defaults_file}" -nt "${SDKCONFIG_PATH}" ]]; then
        rm -f "${SDKCONFIG_PATH}"
    fi
done

printf '%s' "${SDKCONFIG_DEFAULTS_VALUE}" > "${DEFAULTS_STAMP_PATH}"

cd "${UNIT_APP_DIR}"
idf.py \
    -B "${BUILD_DIR}" \
    -DSDKCONFIG="${SDKCONFIG_PATH}" \
    -DDEPENDENCIES_LOCK="${DEPENDENCIES_LOCK_PATH}" \
    "${IDF_ARGS[@]}" \
    -DEXTRA_COMPONENT_DIRS="${ROOT_DIR}/components" \
    -DSDKCONFIG_DEFAULTS="${SDKCONFIG_DEFAULTS_VALUE}" \
    -DTESTS_ALL=0 \
    -DTEST_COMPONENTS=embed_claw \
    "$@"
