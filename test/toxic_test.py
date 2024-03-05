import tuitest


class ToxicTest(tuitest.TestCase):
    async def start_toxic(self) -> None:
        self.write("toxic", "C-m")
        self.assertTrue(await self.visible("Welcome to Toxic"))

    @tuitest.blocking
    async def test_0_init(self) -> None:
        """Name starts with 0 to make sure it runs first."""
        self.write("toxic", "C-m")
        self.assertTrue(await self.visible("Would you like to encrypt it"))
        self.write("n", "C-m")
        self.assertTrue(await self.visible("Welcome to Toxic"))

    @tuitest.blocking
    async def test_help(self) -> None:
        # terminal.write("toxic --help\r");
        # await expect(terminal.getByText("usage: toxic")).toBeVisible(fast);
        # await expect(terminal).toMatchSnapshot();
        self.write("toxic --help", "C-m")
        self.assertTrue(await self.visible("usage: toxic"))

    @tuitest.blocking
    async def test_help_window(self) -> None:
        await self.start_toxic()
        self.write("/help", "C-m")
        self.assertTrue(await self.visible("Help Menu"))

    @tuitest.blocking
    async def test_global_help_window(self) -> None:
        await self.start_toxic()
        self.write("/help", "C-m")
        self.assertTrue(await self.visible("Help Menu"))
        self.write("g")
        self.assertTrue(await self.visible("Global Commands"))

    @tuitest.blocking
    async def test_help_window_exit(self) -> None:
        await self.start_toxic()
        self.write("/help", "C-m")
        self.assertTrue(await self.visible("Help Menu"))
        self.write("x")
        self.assertTrue(await self.not_visible("Global Commands"))

    @tuitest.blocking
    async def test_terminal_resize(self) -> None:
        await self.start_toxic()
        for i in range(10):
            self.write(f"we're ({i + 1}) writing some pretty long message here, this is message {i + 1}", "C-m")
        self.assertTrue(await self.visible("message 10"))
        self.resize(60, 20);
        self.assertTrue(await self.visible("message 10"))


if __name__ == "__main__":
    tuitest.main()
