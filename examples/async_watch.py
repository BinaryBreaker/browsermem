"""Consume tab events from asyncio."""

import asyncio

import browsermem


async def main() -> None:
    async with browsermem.TabMonitor() as monitor:
        async for tab in monitor.aevents():
            print(tab.event.name, tab.title, tab.url)


try:
    asyncio.run(main())
except KeyboardInterrupt:
    pass
