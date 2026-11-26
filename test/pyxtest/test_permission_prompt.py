# SPDX-License-Identifier: MIT
#
# Permission prompts (X-NAMESPACE 1.2, dix/capability.c): in enforce mode,
# a request refused for a missing capability is held while the
# permission agent asks the user, and runs again with the answer.
#
# The confined clients are in one namespace, where Xnamespace's own rules
# allow inspecting each other's windows; only the capability decides.

import os
import struct
import subprocess
import time
from pathlib import Path

import pytest
from test_capability import (BAD_ACCESS, ROOT_COOKIE, SANDBOX_COOKIE, X_GET_INPUT_FOCUS,
                             X_GET_WINDOW_ATTRIBUTES, _connect, _errors_of, xserver_args)
from xclient import X11Error

__all__ = ["xserver_args"]

X_XNS_GET_CLIENT_INFO = 12
X_XNS_SET_PERMISSION_AGENT = 14
X_XNS_ANSWER_PERMISSION = 15
DENY_ONCE, ALLOW_ONCE, ALLOW_ALWAYS, DENY_ALWAYS = range(4)
CAP_WINDOW_INSPECT = 6
BAD_VALUE = 2


@pytest.fixture
def capmode():
    return "enforce"


class Agent:
    def __init__(self, xserver):
        self.c = _connect(xserver, ROOT_COOKIE)
        ext = self.c.query_extension("X-NAMESPACE")
        assert ext
        self.op, self.event = ext.opcode, ext.first_event

    def request(self, minor, fmt="", *args):
        body = struct.pack("<" + fmt, *args) if fmt else b""
        return _errors_of(self.c, struct.pack("<BBH", self.op, minor, 1 + len(body) // 4) + body)

    def enable(self, on=True):
        return self.request(X_XNS_SET_PERMISSION_AGENT, "BxH", int(on), 0)

    def answer(self, prompt_id, answer):
        return self.request(X_XNS_ANSWER_PERMISSION, "IBxH", prompt_id, answer, 0)

    def prompts(self, timeout=2.0):
        """(id, capability, clientResource, target, major, minor) of the
        PermissionRequest events that came"""
        out = []
        while True:
            r = self.c.recv_response(timeout=timeout)
            if r is None:
                return out
            assert not isinstance(r, X11Error), r
            if r.response_type == self.event:
                cap = r.data[1]
                pid, client, target, major, minor = struct.unpack_from("<IIIBxH", r.data, 4)
                out.append((pid, cap, client, target, major, minor))
                timeout = 0.2

    def client_namespace(self, resource):
        seq = self.c.send_request(struct.pack("<BBHI", self.op, X_XNS_GET_CLIENT_INFO, 2,
                                              resource))
        while True:
            r = self.c.recv_response(timeout=5.0)
            assert r is not None and not isinstance(r, X11Error), r
            if r.response_type == 1 and r.sequence == seq:
                n = struct.unpack_from("<H", r.data, 16)[0]
                return r.data[32:32 + n].decode()


def _other_window(xserver):
    owner = _connect(xserver, SANDBOX_COOKIE)
    wid = owner.create_window()
    owner.flush_responses(timeout=0.2)
    return owner, wid


def _inspect(spy, wid):
    """send GetWindowAttributes and a round trip; their sequence numbers"""
    a = spy.send_request(struct.pack("<BxHI", X_GET_WINDOW_ATTRIBUTES, 2, wid))
    b = spy.send_request(struct.pack("<BxH", X_GET_INPUT_FOCUS, 1))
    return a, b


def _outcome(spy, seqs, timeout=5.0):
    """'reply' or the error code of the first request, once the second's
    reply came; None if nothing came"""
    first, last = seqs
    got = None
    while True:
        r = spy.recv_response(timeout=timeout)
        if r is None:
            return None
        if isinstance(r, X11Error) and r.sequence == first:
            got = r.error_code
        elif r.response_type == 1 and r.sequence == first:
            got = "reply"
        elif r.response_type == 1 and r.sequence == last:
            return got


def test_no_agent_refuses_at_once(xserver):
    owner, wid = _other_window(xserver)
    spy = _connect(xserver, SANDBOX_COOKIE)
    assert _outcome(spy, _inspect(spy, wid)) == BAD_ACCESS


def test_allow_once(xserver):
    agent = Agent(xserver)
    assert agent.enable() == []
    owner, wid = _other_window(xserver)
    spy = _connect(xserver, SANDBOX_COOKIE)
    seqs = _inspect(spy, wid)
    (prompt,) = agent.prompts()
    pid, cap, client, target, major, minor = prompt
    assert (cap, target, major) == (CAP_WINDOW_INSPECT, wid, X_GET_WINDOW_ATTRIBUTES)
    assert agent.client_namespace(client) == "sandbox"
    # the client waits, and so do its later requests
    assert _outcome(spy, seqs, timeout=0.5) is None
    assert agent.answer(pid, ALLOW_ONCE) == []
    assert _outcome(spy, seqs) == "reply"
    # asked again next time
    seqs = _inspect(spy, wid)
    (prompt,) = agent.prompts()
    assert agent.answer(prompt[0], DENY_ONCE) == []
    assert _outcome(spy, seqs) == BAD_ACCESS


def test_allow_always(xserver):
    agent = Agent(xserver)
    assert agent.enable() == []
    owner, wid = _other_window(xserver)
    spy = _connect(xserver, SANDBOX_COOKIE)
    seqs = _inspect(spy, wid)
    (prompt,) = agent.prompts()
    assert agent.answer(prompt[0], ALLOW_ALWAYS) == []
    assert _outcome(spy, seqs) == "reply"
    assert _outcome(spy, _inspect(spy, wid)) == "reply"
    assert agent.prompts(timeout=0.3) == []
    # for that client only
    other = _connect(xserver, SANDBOX_COOKIE)
    seqs = _inspect(other, wid)
    (prompt,) = agent.prompts()
    assert agent.answer(prompt[0], DENY_ONCE) == []
    assert _outcome(other, seqs) == BAD_ACCESS


def test_deny_always(xserver):
    agent = Agent(xserver)
    assert agent.enable() == []
    owner, wid = _other_window(xserver)
    spy = _connect(xserver, SANDBOX_COOKIE)
    seqs = _inspect(spy, wid)
    (prompt,) = agent.prompts()
    assert agent.answer(prompt[0], DENY_ALWAYS) == []
    assert _outcome(spy, seqs) == BAD_ACCESS
    assert _outcome(spy, _inspect(spy, wid)) == BAD_ACCESS
    assert agent.prompts(timeout=0.3) == []


def test_agent_that_goes_refuses(xserver):
    agent = Agent(xserver)
    assert agent.enable() == []
    owner, wid = _other_window(xserver)
    spy = _connect(xserver, SANDBOX_COOKIE)
    seqs = _inspect(spy, wid)
    assert len(agent.prompts()) == 1
    agent.c.close()
    assert _outcome(spy, seqs) == BAD_ACCESS
    # nobody to ask now
    assert _outcome(spy, _inspect(spy, wid)) == BAD_ACCESS


def test_disabling_refuses(xserver):
    agent = Agent(xserver)
    assert agent.enable() == []
    owner, wid = _other_window(xserver)
    spy = _connect(xserver, SANDBOX_COOKIE)
    seqs = _inspect(spy, wid)
    assert len(agent.prompts()) == 1
    assert agent.enable(False) == []
    assert _outcome(spy, seqs) == BAD_ACCESS


def test_client_that_goes_while_asked(xserver):
    agent = Agent(xserver)
    assert agent.enable() == []
    owner, wid = _other_window(xserver)
    spy = _connect(xserver, SANDBOX_COOKIE)
    _inspect(spy, wid)
    (prompt,) = agent.prompts()
    spy.close()
    # a late answer is ignored
    assert agent.answer(prompt[0], ALLOW_ALWAYS) == []
    assert _errors_of(_connect(xserver, SANDBOX_COOKIE),
                      struct.pack("<BxH", X_GET_INPUT_FOCUS, 1)) == []


def test_one_agent_and_only_it_answers(xserver):
    agent = Agent(xserver)
    assert agent.enable() == []
    assert Agent(xserver).enable() == [BAD_ACCESS]
    assert agent.enable() == []                     # again: still it
    other = Agent(xserver)
    assert other.answer(1, ALLOW_ALWAYS) == [BAD_ACCESS]
    assert agent.answer(12345, ALLOW_ONCE) == []    # no such prompt: ignored
    assert agent.answer(1, 4) == [BAD_VALUE]
    assert agent.enable(False) == []
    assert other.enable() == []


def test_unconfined_clients_are_not_asked(xserver):
    agent = Agent(xserver)
    assert agent.enable() == []
    owner, wid = _other_window(xserver)
    root = _connect(xserver, ROOT_COOKIE)
    assert _outcome(root, _inspect(root, wid)) == "reply"
    assert agent.prompts(timeout=0.3) == []


class TestAudit:
    """Nothing is refused in audit mode, so nothing is asked."""

    @pytest.fixture
    def capmode(self):
        return "audit"

    def test_not_asked(self, xserver):
        agent = Agent(xserver)
        assert agent.enable() == []
        owner, wid = _other_window(xserver)
        spy = _connect(xserver, SANDBOX_COOKIE)
        assert _outcome(spy, _inspect(spy, wid)) == "reply"
        assert agent.prompts(timeout=0.3) == []


# ---- utils/xprism-permission-agent ----------------------------------

def _agent_tool():
    builddir = os.environ.get("XSERVER_BUILDDIR")
    path = Path(builddir or "", "utils/xprism-permission-agent/xprism-permission-agent")
    if not builddir or not path.exists():
        pytest.skip("xprism-permission-agent not built")
    return str(path)


@pytest.fixture
def agent_tool(xserver, tmp_path):
    """start(ask) runs the agent with that ask command"""
    from test_xns_listen import _xauth_file
    env = dict(os.environ, DISPLAY=f":{xserver.display_num}",
               XAUTHORITY=_xauth_file(tmp_path / "xauth", xserver.display_num))
    procs = []

    def start(ask, check_ready=True):
        p = subprocess.Popen([_agent_tool(), "--verbose", "--ask", ask], env=env,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        procs.append(p)
        if check_ready:
            line = p.stdout.readline()
            if line.strip() != "ready":
                p.kill()
                pytest.fail(f"agent did not get ready: {line!r} {p.communicate(timeout=5)[1]}")
        return p

    yield start
    for p in procs:
        p.terminate()
        p.wait(timeout=5)


class TestAgentTool:
    def test_allow_always(self, xserver, agent_tool, tmp_path):
        seen = tmp_path / "seen"
        agent_tool(f'printf "%s|%s|%s|%s" "$XPRISM_CAPABILITY" "$XPRISM_NAMESPACE" '
                   f'"$XPRISM_PROMPT" "$XPRISM_PID" > {seen}; exit 2')
        owner, wid = _other_window(xserver)
        spy = _connect(xserver, SANDBOX_COOKIE)
        assert _outcome(spy, _inspect(spy, wid)) == "reply"
        cap, ns, prompt, pid = seen.read_text().split("|")
        assert (cap, ns) == ("window.inspect", "sandbox")
        assert "in sandbox wants to read details of other applications' windows" in prompt
        assert pid == str(os.getpid())
        seen.unlink()
        assert _outcome(spy, _inspect(spy, wid)) == "reply"
        assert not seen.exists()                    # not asked again

    @pytest.mark.parametrize("ask", ["exit 1", "exit 3", "exit 7", "kill -9 $$"])
    def test_deny(self, xserver, agent_tool, ask):
        agent_tool(ask)
        owner, wid = _other_window(xserver)
        spy = _connect(xserver, SANDBOX_COOKIE)
        assert _outcome(spy, _inspect(spy, wid)) == BAD_ACCESS

    def test_prompts_in_parallel(self, xserver, agent_tool, tmp_path):
        """A dialog left open does not hold up another client's prompt."""
        gate = tmp_path / "gate"
        agent_tool(f'if [ "$XPRISM_PID" = "" ]; then exit 1; fi; '
                   f'if [ ! -e {gate} ]; then touch {gate}; sleep 30; fi; exit 0')
        owner, wid = _other_window(xserver)
        first, second = _connect(xserver, SANDBOX_COOKIE), _connect(xserver, SANDBOX_COOKIE)
        held = _inspect(first, wid)
        for _ in range(50):
            if gate.exists():
                break
            time.sleep(0.1)
        assert gate.exists()
        assert _outcome(second, _inspect(second, wid)) == "reply"
        assert _outcome(first, held, timeout=0.3) is None

    def test_one_agent(self, xserver, agent_tool):
        agent_tool("exit 0")
        p = agent_tool("exit 0", check_ready=False)
        assert p.wait(timeout=10) == 1
        assert "another permission agent is running" in p.stderr.read()

    def test_needs_the_session_authorization(self, xserver, tmp_path):
        env = dict(os.environ, DISPLAY=f":{xserver.display_num}",
                   XAUTHORITY=str(tmp_path / "missing"))
        r = subprocess.run([_agent_tool(), "--ask", "exit 0"], env=env, capture_output=True,
                           text=True, timeout=10)
        assert r.returncode == 1
        assert "X-NAMESPACE" in r.stderr or "cannot connect" in r.stderr, r.stderr
