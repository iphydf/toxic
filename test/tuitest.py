import asyncio
import random
import string
import subprocess
import unittest


class TestCase(unittest.TestCase):
    def setUp(self) -> None:
        self._session = ''.join(random.choice(string.ascii_lowercase)
                                for i in range(10))
        subprocess.run(["tmux", "new-session", "-d", "-s", self._session], check=True)
        self.resize(80, 30)

    def tearDown(self) -> None:
        subprocess.run(["tmux", "kill-session", "-t", self._session], check=True)

    def write(self, *text: str) -> None:
        subprocess.run(["tmux", "send-keys", "-t", self._session] + list(text), check=True)

    def resize(self, columns: int, rows: int) -> None:
        subprocess.run(["tmux", "resize-window", "-t", self._session, "-x", str(columns), "-y", str(rows)], check=True)

    def dump(self) -> str:
        subprocess.run(["tmux", "capture-pane", "-t", self._session], check=True)
        stdout = subprocess.run(["tmux", "save-buffer", "/dev/stdout"], check=True, capture_output=True).stdout.decode("utf-8")
        subprocess.run(["tmux", "delete-buffer"], check=True)
        return stdout

    async def visible(self, text: str) -> bool:
        for _ in range(10):
            await asyncio.sleep(0.1)
            dump = self.dump()
            if text in dump:
                return True
        print(dump)
        return False

    async def not_visible(self, text: str) -> bool:
        for _ in range(10):
            await asyncio.sleep(0.1)
            dump = self.dump()
            if text not in dump:
                return True
        print(dump)
        return False



def blocking(f):
    return lambda *args: asyncio.run(f(*args))


def main() -> None:
    unittest.main()
