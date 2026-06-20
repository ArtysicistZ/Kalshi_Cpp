#!/usr/bin/env bash
# Usage: ./script/run.sh <binary> [args...]   e.g. ./script/run.sh kalshi-sim
set -euo pipefail
cd "$(dirname "$0")/.."
[ -f .env ] && { set -a; source .env; set +a; }
exec "./build/$@"
