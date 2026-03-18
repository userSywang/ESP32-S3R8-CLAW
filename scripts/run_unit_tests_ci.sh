#!/usr/bin/env bash
set -euo pipefail

if [[ -z "${IDF_PATH:-}" ]]; then
    echo "IDF_PATH is not set. Source the ESP-IDF environment first." >&2
    exit 1
fi

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${ESPPORT:-${PORT:-}}"
BAUD="${ESPBAUD:-115200}"
RESULT_DIR="${UNITY_RESULTS_DIR:-${ROOT_DIR}/build/unit-test-results}"
UNITY_SELECTORS="${UNITY_SELECTORS:-[embed_claw]}"

if [[ -z "${PORT}" ]]; then
    echo "ESPPORT is not set. Example: ESPPORT=/dev/ttyUSB0 ./scripts/run_unit_tests_ci.sh" >&2
    exit 1
fi

SELECTOR_ARGS=()
for selector in ${UNITY_SELECTORS}; do
    SELECTOR_ARGS+=(--selector "${selector}")
done

"${ROOT_DIR}/scripts/run_unit_tests.sh" build
"${ROOT_DIR}/scripts/run_unit_tests.sh" -p "${PORT}" flash

python3 "${ROOT_DIR}/scripts/collect_unity_results.py" \
    --port "${PORT}" \
    --baud "${BAUD}" \
    --output-dir "${RESULT_DIR}" \
    "${SELECTOR_ARGS[@]}" \
    "$@"
