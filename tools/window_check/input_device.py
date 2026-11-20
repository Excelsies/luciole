"""The input helper process: virtual pointer and keyboard commands, one answer per command."""

import select
import subprocess
import time
from collections.abc import Callable

from build import HELPER
from processes import kill_with_parent

REPLY_TIMEOUT = 10.0
POLL = 0.05


class InputError(RuntimeError):
    pass


class InputHelper:
    def __init__(self, env: dict[str, str], watch: Callable[[], None]):
        self.watch = watch
        self.process = subprocess.Popen([str(HELPER)], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, text=True, bufsize=1, start_new_session=True,
                                        preexec_fn=kill_with_parent())
        reply = self._reply()
        if reply != "ready":
            raise InputError(f"the input helper did not start: {reply}")

    def _reply(self) -> str:
        deadline = time.monotonic() + REPLY_TIMEOUT
        while time.monotonic() < deadline:
            ready, _, _ = select.select([self.process.stdout], [], [], POLL)
            if ready:
                line = self.process.stdout.readline()
                if not line:
                    raise InputError(f"the input helper exited with {self.process.wait(5)}")
                return line.strip()
            self.watch()
        raise InputError(f"the input helper gave no answer within {REPLY_TIMEOUT:.0f} s")

    def send(self, command: str) -> None:
        try:
            self.process.stdin.write(command + "\n")
            self.process.stdin.flush()
        except OSError:
            raise InputError(f"the input helper exited with {self.process.poll()}") from None
        reply = self._reply()
        if reply != "ok":
            raise InputError(reply.removeprefix("error "))

    def stop(self) -> None:
        if self.process.poll() is None:
            try:
                self.process.stdin.close()
            except OSError:
                pass
            try:
                self.process.wait(3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(3)
