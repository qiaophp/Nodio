# deterministic local HTTP server for the web-library tests:
# GET /data.txt -> "hello-web"; POST anywhere -> "echo:" + body
from http.server import BaseHTTPRequestHandler, HTTPServer


class H(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == '/data.txt':
            body = b'hello-web'
        else:
            body = b'not-found'
        self.send_response(200)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        n = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(n) if n else b''
        out = b'echo:' + body
        self.send_response(200)
        self.send_header('Content-Length', str(len(out)))
        self.end_headers()
        self.wfile.write(out)

    def log_message(self, *a):
        pass


HTTPServer(('127.0.0.1', 8123), H).serve_forever()
