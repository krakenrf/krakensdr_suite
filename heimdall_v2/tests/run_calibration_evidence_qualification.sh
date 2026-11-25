#!/usr/bin/env bash
# Source-only calibration coverage qualification; no SDR, service or USB.
set -euo pipefail
cd "$(dirname "$0")/.."
PYTHONDONTWRITEBYTECODE=1 python3 tests/calibration_evidence_source_audit.py
bash tests/run_calibration_evidence_guard.sh
echo 'CALIBRATION_EVIDENCE_FULL_OFFLINE_PASS; NO_PI_HARDWARE_REOPEN_OR_TRANSPORT_CLAIM'
