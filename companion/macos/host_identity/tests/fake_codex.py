#!/usr/bin/env python3
"""Offline app-server fixture. Never contacts Codex or any hardware/network."""
import json
import os
import sys

if sys.argv[1:] != ["app-server", "--listen", "stdio://"]:
    sys.exit(2)

scenario = os.environ.get("CODEX_IDENTITY_TEST_SCENARIO", "normal")
for line in sys.stdin:
    request = json.loads(line)
    method = request.get("method")
    if method == "initialize":
        response = {"id": request["id"], "result": {}}
        if scenario == "initialize_error":
            response = {"id": request["id"], "error": {"message": "fixture rejection"}}
    elif method == "initialized":
        continue
    elif method == "account/rateLimits/read":
        if scenario == "closed_pipe":
            break
        response = {"id": request["id"], "result": {"rateLimitsByLimitId": {"codex": {
            "limitId": "codex",
            "primary": {"usedPercent": 25.5, "windowDurationMins": 300, "resetsAt": 1},
            "secondary": {"usedPercent": 80, "windowDurationMins": 10080, "resetsAt": 1},
        }}}}
        if scenario == "read_error":
            response = {"id": request["id"], "error": {"message": "fixture rejection"}}
    else:
        sys.exit("unexpected request: " + str(method))
    # Exercise notification skipping and line framing with separately flushed writes.
    sys.stdout.write('{"method":"fixture/ignored"}\n')
    encoded = json.dumps(response) + "\n"
    split = len(encoded) // 2
    sys.stdout.write(encoded[:split])
    sys.stdout.flush()
    sys.stdout.write(encoded[split:])
    sys.stdout.flush()
