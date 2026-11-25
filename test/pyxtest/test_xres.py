# SPDX-License-Identifier: MIT
#
# Tests for X-Resource extension.

import os
import struct

import pytest
from proto import xres
from proto.x11 import BadLength
from xclient import Extension, X11Error, X11Reply

# numSpecs just past UINT32_MAX / sizeof(xXResClientIdSpec|xXResResourceIdSpec)
# (both specs are 8 bytes). On 32-bit this wraps the request-size multiply.
OVERFLOW_NUM_SPECS = 0x20000000


def init_xres(conn):
    ext = conn.query_extension(Extension.XRES)
    if not ext:
        pytest.skip("X-Resource extension not available")

    req = xres.QueryVersionRequest(opcode=ext.opcode)
    conn.send_request(req)
    resp = conn.recv_response(timeout=5.0)
    if not isinstance(resp, X11Reply):
        pytest.skip("XRes QueryVersion failed")

    return conn, ext.opcode


@pytest.fixture
def xres_xclient(xclient):
    """Provide an xclient with X-Resource initialized."""
    return init_xres(xclient)


@pytest.fixture
def xres_xclient_swapped(xclient_swapped):
    """Provide a byte-swapped xclient with X-Resource initialized."""
    return init_xres(xclient_swapped)


class TestXResQueryClientIds:
    @pytest.mark.swapped_client
    def test_query_client_ids_spec_entries_swapped(self, xserver, xres_xclient_swapped):
        """
        SProcXResQueryClientIds was missing byte-swaps for the
        xXResClientIdSpec entries.  Without the swaps, the mask field
        (e.g. XResClientXIDMask=1) is seen as 0x01000000, which
        doesn't match any known mask → the reply contains zero results.

        With the fix, the reply should contain at least one result.

        Fixed in commit f7b574931544 ("Xext/xres: add missing byte-swap
        of spec entries in SProcXResQueryClientIds").
        """
        conn, opcode = xres_xclient_swapped

        # X_XResClientXIDMask = 1.
        # client=0 means "all clients" (the wildcard).
        req = xres.QueryClientIdsRequest(
            opcode=opcode,
            specs=[(0, 1)],  # mask=1 (XResClientXIDMask)
        )
        conn.send_request(req)
        resp = conn.recv_response(timeout=5.0)

        assert xserver.is_alive, "Server crashed"
        assert isinstance(resp, X11Reply), f"Expected reply, got {resp}"

        # xXResQueryClientIdsReply:
        #   [8] numIds(4)
        num_ids = struct.unpack_from(">I", resp.data, 8)[0]
        assert num_ids > 0, (
            "QueryClientIds returned numIds=0, expected >0. "
            "The spec entry mask was not byte-swapped, so the "
            "server didn't recognize XResClientXIDMask."
        )

    @pytest.mark.swapped_client
    def test_construct_client_id_value_swap_check(
        self, xserver, xres_xclient_swapped, xclient
    ):
        """
        ConstructClientIdValue used client->swapped instead of
        sendClient->swapped.  When a big-endian client queries
        a native client's ID, the spec.client field in the reply
        is not swapped → garbled value.

        We create an additional native (little-endian) connection.
        The big-endian connection queries the native client's XID.
        With the fix, the returned spec.client should match the
        native client's resource base.  Without the fix, it's
        byte-swapped garbage.

        Fixed in commit d2d4fb35e798 ("Xext/xres: fix wrong swap
        check").
        """
        conn, opcode = xres_xclient_swapped
        conn_native = xclient

        # Get the list of clients
        req = xres.QueryClientsRequest(opcode=opcode)
        conn.send_request(req)
        resp = conn.recv_response(timeout=5.0)
        assert isinstance(resp, X11Reply), f"Expected reply, got {resp}"

        num_clients = struct.unpack_from(">I", resp.data, 8)[0]
        assert num_clients >= 2, f"Expected at least 2 clients, got {num_clients}"

        # Collect all client XIDs from the reply.
        # Each xXResClient is 8 bytes: resource_base(4) + resource_mask(4)
        client_xids = []
        for i in range(num_clients):
            offset = 32 + i * 8
            if offset + 4 <= len(resp.data):
                xid = struct.unpack_from(">I", resp.data, offset)[0]
                client_xids.append(xid)

        # Find the native client's resource base
        native_base = conn_native._resource_id_base
        target_xid = None
        for xid in client_xids:
            if (xid & 0xFFE00000) == (native_base & 0xFFE00000):
                target_xid = xid
                break

        if target_xid is None:
            pytest.skip("Could not find native client in XRes client list")

        # Query the native client's XID using XResClientXIDMask
        req = xres.QueryClientIdsRequest(
            opcode=opcode,
            specs=[(target_xid, 1)],  # XResClientXIDMask
        )
        conn.send_request(req)
        resp = conn.recv_response(timeout=5.0)

        assert isinstance(resp, X11Reply), f"Expected reply, got {resp}"

        num_ids = struct.unpack_from(">I", resp.data, 8)[0]
        assert num_ids > 0, f"Expected >0 IDs, got {num_ids}"

        # xXResClientIdValue:
        #   spec.client(4) + spec.mask(4) + length(4) + value(4*length)
        spec_client = struct.unpack_from(">I", resp.data, 32)[0]

        assert spec_client == target_xid, (
            f"spec.client = {spec_client:#010x}, "
            f"expected {target_xid:#010x}. "
            f"The swap check used client->swapped instead of "
            f"sendClient->swapped."
        )

    def test_query_client_ids_numspecs_overflow(self, xserver, xres_xclient):
        """
        On 32-bit servers, numSpecs * sizeof(xXResClientIdSpec) can wrap
        before REQUEST_FIXED_SIZE. A header-only request with a huge
        numSpecs then walks past the request buffer in Proc or SProc.

        Reject numSpecs that cannot be represented in the size calculation.
        """
        conn, opcode = xres_xclient

        req = xres.QueryClientIdsRequest(
            opcode=opcode,
            specs=[],
            num_specs_override=OVERFLOW_NUM_SPECS,
        )
        conn.send_request(req)
        resp = conn.recv_response(timeout=5.0)

        assert xserver.is_alive, "Server crashed on QueryClientIds numSpecs overflow"
        assert isinstance(resp, X11Error), f"Expected BadLength, got {resp}"
        assert resp.error_code == BadLength, (
            f"Expected BadLength ({BadLength}), got {resp.error_code}"
        )


