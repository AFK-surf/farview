# A site with a login, for tools/boundary.sh: <port> <secret>. Signing in sets an HttpOnly cookie holding the secret;
# the front page says who is signed in only when that cookie comes back.
import http.server, sys

port, secret = int(sys.argv[1]), sys.argv[2]

class Site(http.server.BaseHTTPRequestHandler):
    def page(self, body, cookie=None):
        b = ("<!doctype html><title>boundary</title><body>" + body + "</body>").encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(b)))
        if cookie:
            self.send_header("Set-Cookie", cookie)
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        if ("session=" + secret) in self.headers.get("Cookie", ""):
            self.page('<p id="who">signed in as tester</p>')
        else:
            self.page('<form method="post" action="/login"><input name="user"><input type="submit" value="sign in"></form>')

    def do_POST(self):
        self.rfile.read(int(self.headers.get("Content-Length", "0")))
        self.send_response(303)
        self.send_header("Location", "/")
        self.send_header("Set-Cookie", "session=" + secret + "; Path=/; HttpOnly")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def log_message(self, *a):
        pass

http.server.ThreadingHTTPServer(("127.0.0.1", port), Site).serve_forever()
