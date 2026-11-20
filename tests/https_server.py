"""HTTPS test server for FluxDM's tests: https_server.py <cert.pem> <key.pem> <body-size>.
Prints "PORT <n>" once listening, then serves the same bytes for any GET."""
import http.server
import ssl
import sys

cert, key, size = sys.argv[1], sys.argv[2], int(sys.argv[3])
BODY = bytes((i * 7) % 251 for i in range(size))


class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(BODY)))
        self.end_headers()
        self.wfile.write(BODY)

    def log_message(self, *a):
        pass


srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(cert, key)
srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
print("PORT", srv.server_address[1], flush=True)
srv.serve_forever()
