"""MCP stdio adapter for the optional in-process GnuCash live-book bridge."""
from __future__ import annotations

import argparse
import base64
import binascii
import datetime as dt
import json
import os
from pathlib import Path
import re
import socket
import struct
from typing import Annotated, Any
from uuid import UUID

from mcp.server import MCPServer
from mcp.server.mcpserver.exceptions import ToolError
from mcp.types import ToolAnnotations
from pydantic import BaseModel, ConfigDict, Field, field_validator

MAX_RESPONSE = 4 * 1024 * 1024
Guid = Annotated[str, Field(pattern=r"^[0-9a-fA-F]{32}$")]
PageOffset = Annotated[int, Field(ge=0, le=1_000_000)]
PageSize = Annotated[int, Field(ge=1, le=1000)]


def default_connection() -> Path:
    if os.name == "nt":
        return Path(os.environ["LOCALAPPDATA"]) / "GnuCash" / "mcp" / "live.json"
    return Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local/state")) / "gnucash/mcp/live.json"


class BridgeError(ToolError):
    """A bounded, non-secret diagnostic safe to return as a tool error."""


class BridgeClient:
    def __init__(self, connection: Path, timeout: float = 15):
        self.connection = connection
        self.timeout = timeout

    @staticmethod
    def _receive(sock: socket.socket, size: int) -> bytes:
        chunks = bytearray()
        while len(chunks) < size:
            chunk = sock.recv(size - len(chunks))
            if not chunk:
                raise BridgeError("GnuCash closed the connection before returning a result. A write may have completed; retry with the SAME request_id.")
            chunks.extend(chunk)
        return bytes(chunks)

    def call(self, method: str, arguments: dict[str, Any]) -> dict[str, Any]:
        try:
            if self.connection.stat().st_size > 4096:
                raise BridgeError("Invalid GnuCash connection file.")
            config = json.loads(self.connection.read_text(encoding="utf-8"))
            if (type(config.get("version")) is not int or config["version"] != 1 or config.get("host") != "127.0.0.1"
                    or type(config.get("port")) is not int or not 1 <= config["port"] <= 65535
                    or not isinstance(config.get("token"), str)
                    or not re.fullmatch(r"[0-9a-f]{64}", config["token"])):
                raise BridgeError("Invalid GnuCash connection file.")
        except FileNotFoundError:
            raise BridgeError("The live bridge is not active. Start GnuCash with --mcp-file first.") from None
        except (OSError, ValueError, AttributeError):
            raise BridgeError("Cannot read the GnuCash connection file.") from None
        request = json.dumps({"token": config["token"], "method": method, "arguments": arguments},
                             ensure_ascii=False, allow_nan=False).encode("utf-8")
        if len(request) > 1024 * 1024:
            raise BridgeError("Request is too large.")
        try:
            with socket.create_connection(("127.0.0.1", config["port"]), timeout=self.timeout) as sock:
                sock.sendall(struct.pack("!I", len(request)) + request)
                size, = struct.unpack("!I", self._receive(sock, 4))
                if not 0 < size <= MAX_RESPONSE:
                    raise BridgeError("Invalid response size from GnuCash.")
                response = json.loads(self._receive(sock, size).decode("utf-8"))
        except (OSError, UnicodeError, ValueError):
            raise BridgeError("GnuCash did not return a result. A write may have completed; read the book or retry with the SAME request_id.") from None
        if not isinstance(response, dict):
            raise BridgeError("Invalid response from GnuCash.")
        if "error" in response:
            raise BridgeError(str(response["error"])[:4096])
        if not isinstance(response.get("result"), dict):
            raise BridgeError("Invalid result from GnuCash.")
        return response["result"]


class Split(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)
    account_guid: Guid
    value: Annotated[str, Field(pattern=r"^-?[0-9]+(?:\.[0-9]{1,9})?$", max_length=30)]
    memo: Annotated[str, Field(max_length=4096)] = ""