class TestXResQueryResourceBytes:
    @pytest.mark.swapped_client
    def test_query_resource_bytes_numspecs_overflow_swapped(
        self, xserver, xres_xclient_swapped
    ):
        """
        SProcXResQueryResourceBytes multiplied numSpecs by the spec size
        before REQUEST_FIXED_SIZE. On 32-bit servers a large numSpecs
        wraps, passes length validation, and the swap loop walks past
        the request buffer.

        Reject overflowing numSpecs before swapping the array.
        """
        conn, opcode = xres_xclient_swapped

        req = xres.QueryResourceBytesRequest(
            opcode=opcode,
            client=0,
            specs=[],
            num_specs_override=OVERFLOW_NUM_SPECS,
        )
        conn.send_request(req)
        resp = conn.recv_response(timeout=5.0)

        assert xserver.is_alive, (
            "Server crashed on QueryResourceBytes numSpecs overflow"
        )
        assert isinstance(resp, X11Error), f"Expected BadLength, got {resp}"
        assert resp.error_code == BadLength, (
            f"Expected BadLength ({BadLength}), got {resp.error_code}"
        )


XRES_CLIENT_ID_PID_MASK = 1 << 1


class TestXResClientPid:
    def test_query_client_ids_reports_connecting_pid(self, xserver, xres_xclient):
        """
        The PID reported for a client is the identity the server recorded
        when the client connected (os/peer_identity.c): it must be the PID of
        the process that opened the connection, i.e. this test process.
        """
        import os

        conn, opcode = xres_xclient
        conn.send_request(
            xres.QueryClientIdsRequest(
                opcode=opcode,
                specs=[(conn._resource_id_base, XRES_CLIENT_ID_PID_MASK)],
            )
        )
        resp = conn.recv_response(timeout=5.0)
        assert isinstance(resp, X11Reply), resp

        num_ids = struct.unpack_from("<I", resp.data, 8)[0]
        offset = 32
        pids = []
        for _ in range(num_ids):
            _client, mask, length = struct.unpack_from("<III", resp.data, offset)
            offset += 12
            if mask == XRES_CLIENT_ID_PID_MASK and length == 4:
                pids.append(struct.unpack_from("<I", resp.data, offset)[0])
            offset += length

        assert pids == [os.getpid()]


class TestClientIdentityResources:
    @pytest.mark.skipif(not os.path.isdir("/proc/self/fd"), reason="needs /proc")
    def test_connections_do_not_leak_fds(self, xserver):
        """
        Each local client's identity can hold a pidfd (Linux >= 6.5). It must
        be closed when the client disconnects, so the server's open file
        descriptor count must not grow with the number of past clients.
        """
        import time

        from xclient import RawX11Connection

        proc = getattr(xserver, "_process", None)
        if proc is None:
            pytest.skip("server process not managed by the test")
        fd_dir = f"/proc/{proc.pid}/fd"

        def server_fds():
            # let the server process the disconnects first
            with RawX11Connection(xserver.display_num) as sync_conn:
                sync_conn.send_request(struct.pack("<BxH", 43, 1))  # GetInputFocus
                sync_conn.recv_response(timeout=5.0)
            time.sleep(0.1)
            return len(os.listdir(fd_dir))

        before = server_fds()
        for _ in range(50):
            RawX11Connection(xserver.display_num).close()
        after = server_fds()

        assert after - before < 5, f"server fds grew from {before} to {after}"
