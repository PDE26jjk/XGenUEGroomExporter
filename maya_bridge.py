"""Minimal Agent <-> Maya bridge over Maya's TCP command port.

Talks to a running Maya instance through the `commandPort` socket (the same
mechanism used by MayaLog2Jupyter).  Everything here uses only the Python
standard library so it can run from any Python 3 environment, including the
one used by the agent itself.

The command port is single-client, so every call opens a fresh connection,
sends one command, reads until the NUL terminator, and closes.  Maya must be
started with a command port listening, e.g. in Maya script editor:

    import maya.cmds as cmds
    if not cmds.commandPort(":4434", query=True):
        cmds.commandPort(name=":4434")

Python code is wrapped in MEL's `python("...")`, which is the only way to run
Python through a MEL-typed command port.  `print()` output does not travel
over the socket; it goes to Maya's script editor.  To capture it, attach a
log file with `cmdFileOutput` and read the delta afterwards (see attach_log /
take_log).
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import tempfile
import threading
from pathlib import Path
from typing import Optional


DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 4434
DEFAULT_TIMEOUT = 30.0

# Markers emitted by Maya's command port when execution failed.  The first one
# is the zh_CN locale form, the second the en_US form.
_ERROR_MARKERS = ("Traceback", "执行 MEL 脚本过程中出错", "Error executing MEL script")


def _wrap_python(code: str) -> str:
    """Escape Python code so it can be embedded in MEL's python("...")."""
    escaped = (
        code.replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("\r\n", "\n")
        .replace("\r", "\n")
        .replace("\n", "\\n")
    )
    return 'python("' + escaped + '")'


def _decode_response(raw: bytes) -> str:
    """Decode a command-port response, stripping NUL padding and whitespace."""
    text = raw.strip(b"\x00").decode("utf-8", errors="replace")
    return text.strip()


class MayaBridge:
    """A tiny synchronous client for Maya's TCP command port."""

    def __init__(
        self,
        host: str = DEFAULT_HOST,
        port: int = DEFAULT_PORT,
        timeout: float = DEFAULT_TIMEOUT,
    ) -> None:
        self.host = host
        self.port = port
        self.timeout = timeout
        # The command port accepts one client at a time; serialize calls so an
        # agent that fires requests from multiple threads cannot interleave.
        self._lock = threading.Lock()
        self._log_offset: Optional[int] = None
        self._attached_path: Optional[str] = None

    # -- raw transport -----------------------------------------------------

    def _send(self, code: str, timeout: Optional[float] = None) -> str:
        effective_timeout = self.timeout if timeout is None else timeout
        with self._lock:
            raw = b""
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as client:
                client.settimeout(effective_timeout)
                client.connect((self.host, self.port))
                client.sendall(code.encode("utf-8") + b"\n")
                while True:
                    chunk = client.recv(8192)
                    if not chunk:
                        break
                    raw += chunk
                    if raw.endswith(b"\x00"):
                        break
            return _decode_response(raw)

    def mel(self, code: str, timeout: Optional[float] = None) -> str:
        """Run raw MEL code and return the response text."""
        return self._send(code, timeout=timeout)

    def python(self, code: str, timeout: Optional[float] = None) -> str:
        """Run Python code inside Maya via the MEL python() wrapper."""
        return self._send(_wrap_python(code), timeout=timeout)

    # -- structured helpers ------------------------------------------------

    @staticmethod
    def is_error(text: str) -> bool:
        return any(marker in text for marker in _ERROR_MARKERS)

    def run_python(self, code: str, timeout: Optional[float] = None) -> dict:
        """Run Python code and return {'ok': bool, 'result': str}."""
        text = self.python(code, timeout=timeout)
        if self.is_error(text):
            return {"ok": False, "result": "", "error": text}
        return {"ok": True, "result": text, "error": ""}

    def eval_json(self, expression: str, timeout: Optional[float] = None):
        """Evaluate a single-line Python expression returning JSON-able data.

        Maya's python() command only returns a value when the code is a single
        line, so the caller must pass one line of code.
        """
        code = "import json; json.dumps({})".format(expression)
        text = self.python(code, timeout=timeout)
        if self.is_error(text):
            raise RuntimeError(text)
        return json.loads(text) if text else None

    # -- stdout capture via cmdFileOutput ----------------------------------

    def attach_log(self, path: Optional[str] = None) -> str:
        """Redirect Maya's script-editor output to a file and start capturing.

        Returns the log path.  After this, take_log() returns only the output
        produced since the previous take_log() call.
        """
        if path is None:
            handle, path = tempfile.mkstemp(prefix="maya_bridge_", suffix=".log")
            os.close(handle)
        log_path = Path(path).as_posix()
        code = (
            "import maya.cmds as cmds\n"
            "cmds.cmdFileOutput(closeAll=True)\n"
            'cmds.cmdFileOutput(o="{}")\n'.format(log_path)
        )
        self.python(code)
        self._attached_path = log_path
        # Remember the current end of the file so take_log() only returns
        # output produced after attach.
        if Path(log_path).exists():
            self._log_offset = Path(log_path).stat().st_size
        else:
            self._log_offset = 0
        return log_path

    def detach_log(self) -> None:
        self.python("import maya.cmds as cmds; cmds.cmdFileOutput(closeAll=True)")
        self._attached_path = None
        self._log_offset = None

    def take_log(self) -> str:
        """Return new bytes appended to the attached log since the last read."""
        if self._attached_path is None or self._log_offset is None:
            raise RuntimeError("attach_log() must be called before take_log()")
        path = Path(self._attached_path)
        with open(path, "rb") as f:
            f.seek(self._log_offset)
            data = f.read()
            self._log_offset = f.tell()
        return data.decode("utf-8", errors="replace")

    def run_python_with_log(self, code: str, timeout: Optional[float] = None) -> dict:
        """Run code and return {'ok', 'result', 'error', 'stdout'}."""
        before = self._log_offset
        if before is None:
            raise RuntimeError("attach_log() must be called before run_python_with_log()")
        outcome = self.run_python(code, timeout=timeout)
        stdout = self.take_log()
        return {**outcome, "stdout": stdout}


def main() -> None:
    parser = argparse.ArgumentParser(description="Talk to a running Maya instance.")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="command port")
    parser.add_argument("--mel", help="raw MEL code to run")
    parser.add_argument("--python", dest="pycode", help="Python code to run")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    args = parser.parse_args()

    bridge = MayaBridge(port=args.port, timeout=args.timeout)
    if args.pycode is not None:
        print(bridge.python(args.pycode))
    elif args.mel is not None:
        print(bridge.mel(args.mel))
    else:
        parser.error("provide --mel or --python")


if __name__ == "__main__":
    main()
