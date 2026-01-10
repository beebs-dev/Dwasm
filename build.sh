#!/bin/bash
set -euo pipefail
docker build --push -t thavlik/dwasm:latest .
kubectl rollout restart deployment -n apps apps-prboom
k9s -n apps -c pods --headless --splashless