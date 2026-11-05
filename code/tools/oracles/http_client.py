#!/usr/bin/env python3
"""
VPT1 Test Oracle: HTTP Client (TEST_PLAN.md §4.1)

An ordinary HTTP client that sends a fresh unpredictable request ID and a no-cache request,
validates the matching response ID and server identity,
and asserts the socket-observed source address matches expectations.
"""

import argparse
import json
import sys
import time
import urllib.error
import urllib.request
import uuid


def run_client(target_url, expected_server_id=None, expected_client_ip=None, timeout=3.0):
    request_id = str(uuid.uuid4())
    req = urllib.request.Request(
        target_url,
        headers={
            "X-Request-ID": request_id,
            "Cache-Control": "no-cache, no-store",
            "User-Agent": "VPT1-Oracle-Client/1.0",
        },
    )

    start_time = time.time()
    result = {
        "target_url": target_url,
        "sent_request_id": request_id,
        "start_time": start_time,
        "success": False,
        "duration_ms": 0.0,
        "error": None,
        "response": None,
    }

    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = resp.read().decode("utf-8")
            duration_ms = (time.time() - start_time) * 1000.0
            result["duration_ms"] = round(duration_ms, 2)
            result["http_status"] = resp.status

            if resp.status != 200:
                result["error"] = f"Unexpected HTTP status: {resp.status}"
                return result

            data = json.loads(body)
            result["response"] = data

            # Assertion 1: request_id match
            if data.get("request_id") != request_id:
                result["error"] = f"Request ID mismatch: sent {request_id}, got {data.get('request_id')}"
                return result

            # Assertion 2: expected server identity
            if expected_server_id and data.get("server_id") != expected_server_id:
                result["error"] = f"Server ID mismatch: expected {expected_server_id}, got {data.get('server_id')}"
                return result

            # Assertion 3: original client IP observed at server (proves no NAT/proxy translation)
            if expected_client_ip and data.get("observed_client_ip") != expected_client_ip:
                result["error"] = f"Source IP mismatch: expected {expected_client_ip}, got {data.get('observed_client_ip')}"
                return result

            result["success"] = True
            return result

    except urllib.error.HTTPError as e:
        result["duration_ms"] = round((time.time() - start_time) * 1000.0, 2)
        result["http_status"] = e.code
        result["error"] = f"HTTP Error: {e.code} {e.reason}"
        return result
    except urllib.error.URLError as e:
        result["duration_ms"] = round((time.time() - start_time) * 1000.0, 2)
        result["error"] = f"Connection Failed: {e.reason}"
        return result
    except Exception as e:
        result["duration_ms"] = round((time.time() - start_time) * 1000.0, 2)
        result["error"] = f"Unexpected Exception: {str(e)}"
        return result


def main():
    parser = argparse.ArgumentParser(description="VPT1 Independent HTTP Client Oracle")
    parser.add_argument("url", help="Target URL (e.g. http://10.77.2.10:8080)")
    parser.add_argument("--expect-server", help="Expected server identity (e.g. host-b)")
    parser.add_argument("--expect-client-ip", help="Expected client IP observed by server (e.g. 10.77.1.10)")
    parser.add_argument("--timeout", type=float, default=3.0, help="Connection timeout in seconds (default: 3.0)")
    args = parser.parse_args()

    outcome = run_client(
        args.url,
        expected_server_id=args.expect_server,
        expected_client_ip=args.expect_client_ip,
        timeout=args.timeout,
    )

    sys.stdout.write(json.dumps(outcome, indent=2) + "\n")
    sys.exit(0 if outcome["success"] else 1)


if __name__ == "__main__":
    main()