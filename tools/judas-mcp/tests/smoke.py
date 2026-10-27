"""Real smoke test: spawn the MCP server over stdio and drive the live daemon."""
import asyncio
import json
import sys

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

CWD = sys.argv[1]


async def main():
    p = StdioServerParameters(command=sys.executable, args=["-m", "judas_mcp.server"])
    async with stdio_client(p) as (r, w):
        async with ClientSession(r, w) as s:
            await s.initialize()
            print("tools:", sorted(t.name for t in (await s.list_tools()).tools))

            async def t(name, **a):
                out = (await s.call_tool(name, a)).content[0].text
                print(f"--- {name}\n{out}")
                return json.loads(out)
            await t("judas_up")
            run = await t("judas_run", task="create hello.txt containing hi, then call done", cwd=CWD, wait_s=300)
            await t("judas_result", session=run["session"])
            await t("judas_sessions", limit=2)

asyncio.run(main())