class TransactionDraft(BaseModel):
    model_config = ConfigDict(extra="forbid")
    book_token: Annotated[str, Field(min_length=1, max_length=128)]
    request_id: UUID
    date: Annotated[str, Field(pattern=r"^[0-9]{4}-[0-9]{2}-[0-9]{2}$")]
    description: Annotated[str, Field(max_length=4096)]
    currency: Annotated[str, Field(min_length=1, max_length=32)]
    splits: Annotated[list[Split], Field(min_length=2, max_length=100)]

    @field_validator("date")
    @classmethod
    def calendar_date(cls, value: str) -> str:
        date = dt.date.fromisoformat(value)
        if date.year < 1400:
            raise ValueError("Year must be at least 1400.")
        return value


class Statement(BaseModel):
    model_config = ConfigDict(extra="forbid")
    account_guid: Guid
    statement_date: Annotated[str, Field(pattern=r"^[0-9]{4}-[0-9]{2}-[0-9]{2}$")]
    ending_balance: Annotated[str, Field(pattern=r"^-?[0-9]+(?:\.[0-9]{1,9})?$", max_length=30)]

    @field_validator("statement_date")
    @classmethod
    def calendar_date(cls, value: str) -> str:
        return TransactionDraft.calendar_date(value)


class CsvUpload(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)
    settings_name: Annotated[str, Field(min_length=1, max_length=256)]
    csv_base64: Annotated[str, Field(min_length=4, max_length=699052)]
    default_transfer_account_guid: Guid | None = None

    @field_validator("csv_base64")
    @classmethod
    def uploaded_bytes(cls, value: str) -> str:
        try:
            data = base64.b64decode(value, validate=True)
        except (ValueError, binascii.Error):
            raise ValueError("CSV must be canonical base64 of the original uploaded bytes.") from None
        if not data or len(data) > 512 * 1024 or base64.b64encode(data).decode("ascii") != value:
            raise ValueError("CSV upload must contain 1 to 524288 bytes in canonical base64.")
        if b"\0" in data:
            raise ValueError("CSV containing NUL bytes is unsupported; export UTF-8 or the setting's legacy encoding.")
        return value


READ = ToolAnnotations(read_only_hint=True, open_world_hint=False)
WRITE = ToolAnnotations(read_only_hint=False, destructive_hint=True, idempotent_hint=True, open_world_hint=False)


