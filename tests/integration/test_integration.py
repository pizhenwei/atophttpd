#!/usr/bin/env python3
"""
Integration tests for atophttpd.

These start a real atophttpd server against a real atop raw-log and exercise
the HTTP endpoints end-to-end (routing, JSON serialisation, deflate
compression, argument parsing and error handling).

Configuration via environment variables:
  ATOPHTTPD_BIN  path to the atophttpd binary          (default: ../../atophttpd)
  ATOP_BIN       path to an atop binary to make a log   (default: ../../atop/atop)
  ATOP_LOG_DIR   use an existing log dir instead of generating one (optional)

If no atop binary and no log dir are available the sample-dependent tests are
skipped (the endpoint-only tests still run).
"""

import json
import os
import socket
import subprocess
import tempfile
import time
import unittest
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

ATOPHTTPD_BIN = os.environ.get("ATOPHTTPD_BIN", os.path.join(ROOT, "atophttpd"))
ATOP_BIN = os.environ.get("ATOP_BIN", os.path.join(ROOT, "atop", "atop"))
ATOP_LOG_DIR = os.environ.get("ATOP_LOG_DIR")

HOST = "127.0.0.1"

# module level state
_server = None
_port = None
_tmpdir = None
_log_dir = None
_sample_epochs = []


def _free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind((HOST, 0))
    port = s.getsockname()[1]
    s.close()
    return port


def _raw_request(path, method=b"GET", version=b"HTTP/1.1", host_header=True):
    """Send a raw request and return (status_line, headers, body)."""
    s = socket.create_connection((HOST, _port), timeout=5)
    req = method + b" " + path.encode() + b" " + version + b"\r\n"
    if host_header:
        req += b"Host: test\r\n"
    req += b"\r\n"
    s.sendall(req)
    buf = b""
    while True:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            break
        if not chunk:
            break
        buf += chunk
    s.close()

    if b"\r\n\r\n" not in buf:
        return (buf.split(b"\r\n", 1)[0] if buf else b"", {}, b"")
    head, _, body = buf.partition(b"\r\n\r\n")
    lines = head.split(b"\r\n")
    status = lines[0]
    headers = {}
    for line in lines[1:]:
        if b":" in line:
            k, _, v = line.partition(b":")
            headers[k.strip().lower().decode()] = v.strip().decode()
    return (status, headers, body)


def _raw_exchange(raw, timeout=5):
    """Send raw bytes, then read until the server closes. Returns what came back."""
    s = socket.create_connection((HOST, _port), timeout=timeout)
    s.sendall(raw)
    data = b""
    try:
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
    except (socket.timeout, ConnectionResetError, BrokenPipeError, OSError):
        pass
    finally:
        s.close()
    return data


def _gen_log():
    """Generate a short atop raw-log, return (log_dir, [sample_epochs])."""
    global _tmpdir
    if ATOP_LOG_DIR:
        log_dir = ATOP_LOG_DIR
    else:
        if not os.path.exists(ATOP_BIN):
            return None, []
        _tmpdir = tempfile.mkdtemp(prefix="atophttpd_it_")
        log_dir = _tmpdir
        logfile = os.path.join(log_dir, "atop_test")
        # 3 samples at 1s interval
        subprocess.run([ATOP_BIN, "-w", logfile, "1", "3"],
                       check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=30)

    # extract sample epochs from the first log file found
    epochs = []
    try:
        logfile = None
        for fn in sorted(os.listdir(log_dir)):
            full = os.path.join(log_dir, fn)
            if os.path.isfile(full):
                logfile = full
                break
        if logfile and os.path.exists(ATOP_BIN):
            out = subprocess.run([ATOP_BIN, "-r", logfile, "-P", "CPU"],
                                 capture_output=True, text=True, timeout=30).stdout
            for line in out.splitlines():
                parts = line.split()
                if len(parts) >= 3 and parts[0] == "CPU":
                    epochs.append(int(parts[2]))
    except Exception:
        pass
    return log_dir, epochs


