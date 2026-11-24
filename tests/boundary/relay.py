# Between a client and the host, keeping every byte either side sends: <listen port> <host port> <file>. Serves one
# connection after another until killed, appending them all to the file.
import socket, sys, threading

lp, hp, out = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", lp))
s.listen(4)
f = open(out, "ab")
lock = threading.Lock()

def pipe(a, b):
    while True:
        d = a.recv(65536)
        if not d:
            break
        with lock:
            f.write(d)
            f.flush()
        b.sendall(d)
    try:
        b.shutdown(socket.SHUT_WR)
    except OSError:
        pass

while True:
    c, _ = s.accept()
    h = socket.create_connection(("127.0.0.1", hp))
    threading.Thread(target=pipe, args=(c, h), daemon=True).start()
    threading.Thread(target=pipe, args=(h, c), daemon=True).start()