def make_server(bridge: BridgeClient) -> MCPServer:
    server = MCPServer("GnuCash Live", version="0.2.0", instructions=(
        "Works only with the live book in the explicitly enabled GnuCash GUI. "
        "Read get_book and list_accounts first. Amounts are exact decimal strings; read results are rational numbers. "
        "Writes support ordinary same-currency, unreconciled transactions. "
        "Use a unique UUID request_id per intended change and reuse it for retries. "
        "For updates first read the transaction and supply its revision; updates replace ALL splits. "
        "Reconciliation requires preview_reconciliation with the user's statement date and exact ending balance, then its revision. "
        "CSV imports use the original uploaded file bytes as base64 and an exact saved settings name; list settings first. "
        "Never substitute a ChatGPT sandbox path for a Windows path. Upload bytes are passed in the tool argument. "
        "Preview CSV first, resolve errors and exclude potential duplicate rows explicitly. Batch commits may partially persist. "
        "Do not retry a timed-out write with a new ID. Receipts last only for this running bridge/book session. "
        "GnuCash performs its normal backend persistence: XML usually needs GUI Save; SQL may persist immediately."
    ))

    @server.tool(annotations=READ)
    def get_book() -> dict[str, Any]:
        """Get the currently open book identity, session token, dirty and read-only state."""
        return bridge.call("get_book", {})

    @server.tool(annotations=READ)
    def list_accounts(offset: PageOffset = 0, limit: PageSize = 100) -> dict[str, Any]:
        """List live accounts, GUIDs, commodities and exact balances without currency conversion."""
        return bridge.call("list_accounts", {"offset": offset, "limit": limit})

    @server.tool(annotations=READ)
    def list_transactions(offset: PageOffset = 0, limit: PageSize = 100) -> dict[str, Any]:
        """List transaction summaries in date/GUID order. Call get_transaction for splits and revision."""
        return bridge.call("list_transactions", {"offset": offset, "limit": limit})

    @server.tool(annotations=READ)
    def get_transaction(guid: Guid) -> dict[str, Any]:
        """Read a transaction, its exact split values, and optimistic concurrency revision."""
        return bridge.call("get_transaction", {"guid": guid})

    @server.tool(annotations=WRITE)
    def create_transaction(draft: TransactionDraft) -> dict[str, Any]:
        """Create a balanced same-currency transaction in the live GUI book. May persist immediately on SQL."""
        return bridge.call("create_transaction", draft.model_dump(mode="json"))

    @server.tool(annotations=WRITE)
    def update_transaction(guid: Guid, expected_revision: Annotated[str, Field(pattern=r"^[0-9a-f]{64}$")],
                           draft: TransactionDraft) -> dict[str, Any]:
        """Replace date, description and ALL splits of an unprotected transaction, only if its revision matches."""
        return bridge.call("update_transaction", {**draft.model_dump(mode="json"), "guid": guid,
                                                   "expected_revision": expected_revision})

    @server.tool(annotations=READ)
    def preview_reconciliation(statement: Statement) -> dict[str, Any]:
        """Compare the exact account balance through the statement date, without changing split states."""
        return bridge.call("preview_reconciliation", statement.model_dump(mode="json"))

    @server.tool(annotations=WRITE)
    def reconcile_account(statement: Statement, book_token: str,
                          expected_revision: Annotated[str, Field(pattern=r"^[0-9a-f]{64}$")],
                          request_id: UUID) -> dict[str, Any]:
        """Reconcile only n/c splits through the statement date when balance and preview revision still match.

        Preserve later splits, other accounts and existing y/f states. Check completed and changed_splits:
        backend commits are per transaction and a failure may leave a partial result. Never invent statement balances.
        """
        return bridge.call("reconcile_account", {**statement.model_dump(mode="json"), "book_token": book_token,
            "expected_revision": expected_revision, "request_id": str(request_id)})

    @server.tool(annotations=READ)
    def list_csv_import_settings() -> dict[str, Any]:
        """List exact saved CSV setting names for the current GUI book, encoding and base account."""
        return bridge.call("list_csv_import_settings", {})

    @server.tool(annotations=READ)
    def preview_csv_import(upload: CsvUpload) -> dict[str, Any]:
        """Parse an uploaded CSV using its named GnuCash setting, with no transaction creation.

        csv_base64 contains the original file bytes, not a path. Supports at most 512 KiB / 500 two-split,
        same-currency rows. Reports unresolved accounts, precision errors and conservative possible duplicates.
        An explicit default_transfer_account_guid can complete rows without a transfer account column.
        """
        return bridge.call("preview_csv_import", upload.model_dump(mode="json", exclude_none=True))

    @server.tool(annotations=WRITE)
    def import_csv_transactions(upload: CsvUpload, book_token: str,
                                expected_revision: Annotated[str, Field(pattern=r"^[0-9a-f]{64}$")],
                                request_id: UUID,
                                skip_rows: Annotated[list[Annotated[int, Field(ge=1, le=100000)]], Field(max_length=500)] | None = None) -> dict[str, Any]:
        """Import a reviewed CSV preview using unchanged upload bytes and named settings.

        Potential duplicate row numbers must be explicitly listed in skip_rows. No automatic matching or changes
        to existing transactions. Check completed/imported/error: a backend failure can leave a partial batch.
        Reuse the SAME request_id and identical arguments after a timeout. Receipts are session-local.
        """
        return bridge.call("import_csv_transactions", {**upload.model_dump(mode="json", exclude_none=True),
            "book_token": book_token, "expected_revision": expected_revision, "request_id": str(request_id),
            "skip_rows": skip_rows or []})

    return server


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--connection", type=Path, default=default_connection())
    args = parser.parse_args()
    make_server(BridgeClient(args.connection)).run(transport="stdio")


if __name__ == "__main__":
    main()