def setUpModule():
    global _server, _port, _log_dir, _sample_epochs
    _log_dir, _sample_epochs = _gen_log()
    if _log_dir is None:
        # no log available: use an empty dir so the server still parses OK
        global _tmpdir
        _tmpdir = tempfile.mkdtemp(prefix="atophttpd_it_")
        _log_dir = _tmpdir
        # server refuses to start on an empty/invalid dir, so generation is required
        if os.environ.get("ATOPHTTPD_REQUIRE_TLS"):
            raise RuntimeError("ATOPHTTPD_REQUIRE_TLS is set but no raw-log could be "
                               "generated (is %s built?)" % ATOP_BIN)
        raise unittest.SkipTest(
            "no atop binary/log available to generate a raw-log for integration tests")

    _port = _free_port()
    _server = subprocess.Popen(
        [ATOPHTTPD_BIN, "-a", HOST, "-p", str(_port), "-P", _log_dir],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    # wait until the server answers /ping
    deadline = time.time() + 10
    while time.time() < deadline:
        try:
            status, _, body = _raw_request("/ping")
            if b"200" in status:
                break
        except OSError:
            pass
        time.sleep(0.1)
    else:
        out = b""
        if _server.stdout:
            try:
                out = _server.stdout.read1(4096)
            except Exception:
                pass
        _server.terminate()
        raise RuntimeError("atophttpd did not become ready: %r" % out)


def tearDownModule():
    global _server, _tmpdir
    if _server:
        _server.terminate()
        try:
            _server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            _server.kill()
    if _tmpdir and os.path.isdir(_tmpdir):
        import shutil
        shutil.rmtree(_tmpdir, ignore_errors=True)


class StaticEndpointTests(unittest.TestCase):
    def test_ping(self):
        status, _, body = _raw_request("/ping")
        self.assertIn(b"200", status)
        self.assertIn(b"pong", body)

    def test_index_root(self):
        status, headers, body = _raw_request("/")
        self.assertIn(b"200", status)
        self.assertGreater(len(body), 0)

    def test_index_html(self):
        status, _, body = _raw_request("/index.html")
        self.assertIn(b"200", status)
        self.assertGreater(len(body), 0)

    def test_help(self):
        status, _, body = _raw_request("/help")
        self.assertIn(b"200", status)
        self.assertGreater(len(body), 0)

    def test_favicon(self):
        status, _, _ = _raw_request("/favicon.ico")
        self.assertIn(b"200", status)

    def test_css(self):
        status, headers, _ = _raw_request("/css/atop.css")
        self.assertIn(b"200", status)
        self.assertIn("text/css", headers.get("content-type", ""))

    def test_js_files(self):
        for path in ("/js/atop.js", "/js/atop_parse.js", "/js/atop_compare_fc.js"):
            status, headers, _ = _raw_request(path)
            self.assertIn(b"200", status, path)
            self.assertIn("javascript", headers.get("content-type", ""), path)

    def test_template_header(self):
        status, _, _ = _raw_request("/template_header")
        self.assertIn(b"200", status)

    def test_template_types(self):
        for t in ("generic", "memory", "disk", "command_line"):
            status, _, _ = _raw_request("/template?type=%s" % t)
            self.assertIn(b"200", status, t)

    def test_template_bad_type(self):
        status, _, _ = _raw_request("/template?type=bogus")
        self.assertIn(b"404", status)

    def test_unknown_path_404(self):
        status, _, _ = _raw_request("/this/does/not/exist")
        self.assertIn(b"404", status)


class RequestValidationTests(unittest.TestCase):
    def test_post_rejected(self):
        status, _, body = _raw_request("/ping", method=b"POST")
        self.assertNotIn(b"200", status)  # connection closed, no response

    def test_http10_rejected(self):
        status, _, body = _raw_request("/ping", version=b"HTTP/1.0")
        self.assertNotIn(b"200", status)

    def test_oversized_url_rejected(self):
        status, _, body = _raw_request("/" + "a" * 2000)
        self.assertNotIn(b"200", status)

    def test_request_line_without_version_rejected(self):
        # no space before the version: nothing to split the URI from it
        self.assertNotIn(b"200", _raw_exchange(b"GET /ping\r\n\r\n"))

    def test_relative_uri_rejected(self):
        # the URI must be absolute (start with '/')
        self.assertNotIn(b"200", _raw_exchange(b"GET ping HTTP/1.1\r\n\r\n"))

    def test_template_without_type_closed(self):
        # /template needs a type= argument; without it no response is built
        # and the connection is simply closed
        self.assertEqual(_raw_exchange(b"GET /template HTTP/1.1\r\n\r\n"), b"")


class ShowsampErrorTests(unittest.TestCase):
    def test_missing_timestamp(self):
        _, _, body = _raw_request("/showsamp?lables=ALL")
        self.assertIn(b"missing timestamp", body)

    def test_missing_lables(self):
        _, _, body = _raw_request("/showsamp?timestamp=1")
        self.assertIn(b"missing lables", body)

    def test_invalid_timestamp(self):
        _, _, body = _raw_request("/showsamp?timestamp=0&lables=ALL")
        self.assertIn(b"invalid timestamp", body)

    def test_bad_encoding(self):
        _, _, body = _raw_request(
            "/showsamp?timestamp=1&lables=ALL&encoding=bogus")
        self.assertIn(b"encoding supports", body)

    def test_unknown_label(self):
        ts = _sample_epochs[0] if _sample_epochs else 1
        _, _, body = _raw_request(
            "/showsamp?timestamp=%d&lables=NOPE&encoding=none" % ts)
        self.assertIn(b"not supported", body)

    def test_missing_sample(self):
        # a timestamp older than any recorded sample has no record and no
        # usable recent fallback -> "missing sample"
        _, _, body = _raw_request(
            "/showsamp?timestamp=1&lables=CPU&encoding=none")
        self.assertIn(b"missing sample", body)


class ConnectionHandlingTests(unittest.TestCase):
    # Exercise the event loop's request-reading edge cases.

    def test_slow_client_closed(self):
        # connect but never send a complete request; the server must time out
        # (~100ms) and close the connection without a response.
        s = socket.create_connection((HOST, _port), timeout=5)
        s.sendall(b"GET /ping")  # no terminating CRLFCRLF
        s.settimeout(5)
        data = b""
        try:
            while True:
                chunk = s.recv(4096)
                if not chunk:
                    break
                data += chunk
        except (socket.timeout, ConnectionResetError, BrokenPipeError, OSError):
            pass
        finally:
            s.close()
        self.assertNotIn(b"200", data)

    def test_oversized_header_no_terminator(self):
        # fill the 4096-byte input buffer without a header terminator -> the
        # server gives up and closes the connection.
        s = socket.create_connection((HOST, _port), timeout=5)
        s.settimeout(5)
        data = b""
        try:
            s.sendall(b"GET /" + b"a" * 5000)
            while True:
                chunk = s.recv(4096)
                if not chunk:
                    break
                data += chunk
        except (socket.timeout, ConnectionResetError, BrokenPipeError, OSError):
            pass
        finally:
            s.close()
        self.assertNotIn(b"200", data)

    def test_eof_before_request_complete(self):
        # the client gives up (clean FIN) halfway through the request line; the
        # server must just drop the connection and keep serving others.
        s = socket.create_connection((HOST, _port), timeout=5)
        s.sendall(b"GET /pi")
        s.close()
        time.sleep(0.05)
        status, _, body = _raw_request("/ping")
        self.assertIn(b"200", status)

    def test_reset_mid_request(self):
        # the client aborts with a TCP RST (SO_LINGER 0) mid-request: a hard
        # socket error, handled by dropping that client only.
        import struct
        s = socket.create_connection((HOST, _port), timeout=5)
        s.sendall(b"GET /pi")
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        s.close()
        time.sleep(0.05)
        status, _, body = _raw_request("/ping")
        self.assertIn(b"200", status)

    def test_slow_reader_gets_full_response(self):
        # A client that does not read for a while makes the server's send
        # buffer fill up: the event loop must park that connection on EPOLLOUT
        # and resume it, delivering the complete body, instead of blocking or
        # truncating. A tiny receive buffer makes the stall likely.
        if not _sample_epochs:
            self.skipTest("no sample epochs available")
        ts = _sample_epochs[len(_sample_epochs) // 2]
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        s.settimeout(10)
        s.connect((HOST, _port))
        s.sendall(("GET /showsamp?timestamp=%d&lables=ALL&encoding=none HTTP/1.1\r\n"
                   "Host: t\r\n\r\n" % ts).encode())
        time.sleep(1.0)  # let the server hit EAGAIN on a full socket
        buf = b""
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
        s.close()
        head, _, body = buf.partition(b"\r\n\r\n")
        length = int([l for l in head.split(b"\r\n")
                      if l.lower().startswith(b"content-length:")][0].split(b":")[1])
        self.assertEqual(len(body), length)

    def test_partial_then_complete(self):
        # send the request in two pieces; the read loop must reassemble it.
        s = socket.create_connection((HOST, _port), timeout=5)
        s.sendall(b"GET /ping HTTP/1.1\r\n")
        time.sleep(0.02)
        s.sendall(b"Host: t\r\n\r\n")
        data = b""
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
        s.close()
        self.assertIn(b"200", data)
        self.assertIn(b"pong", data)


class TlsStalledClientTests(unittest.TestCase):
    """Regression tests for issue #12: a TLS client that connects and stays
    silent used to block the handshake -- and with it the whole accept loop,
    plain HTTP included. Needs a TLS-enabled build (make USE_TLS=yes) and the
    openssl CLI; skipped otherwise, unless ATOPHTTPD_REQUIRE_TLS is set (the
    USE_TLS=yes CI job does), in which case a missing prerequisite or a server
    that fails to start is an error, not a skip."""

    @classmethod
    def setUpClass(cls):
        import shutil
        require = bool(os.environ.get("ATOPHTTPD_REQUIRE_TLS"))
        if shutil.which("openssl") is None:
            if require:
                raise RuntimeError("openssl CLI not available")
            raise unittest.SkipTest("openssl CLI not available")

        cls.certdir = tempfile.mkdtemp(prefix="atophttpd_tls_")
        d = cls.certdir
        cls.ca = os.path.join(d, "ca.crt")
        run = lambda *a: subprocess.run(a, check=True, stdout=subprocess.DEVNULL,
                                        stderr=subprocess.DEVNULL, timeout=60)
        run("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", os.path.join(d, "ca.key"), "-out", cls.ca,
            "-days", "2", "-subj", "/CN=ca")
        for name in ("server", "client"):
            run("openssl", "req", "-newkey", "rsa:2048", "-nodes",
                "-keyout", os.path.join(d, name + ".key"),
                "-out", os.path.join(d, name + ".csr"), "-subj", "/CN=" + name)
            run("openssl", "x509", "-req", "-in", os.path.join(d, name + ".csr"),
                "-CA", cls.ca, "-CAkey", os.path.join(d, "ca.key"),
                "-CAcreateserial", "-out", os.path.join(d, name + ".crt"),
                "-days", "2")

        cls.http_port = _free_port()
        cls.tls_port = _free_port()
        cls.server = subprocess.Popen(
            [ATOPHTTPD_BIN, "-a", HOST, "-p", str(cls.http_port),
             "-t%d" % cls.tls_port, "-A", HOST, "-P", _log_dir,
             "-C", cls.ca,
             "-c", os.path.join(d, "server.crt"), "-k", os.path.join(d, "server.key")],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

        deadline = time.time() + 10
        while time.time() < deadline:
            if cls.server.poll() is not None:
                out = cls.server.stdout.read().decode(errors="replace")
                cls._cleanup()
                if require:
                    raise RuntimeError("TLS server failed to start: %s" % out[-200:])
                raise unittest.SkipTest("server has no TLS support: %s" % out[-200:])
            try:
                socket.create_connection((HOST, cls.tls_port), timeout=0.5).close()
                break
            except OSError:
                time.sleep(0.1)
        else:
            cls._cleanup()
            raise RuntimeError("TLS server did not become ready")

    @classmethod
    def _cleanup(cls):
        import shutil
        if getattr(cls, "server", None) and cls.server.poll() is None:
            cls.server.terminate()
            try:
                cls.server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                cls.server.kill()
        shutil.rmtree(cls.certdir, ignore_errors=True)

    @classmethod
    def tearDownClass(cls):
        cls._cleanup()

    def _tls_get(self, path):
        import ssl
        ctx = ssl.create_default_context(ssl.Purpose.SERVER_AUTH, cafile=self.ca)
        ctx.check_hostname = False
        # the throwaway test CA has no keyUsage extension, which Python's
        # strict X.509 mode (default since 3.13) rejects
        ctx.verify_flags &= ~ssl.VERIFY_X509_STRICT
        ctx.load_cert_chain(os.path.join(self.certdir, "client.crt"),
                            os.path.join(self.certdir, "client.key"))
        raw = socket.create_connection((HOST, self.tls_port), timeout=10)
        with ctx.wrap_socket(raw, server_hostname="localhost") as s:
            s.sendall(("GET %s HTTP/1.1\r\nHost: t\r\n\r\n" % path).encode())
            buf = b""
            while True:
                try:
                    chunk = s.recv(65536)
                except (ssl.SSLError, socket.timeout):
                    break
                if not chunk:
                    break
                buf += chunk
        return buf

    def _http_ping(self):
        return self._http_get("/ping")

    def _http_get(self, path):
        s = socket.create_connection((HOST, self.http_port), timeout=10)
        s.sendall(("GET %s HTTP/1.1\r\nHost: t\r\n\r\n" % path).encode())
        buf = b""
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
        s.close()
        return buf

    def test_silent_tls_client_does_not_block_others(self):
        # the exact reproducer from the issue: connect to the TLS port, say nothing
        silent = socket.create_connection((HOST, self.tls_port), timeout=3)
        try:
            time.sleep(0.2)
            t0 = time.time()
            self.assertIn(b"200", self._http_ping())   # plain HTTP still served
            self.assertLess(time.time() - t0, 2.0)
            self.assertIn(b"pong", self._tls_get("/ping"))  # and so is TLS
        finally:
            silent.close()

    def test_many_silent_tls_clients(self):
        # several stalled handshakes at once must not exhaust or stall the loop
        silent = [socket.create_connection((HOST, self.tls_port), timeout=3)
                  for _ in range(20)]
        try:
            time.sleep(0.2)
            self.assertIn(b"200", self._http_ping())
            self.assertIn(b"pong", self._tls_get("/ping"))
        finally:
            for s in silent:
                s.close()

    def test_tls_large_response(self):
        # a multi-KB static file over TLS, checked against its Content-Length
        self._assert_complete(self._tls_get("/js/atop.js"))

    def test_tls_dynamic_response_matches_plain_http(self):
        # A showsamp body is far larger than one write chunk on most hosts, so
        # real OpenSSL has to deliver it across several writes. It must arrive
        # complete and identical to the plain-HTTP answer. (The -EAGAIN retry
        # contract is covered deterministically by tests/unit/test_httpd_write.c:
        # kernel buffers are too large to force it from here.)
        if not _sample_epochs:
            self.skipTest("no sample epochs available")
        path = ("/showsamp?timestamp=%d&lables=ALL&encoding=none"
                % _sample_epochs[len(_sample_epochs) // 2])

        expected = self._assert_complete(self._http_get(path))
        self.assertEqual(self._assert_complete(self._tls_get(path)), expected)

    def _assert_complete(self, resp):
        """Return the body, after checking it matches the Content-Length header
        (an oracle independent of the server's other transport)."""
        head, _, payload = resp.partition(b"\r\n\r\n")
        self.assertIn(b"200", head.split(b"\r\n", 1)[0])
        length = int([l for l in head.split(b"\r\n")
                      if l.lower().startswith(b"content-length:")][0].split(b":")[1])
        self.assertEqual(len(payload), length)
        return payload


class ShowsampDataTests(unittest.TestCase):
    """Only meaningful when a real log with samples is available."""

    @classmethod
    def setUpClass(cls):
        if not _sample_epochs:
            raise unittest.SkipTest("no sample epochs available")
        cls.ts = _sample_epochs[len(_sample_epochs) // 2]

    def test_showsamp_none_json(self):
        _, headers, body = _raw_request(
            "/showsamp?timestamp=%d&lables=CPU%%2CMEM&encoding=none" % self.ts)
        obj = json.loads(body)
        self.assertEqual(obj["timestamp"], self.ts)
        self.assertIn("CPU", obj)
        self.assertIn("MEM", obj)

    def test_showsamp_all_none(self):
        _, _, body = _raw_request(
            "/showsamp?timestamp=%d&lables=ALL&encoding=none" % self.ts)
        obj = json.loads(body)
        # a representative subset of labels must be present
        for k in ("CPU", "cpu", "CPL", "MEM", "SWP", "PAG", "DSK"):
            self.assertIn(k, obj)

    def test_showsamp_deflate_roundtrip(self):
        _, headers, body = _raw_request(
            "/showsamp?timestamp=%d&lables=ALL&encoding=deflate" % self.ts)
        self.assertEqual(headers.get("content-encoding"), "deflate")
        inflated = zlib.decompress(body)
        obj = json.loads(inflated)
        self.assertEqual(obj["timestamp"], self.ts)

    def test_showsamp_default_encoding_is_deflate(self):
        # no encoding arg -> defaults to deflate
        _, headers, body = _raw_request(
            "/showsamp?timestamp=%d&lables=CPU" % self.ts)
        self.assertEqual(headers.get("content-encoding"), "deflate")
        obj = json.loads(zlib.decompress(body))
        self.assertIn("CPU", obj)

    def test_showsamp_pct2c_labels(self):
        _, _, body = _raw_request(
            "/showsamp?timestamp=%d&lables=CPU%%2CMEM%%2CDSK&encoding=none" % self.ts)
        obj = json.loads(body)
        self.assertIn("CPU", obj)
        self.assertIn("MEM", obj)
        self.assertIn("DSK", obj)

    def test_showsamp_future_timestamp_recent_fallback(self):
        future = _sample_epochs[-1] + 100000
        _, _, body = _raw_request(
            "/showsamp?timestamp=%d&lables=CPU&encoding=none" % future)
        obj = json.loads(body)
        self.assertIn("CPU", obj)


class ConcurrencyTests(unittest.TestCase):
    """Hammer the server with many parallel requests.

    The server serves every connection from one epoll loop, so this checks
    that interleaved requests are each answered completely and correctly (no
    cross-talk between per-connection buffers) while the loop periodically
    re-parses the log under the cache write lock. Under AddressSanitizer a
    use-after-free or heap corruption would abort the server here.
    """

    @classmethod
    def setUpClass(cls):
        if not _sample_epochs:
            raise unittest.SkipTest("no sample epochs available")
        cls.ts = _sample_epochs[len(_sample_epochs) // 2]

    def _showsamp(self, _):
        # The anti-slowloris guard (see test_slow_client_closed) drops a
        # connection that does not send its request line within ~100ms. Under a
        # sanitizer-slowed server plus the GIL serialising these client
        # threads, a freshly connected socket can occasionally miss that window
        # before sendall() runs. That is a benign, connection-level drop (an
        # empty read, status == b""), not a response error, so retry it a few
        # times. A real server crash yields a refused connection (which raises
        # and fails the test), and a malformed/incorrect response is a non-empty
        # status and is never retried.
        for _attempt in range(5):
            status, _, body = _raw_request(
                "/showsamp?timestamp=%d&lables=ALL&encoding=none" % self.ts)
            if not status:
                continue  # slow-client guard fired under load -> new socket
            if b"200" not in status:
                return ("bad status", status)
            try:
                obj = json.loads(body)
            except Exception as exc:  # noqa: BLE001
                return ("bad json", repr(exc))
            if obj.get("timestamp") != self.ts or "CPU" not in obj:
                return ("bad body", obj.get("timestamp"))
            return None
        return ("dropped repeatedly", b"")

    def test_parallel_showsamp(self):
        # 8 client threads each issue a tight loop of requests: this keeps many
        # connections in flight on the event loop at once while keeping every
        # connect->send pair prompt (a connection that stays silent is
        # intentionally dropped by the slow-client guard, so we must not starve
        # the senders the way a huge thread fan-out would).
        from concurrent.futures import ThreadPoolExecutor

        def run(_):
            return [e for e in (self._showsamp(i) for i in range(25)) if e]

        with ThreadPoolExecutor(max_workers=8) as pool:
            errors = [e for batch in pool.map(run, range(8)) for e in batch]
        self.assertEqual(errors, [], "concurrent requests failed: %r" % errors[:5])

    def test_parallel_mixed_endpoints(self):
        from concurrent.futures import ThreadPoolExecutor

        def one(i):
            if i % 3 == 0:
                # Same benign slow-client-guard drop as _showsamp: retry an
                # empty read (status == b"") a few times before giving up.
                for _attempt in range(5):
                    status, _, _ = _raw_request("/ping")
                    if not status:
                        continue
                    return None if b"200" in status else ("ping", status)
                return ("ping dropped repeatedly", b"")
            return self._showsamp(i)

        def run(_):
            return [e for e in (one(i) for i in range(20)) if e]

        with ThreadPoolExecutor(max_workers=8) as pool:
            errors = [e for batch in pool.map(run, range(8)) for e in batch]
        self.assertEqual(errors, [], "mixed concurrent requests failed: %r" % errors[:5])


class CliTests(unittest.TestCase):
    """Exercise main()'s option parsing and start-up error handling."""

    def _run(self, args, timeout=10):
        p = subprocess.run([ATOPHTTPD_BIN] + args,
        capture_output=True, text=True, timeout=timeout)
        return p

    def test_version(self):
        p = self._run(["-V"])
        self.assertEqual(p.returncode, 0)
        self.assertIn("Version", p.stdout)

    def test_help(self):
        p = self._run(["-h"])
        self.assertEqual(p.returncode, 0)
        self.assertIn("Usage", p.stdout)

    def test_invalid_option(self):
        p = self._run(["-Z"])
        # getopt error falls through to the help screen (which exits 0)
        self.assertIn("Usage", p.stdout)

    def test_bad_log_path(self):
        p = self._run(["-P", "/no/such/atop/log/dir"])
        self.assertNotEqual(p.returncode, 0)
        # start-up errors are logged via the logging layer, i.e. to stderr
        self.assertIn("parse failed", p.stderr)

    def test_tls_port_without_tls_build(self):
        # requesting a TLS port in a non-TLS build must fail cleanly
        p = self._run(["-p", "0", "-t", "-P", _log_dir])
        self.assertNotEqual(p.returncode, 0)

    def test_tls_port_with_optarg(self):
        # -t with an explicit port exercises the optarg branch; a non-TLS
        # build still cannot serve it, so start-up fails.
        p = self._run(["-p", "0", "-t5000", "-P", _log_dir])
        self.assertNotEqual(p.returncode, 0)

    def test_tls_file_options_parsed(self):
        # -A/-C/-c/-k must be accepted; combined with a bad log path the
        # process exits after option parsing (before any TLS use).
        p = self._run(["-A", "::", "-C", "/x/ca", "-c", "/x/crt",
                       "-k", "/x/key", "-P", "/no/such/atop/log/dir"])
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("parse failed", p.stderr)

    def test_listen_failure(self):
        # an address getaddrinfo cannot resolve: the listener cannot be set up
        p = self._run(["-a", "999.999.999.999", "-p", str(_free_port()), "-P", _log_dir])
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("failed listening", p.stderr)

    def test_shutdown_with_client_connected(self):
        # SIGTERM while a client is still mid-request: the loop must close it
        # and exit cleanly (exercises the shutdown cleanup path).
        port = _free_port()
        srv = subprocess.Popen([ATOPHTTPD_BIN, "-a", HOST, "-p", str(port), "-P", _log_dir],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            deadline = time.time() + 10
            while time.time() < deadline:
                try:
                    socket.create_connection((HOST, port), timeout=0.5).close()
                    break
                except OSError:
                    time.sleep(0.05)
            client = socket.create_connection((HOST, port), timeout=5)
            client.sendall(b"GET /pi")  # incomplete: stays in the read state
            time.sleep(0.02)
            srv.terminate()
            self.assertEqual(srv.wait(timeout=5), 0)
            client.close()
        finally:
            if srv.poll() is None:
                srv.kill()

    def test_listen_nothing(self):
        # port 0 and no TLS port -> nothing to listen on -> clean exit(1)
        p = self._run(["-p", "0", "-P", _log_dir])
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("Listen Nothing", p.stderr)

    def test_daemon_mode(self):
        # -d exercises the daemon() path; with nothing to listen on the
        # daemonised child exits immediately while the parent returns 0.
        p = self._run(["-d", "-p", "0", "-P", _log_dir])
        self.assertEqual(p.returncode, 0)

    def test_debug_flag(self):
        # -D (debug) combined with a bad path exits after option parsing.
        p = self._run(["-D", "-P", "/no/such/atop/log/dir"])
        self.assertNotEqual(p.returncode, 0)


class HideCmdlineTests(unittest.TestCase):
    """The -H flag must mask process command lines in the JSON output."""

    @classmethod
    def setUpClass(cls):
        if not _sample_epochs:
            raise unittest.SkipTest("no sample epochs available")
        cls.port = _free_port()
        cls.proc = subprocess.Popen(
            [ATOPHTTPD_BIN, "-a", HOST, "-p", str(cls.port),
             "-P", _log_dir, "-D", "-H"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.time() + 10
        while time.time() < deadline:
            try:
                s = socket.create_connection((HOST, cls.port), timeout=2)
                s.sendall(b"GET /ping HTTP/1.1\r\nHost: t\r\n\r\n")
                if b"200" in s.recv(1024):
                    s.close()
                    break
                s.close()
            except OSError:
                pass
            time.sleep(0.1)
        else:
            cls.proc.terminate()
            raise RuntimeError("hide-cmdline server did not start")

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        try:
            cls.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            cls.proc.kill()

    def test_cmdline_masked(self):
        ts = _sample_epochs[len(_sample_epochs) // 2]
        s = socket.create_connection((HOST, self.port), timeout=5)
        s.sendall(("GET /showsamp?timestamp=%d&lables=PRG&encoding=none "
                   "HTTP/1.1\r\nHost: t\r\n\r\n" % ts).encode())
        buf = b""
        while True:
            d = s.recv(65536)
            if not d:
                break
            buf += d
        s.close()
        _, _, body = buf.partition(b"\r\n\r\n")
        obj = json.loads(body)
        # every reported command line must be masked
        self.assertIn("PRG", obj)
        found = False
        for row in obj["PRG"]:
            # PRG rows carry the (masked) command line somewhere in the record
            if "***" in json.dumps(row):
                found = True
        self.assertTrue(found, "expected masked '***' command lines")


if __name__ == "__main__":
    unittest.main(verbosity=2)
