#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PY_SCRIPT="$SCRIPT_DIR/build_and_deploy.py"

CYAN='\033[0;36m'
YELLOW='\033[1;33m'
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

printf "%b\n" "${CYAN}===========================================${NC}"
printf "%b\n" "${CYAN}  OBS Mouse Trail Build & Deploy Launcher ${NC}"
printf "%b\n" "${CYAN}===========================================${NC}"

if command -v python3 >/dev/null 2>&1; then
  printf "%b\n" "${YELLOW}[INFO] Using python3${NC}"
  python3 "$PY_SCRIPT" "$@"
  printf "%b\n" "${GREEN}[SUCCESS] Build and deploy finished.${NC}"
  exit 0
elif command -v python >/dev/null 2>&1; then
  printf "%b\n" "${YELLOW}[INFO] Using python${NC}"
  python "$PY_SCRIPT" "$@"
  printf "%b\n" "${GREEN}[SUCCESS] Build and deploy finished.${NC}"
  exit 0
else
  printf "%b\n" "${RED}[ERROR] Python 3 was not found. Install Python 3 and retry.${NC}" >&2
  exit 1
fi
