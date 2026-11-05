#!/usr/bin/env python3
"""
VPT1 Test Oracle: HTTP Service (TEST_PLAN.md §4.1)

An ordinary HTTP service that logs socket-observed source address/port
and request ID, returning a deterministic JSON payload with the echoed ID.
Does not speak VPT1 or know session keys.
"""

import argparse
import json
import logging
import sys
import time
from http.server import HTTPServer, BaseHTTPRequestHandler

class OracleHTTPServer(HTTPServer):
    """Subclass of HTTPServer that explicitly carries server_id for type safety."""
    def __init__(self, server_address, RequestHandlerClass, server_id="host-b", bind_and_activate=True):
        self.server_id = server_id
        super().__init__(server_address, RequestHandlerClass, bind_and_activate)


class OracleHTTPHandler(BaseHTTPRequestHandler):
    server_version = "VPT1-Oracle/1.0"

    def do_GET(self):
        self._handle_request()

    def do_POST(self):
        self._handle_request()

    def _handle_request(self):
        request_id = self.headers.get("X-Request-ID")
        if not request_id:
            # Fall back to URL path if header not set: /<request_id>
            request_id = self.path.lstrip("/") or "unknown"

        client_ip, client_port = self.client_address[:2]
        now = time.time()
        server_id = getattr(self.server, "server_id", "unknown-server")

        payload = {
            "status": "ok",
            "server_id": server_id,
            "request_id": request_id,
            "observed_client_ip": client_ip,
            "observed_client_port": client_port,
            "timestamp": now,
        }

        body = (json.dumps(payload, indent=2) + "\n").encode("utf-8")

        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate")
        self.send_header("X-Request-ID", request_id)
        self.end_headers()
        self.wfile.write(body)

        # Log event in machine-readable JSON to stdout
        log_entry = {
            "event": "http_request",
            "server_id": server_id,
            "client_ip": client_ip,
            "client_port": client_port,
            "request_id": request_id,
            "method": self.command,
            "path": self.path,
            "timestamp": now,
        }
        sys.stdout.write(json.dumps(log_entry) + "\n")
        sys.stdout.flush()

    def log_message(self, format, *args):
        # Suppress default BaseHTTPRequestHandler stderr logging
        pass


def main():
    parser = argparse.ArgumentParser(description="VPT1 Independent HTTP Service Oracle")
    parser.add_argument("--host", default="0.0.0.0", help="Listen address (default: 0.0.0.0)")
    parser.add_argument("--port", type=int, default=8080, help="Listen port (default: 8080)")
    parser.add_argument("--server-id", default="host-b", help="Host identity (default: host-b)")
    args = parser.parse_args()

    server = OracleHTTPServer((args.host, args.port), OracleHTTPHandler, server_id=args.server_id)

    sys.stdout.write(json.dumps({
        "event": "server_start",
        "server_id": args.server_id,
        "listen_host": args.host,
        "listen_port": args.port,
        "timestamp": time.time(),
    }) + "\n")
    sys.stdout.flush()

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()