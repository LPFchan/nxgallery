#!/usr/bin/env python3
import argparse
import http.server
import pathlib
import ssl


class ConfigHandler(http.server.BaseHTTPRequestHandler):
    config_bytes = b""

    def do_GET(self):
        if self.path != "/config":
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(self.config_bytes)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(self.config_bytes)

    def log_message(self, _format, *_args):
        return


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", required=True)
    parser.add_argument("--cert", required=True)
    parser.add_argument("--key", required=True)
    parser.add_argument("--port", required=True, type=int)
    args = parser.parse_args()

    config_bytes = pathlib.Path(args.config).read_bytes()
    if not config_bytes or len(config_bytes) > 32 * 1024:
        raise SystemExit("probe configuration must be between 1 byte and 32 KiB")
    ConfigHandler.config_bytes = config_bytes
    server = http.server.HTTPServer(("0.0.0.0", args.port), ConfigHandler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(args.cert, args.key)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    server.handle_request()


if __name__ == "__main__":
    main()
