"""Tests use a fresh in-memory native book, never a user's accounting file."""
import base64
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import time
from uuid import uuid4

import anyio
from mcp.client import Client
from mcp.client.stdio import StdioServerParameters
from pydantic import ValidationError
import pytest

from gnucash_mcp import BridgeClient, BridgeError, CsvUpload, Split, TransactionDraft, make_server

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / ".windows-build/build-gcc15"
PREFIX = ROOT / ".windows-build/mingw15"


@pytest.fixture
def live_book(tmp_path):
    executable = BUILD / "bin/test-live-mcp.exe"
    if not executable.exists():
        pytest.fail("Build the native test-live-mcp target before running integration tests.")
    connection = tmp_path / "connection.json"
    env = {**os.environ, "PATH": f"{BUILD / 'bin'};{PREFIX / 'bin'};{os.environ['PATH']}",
           "LANG": "en_US.UTF-8", "LC_ALL": "en_US.UTF-8"}
    with (tmp_path / "native.log").open("w") as log:
        process = subprocess.Popen([str(executable), "--serve", str(connection)], env=env, stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 15
            while True:
                if process.poll() is not None:
                    pytest.fail(f"Native fixture exited with {process.returncode}; see {tmp_path / 'native.log'}")
                try:
                    BridgeClient(connection).call("get_book", {})
                    break
                except BridgeError:
                    if time.monotonic() > deadline:
                        pytest.fail("Native bridge did not become ready.")
                    time.sleep(0.05)
            yield connection
        finally:
            process.terminate()
            process.wait(timeout=10)


def test_native_engine_invariants():
    env = {**os.environ, "PATH": f"{BUILD / 'bin'};{PREFIX / 'bin'};{os.environ['PATH']}"}
    result = subprocess.run([str(BUILD / "bin/test-live-mcp.exe")], env=env, capture_output=True, text=True, timeout=20)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "invariants passed" in result.stdout


@pytest.mark.parametrize("value", [1.23, "NaN", "1e3", "12,34", "0.0000000001", "--1", ".5"])
def test_amount_schema_rejects_lossy_or_ambiguous_values(value):
    with pytest.raises(ValidationError):
        Split(account_guid="0" * 32, value=value)


def test_missing_connection(tmp_path):
    with pytest.raises(BridgeError, match="not active"):
        BridgeClient(tmp_path / "missing.json").call("get_book", {})


def test_connection_cannot_redirect_to_remote_host(tmp_path):
    connection = tmp_path / "bad.json"
    connection.write_text(json.dumps({"version": 1, "host": "example.com", "port": 80, "token": "0" * 64}))
    with pytest.raises(BridgeError, match="Invalid GnuCash connection"):
        BridgeClient(connection).call("get_book", {})


def test_raw_auth_and_frame_limits(live_book):
    config = json.loads(live_book.read_text())
    payload = json.dumps({"token": "0" * 64, "method": "get_book", "arguments": {}}).encode()
    with socket.create_connection(("127.0.0.1", config["port"]), timeout=5) as sock:
        sock.sendall(struct.pack("!I", len(payload)) + payload)
        size, = struct.unpack("!I", BridgeClient._receive(sock, 4))
        result = json.loads(BridgeClient._receive(sock, size))
        assert result == {"error": "Authentication failed."}
    with socket.create_connection(("127.0.0.1", config["port"]), timeout=5) as sock:
        sock.sendall(struct.pack("!I", 1024 * 1024 + 1))
        assert sock.recv(1) == b""
    assert BridgeClient(live_book).call("list_transactions", {})["total"] == 0


def test_mcp_stdio_read_create_update_retry_and_errors(live_book):
    async def scenario():
        params = StdioServerParameters(command=sys.executable,
            args=[str(Path(__file__).with_name("gnucash_mcp.py")), "--connection", str(live_book)])
        # Force the initialize handshake to exercise compatibility with existing MCP hosts.
        async with Client(params, mode="legacy") as client:
            tools = (await client.list_tools()).tools
            assert {tool.name for tool in tools} == {"get_book", "list_accounts", "list_transactions",
                "get_transaction", "create_transaction", "update_transaction", "preview_reconciliation", "reconcile_account",
                "list_csv_import_settings", "preview_csv_import", "import_csv_transactions"}
            assert next(t for t in tools if t.name == "create_transaction").annotations.destructive_hint

            async def call(name, arguments=None):
                result = await client.call_tool(name, arguments or {})
                assert not result.is_error, result.content
                return result.structured_content

            book = await call("get_book")
            accounts = (await call("list_accounts"))["accounts"]
            assert len(accounts) == 2
            draft = {"book_token": book["book_token"], "request_id": str(uuid4()), "date": "2026-11-18",
                     "description": "MCP synthetic café", "currency": "EUR", "splits": [
                         {"account_guid": accounts[0]["guid"], "value": "-12.34"},
                         {"account_guid": accounts[1]["guid"], "value": "12.34"}]}
            created = await call("create_transaction", {"draft": draft})
            assert created["description"] == draft["description"]
            assert any(split["value"] == {"numerator": "-1234", "denominator": "100"} for split in created["splits"])
            assert await call("create_transaction", {"draft": draft}) == created
            assert (await call("list_transactions"))["total"] == 1
            details = await call("get_transaction", {"guid": created["guid"]})
            changed_draft = {**draft, "request_id": str(uuid4()), "description": "Updated synthetic transaction"}
            update = {"guid": created["guid"], "expected_revision": details["revision"], "draft": changed_draft}
            changed = await call("update_transaction", update)
            assert changed["description"] == changed_draft["description"]
            assert await call("update_transaction", update) == changed
            update["draft"] = {**changed_draft, "request_id": str(uuid4())}
            assert (await client.call_tool("update_transaction", update)).is_error
            bad_draft = {**draft, "request_id": str(uuid4()), "splits": [
                {"account_guid": accounts[0]["guid"], "value": "-12.34"},
                {"account_guid": accounts[1]["guid"], "value": "12.35"}]}
            assert (await client.call_tool("create_transaction", {"draft": bad_draft})).is_error
            assert (await client.call_tool("list_accounts", {"limit": 1001})).is_error
            assert (await call("list_transactions"))["total"] == 1
            assert (await call("get_book"))["dirty"]
    anyio.run(scenario)


@pytest.mark.parametrize("data", ["%%%", "Zg==\n", "", base64.b64encode(b"a\0b").decode(), base64.b64encode(b"a" * (512 * 1024 + 1)).decode()])
def test_csv_upload_rejects_invalid_or_oversize_data(data):
    with pytest.raises(ValidationError):
        CsvUpload(settings_name="test", csv_base64=data)


def test_csv_upload_preview_commit_and_duplicate_guards(live_book):
    async def scenario():
        params = StdioServerParameters(command=sys.executable,
            args=[str(Path(__file__).with_name("gnucash_mcp.py")), "--connection", str(live_book)])
        async with Client(params, mode="legacy") as client:
            async def call(name, args=None):
                result = await client.call_tool(name, args or {})
                assert not result.is_error, result.content
                return result.structured_content

            book = await call("get_book")
            settings = await call("list_csv_import_settings")
            assert any(s["name"] == "MCP synthetic CSV" for s in settings["settings"])
            csv = ('Datum;Text;Betrag;Gegenkonto;Nummer;Notiz;Memo\r\n'
                   '18/11/2026;"Getränke; Café";-12,34;Expense;R42;Beleg;Test\r\n'
                   '19/11/2026;Weitere Ausgabe;-1,66;Expense;R43;;\r\n').encode("utf-8")
            upload = {"settings_name": "MCP synthetic CSV", "csv_base64": base64.b64encode(csv).decode()}
            preview = await call("preview_csv_import", {"upload": upload})
            assert preview["ready"], preview
            assert len(preview["rows"]) == 2
            assert (await call("list_transactions"))["total"] == 0  # preview must create no engine objects
            assert (await call("get_book"))["dirty"] == book["dirty"]
            args = {"upload": upload, "book_token": preview["book_token"],
                    "expected_revision": preview["revision"], "request_id": str(uuid4())}
            stale = {**args, "expected_revision": "0" * 64}
            assert (await client.call_tool("import_csv_transactions", stale)).is_error
            result = await call("import_csv_transactions", args)
            assert result["completed"] and len(result["imported"]) == 2, result
            assert await call("import_csv_transactions", args) == result
            assert (await call("list_transactions"))["total"] == 2
            details = await call("get_transaction", {"guid": result["imported"][0]["guid"]})
            assert details["description"] == "Getränke; Café"
            assert details["notes"] == "Beleg" and details["number"] == "R42"
            preview2 = await call("preview_csv_import", {"upload": upload})
            assert all(row["possible_duplicate"] for row in preview2["rows"])
            args2 = {**args, "request_id": str(uuid4()), "expected_revision": preview2["revision"]}
            assert (await client.call_tool("import_csv_transactions", args2)).is_error
            skipped = await call("import_csv_transactions", {**args2, "skip_rows": [1 + 1, 2 + 1]})
            assert skipped["completed"] and skipped["imported"] == []
            invalid = {**upload, "settings_name": "missing setting"}
            assert (await client.call_tool("preview_csv_import", {"upload": invalid})).is_error
    anyio.run(scenario)


def test_reconciliation_cutoff_zero_net_and_stale_plan(live_book):
    bridge = BridgeClient(live_book)
    book = bridge.call("get_book", {})
    accounts = bridge.call("list_accounts", {})["accounts"]
    bank = next(a for a in accounts if a["name"] == "Bank")
    other = next(a for a in accounts if a["name"] == "Expense")
    def create(day, value, opposite):
        return bridge.call("create_transaction", {"book_token": book["book_token"], "request_id": str(uuid4()),
            "date": day, "description": "reconciliation fixture", "currency": "EUR", "splits": [
                {"account_guid": bank["guid"], "value": value, "memo": ""},
                {"account_guid": other["guid"], "value": opposite, "memo": ""}]})
    first = create("2026-08-30", "10.00", "-10.00")
    second = create("2026-08-31", "-10.00", "10.00")
    future = create("2026-09-01", "5.00", "-5.00")
    statement = {"account_guid": bank["guid"], "statement_date": "2026-08-31", "ending_balance": "0"}
    preview = bridge.call("preview_reconciliation", statement)
    assert preview["matches"] and preview["pending_count"] == 2
    create("2026-08-31", "1.00", "-1.00")
    args = {**statement, "book_token": book["book_token"], "expected_revision": preview["revision"], "request_id": str(uuid4())}
    with pytest.raises(BridgeError, match="changed"):
        bridge.call("reconcile_account", args)
    create("2026-08-31", "-1.00", "1.00")
    args["expected_revision"] = bridge.call("preview_reconciliation", statement)["revision"]
    result = bridge.call("reconcile_account", args)
    assert result["completed"] and len(result["changed_splits"]) == 4
    assert bridge.call("reconcile_account", args) == result
    for txn in (first, second):
        detail = bridge.call("get_transaction", {"guid": txn["guid"]})
        assert all(s["reconcile"] == ("y" if s["account_guid"] == bank["guid"] else "n") for s in detail["splits"])
    assert all(s["reconcile"] == "n" for s in bridge.call("get_transaction", {"guid": future["guid"]})["splits"])


@pytest.mark.parametrize("bad_row", [
    "19/11/2026;precision;-1,234;Expense;;;;",
    "31/02/2026;invalid date;-1,23;Expense;;;;",
    "19/11/2026;unknown account;-1,23;Missing;;;;",
])
def test_csv_valid_prefix_is_not_imported_when_a_later_row_is_invalid(live_book, bad_row):
    bridge = BridgeClient(live_book)
    csv = "date;text;amount;account;number;notes;memo\n18/11/2026;valid;-2,34;Expense;;;\n" + bad_row + "\n"
    upload = {"settings_name": "MCP synthetic CSV", "csv_base64": base64.b64encode(csv.encode()).decode()}
    plan = bridge.call("preview_csv_import", upload)
    assert not plan["ready"], plan
    with pytest.raises(BridgeError, match="errors"):
        bridge.call("import_csv_transactions", {**upload, "book_token": plan["book_token"],
            "expected_revision": plan["revision"], "request_id": str(uuid4())})
    assert bridge.call("list_transactions", {})["total"] == 0


def test_csv_preserves_original_legacy_encoding_bytes(live_book):
    bridge = BridgeClient(live_book)
    csv = "date;text;amount;account;number;notes;memo\n18/11/2026;Getränke Café;-1,23;Expense;;;\n".encode("cp1252")
    plan = bridge.call("preview_csv_import", {"settings_name": "MCP synthetic CP1252",
        "csv_base64": base64.b64encode(csv).decode()})
    assert plan["ready"], plan
    assert plan["rows"][0]["draft"]["description"] == "Getränke Café"
