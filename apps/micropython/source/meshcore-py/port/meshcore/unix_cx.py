# SPDX-License-Identifier: MIT
"""The badge's link to meshcored: its Unix socket, with the same frames as
MeshCore's TCP link, so TCPConnection does everything but connect."""
import asyncio

from .tcp_cx import TCPConnection

SOCKET = "/var/run/meshcored.sock"


class UnixConnection(TCPConnection):
    def __init__(self, path=SOCKET):
        super().__init__(path, None)
        self.path = path

    async def connect(self):
        loop = asyncio.get_running_loop()
        await loop.create_unix_connection(lambda: self.MCClientProtocol(self), self.path)
        return self.path
