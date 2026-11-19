"""Consume tab events from asyncio."""

import asyncio
import contextlib

import browsermem


async def main() -> None:
    async with browsermem.TabMonitor() as monitor:
        async for tab in monitor.aevents():
            print(tab.event.name, tab.title, tab.url)


with contextlib.suppress(KeyboardInterrupt):
    asyncio.run(main())
