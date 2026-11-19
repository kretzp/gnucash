# GnuCash Live MCP

Optional MCP support for the book currently open in the native GnuCash GUI.
The official Python MCP SDK (`mcp==2.3.0`) handles stdio, protocol negotiation
and tool schemas. A native authenticated loopback bridge executes every engine
operation on the GTK main thread. There is no second file/database writer.

## Current status

The native implementation and adapter are built and tested. The user explicitly
approved installation, project Codex registration and live startup on 2026-11-19.
`.codex/config.toml` now registers `gnucash_live`; `codex mcp get` confirms it is
enabled. The original real stdio handshake enumerated six tools and read the live GUI
book status successfully, without modifying book data. The separate
`codex-config.example.toml` remains an example for other installations.

The reconciliation/CSV extension (adapter 0.2.0) was built and installed natively
on 2026-11-19. All **23 focused tests passed**, including actual MCP stdio calls
against synthetic books, exact reconciliation cutoffs, frozen states, metadata
conflicts, zero-net reconciliation, CSV duplicate protection, invalid-row rejection,
UTF-8 and Windows-1252. The native GUI was restarted after the user saved/closed it.
A fresh read-only stdio handshake verified all **11 tools**, live current-book
status and saved CSV presets, with an unchanged clean book. Existing connected
MCP clients may need to reconnect to reload their cached tool list/adapter process.

MCP is disabled during normal GnuCash startup. The optional native build flag
is `-DWITH_MCP=ON` (Boost >= 1.75); the Windows build helper selects it. The
executable only starts the bridge when explicitly given
`--mcp-file <absolute-path-to-private-connection-file>`.

## Local prerequisites and tests

From `C:/repos/gnucash`:

```powershell
python -m venv .windows-build/mcp-venv
.windows-build/mcp-venv/Scripts/python.exe -m pip install -r tools/mcp/requirements.txt
./tools/build-windows.ps1 configure
./tools/build-windows.ps1 build
```

The build environment is documented in `tools/WINDOWS-BUILD.md`. Close native
GnuCash before installing updated binaries. Save pending work in the GUI.

```powershell
.windows-build/mcp-venv/Scripts/python.exe -m pip install pytest
./tools/build-windows.ps1 test-mcp
```

Tests create a fresh **in-memory synthetic book**. They cover native accounting
invariants plus a real stdio initialize/tools/list/call flow, create/update,
revision conflicts, retry deduplication, authentication and oversized frames.
No test opens or changes user financial files. Existing unrelated Windows
locale test failures remain documented in `tools/WINDOWS-BUILD.md`.

## Connection and activation

`./tools/build-windows.ps1 mcp-run` launches the installed GUI with
`--mcp-file %LOCALAPPDATA%/GnuCash/mcp/live.json` using the existing native DLL,
Guile and locale environment. The ordinary `run` action keeps MCP disabled.
The Python adapter defaults to that same file.
It can also accept `--connection <path>` for a separately named instance.

The bridge binds **127.0.0.1 only**, on a random port, and creates a fresh random
session secret. Windows file creation uses an explicit protected ACL granting
access only to the owner, Administrators and SYSTEM. Unix uses mode 0600.
Tokens stay out of MCP configuration and logs. Normal shutdown removes the
connection file. After a crash, ensure no bridge remains before manually
removing a stale file; an existing connection file is never overwritten.

The stdio server does not host HTTP or send book data to a remote service.
Requests/responses and simultaneous connections are bounded. Client timeouts
do not prove a write was cancelled; see the retry contract below.

## Tools

| Tool | Purpose |
|---|---|
| `get_book` | Book identity, session token, read-only/dirty state |
| `list_accounts` | Paginated accounts, GUIDs, commodities, exact balances |
| `list_transactions` | Paginated transaction summaries, date/GUID order |
| `get_transaction` | Full splits, notes, date, currency and revision |
| `create_transaction` | Create a balanced ordinary same-currency transaction |
| `update_transaction` | Replace date, description and **all splits** of an existing transaction |
| `preview_reconciliation` | Compare a statement ending balance and date without modifying the book |
| `reconcile_account` | Mark eligible splits reconciled only against an unchanged matching preview |
| `list_csv_import_settings` | Names, encodings and base accounts of current-book CSV presets |
| `preview_csv_import` | Parse uploaded CSV bytes using an exact preset name, without creating engine objects |
| `import_csv_transactions` | Import a reviewed unchanged CSV plan, excluding explicitly selected rows |

Read `get_book` and `list_accounts` first. Mutations require the returned
`book_token`, invalidated by book close/reopen. Each intended change needs its
own UUID `request_id`; retries reuse the **identical ID and payload**. Receipts
last only for the running bridge/book session and are capped at 1024 changes
or approximately 16 MiB. After a restart or uncertain write, read the book
before generating a new ID.

Updates also need the transaction GUID and `expected_revision` from
`get_transaction`. Stale revisions, concurrent GUI edits, read-only books or
periods, cleared/reconciled/voided/frozen splits, business, investment and
lot-linked transactions are rejected. Split replacement can change split GUIDs;
transaction notes/number are retained. No deletion tool is provided.

Draft example (replace identity values with live read results):

```json
{
  "book_token": "<get_book.book_token>",
  "request_id": "9d200ee8-d626-49c3-9ec2-98cf495e4710",
  "date": "2026-11-18",
  "description": "Example expense",
  "currency": "EUR",
  "splits": [
    {"account_guid": "<bank-guid>", "value": "-12.34", "memo": ""},
    {"account_guid": "<expense-guid>", "value": "12.34", "memo": ""}
  ]
}
```

Amounts are decimal **strings**, never floating-point JSON numbers. Native
validation checks exact balance and account/currency precision before editing.
Reads preserve precision as `{"numerator":"1234","denominator":"100"}`.
This first slice supports one ISO currency per transaction, ordinary accounts,
2–100 splits and at most nine decimal places. It does not add account creation,
multi-currency trading or invoicing.

## Statement reconciliation

`preview_reconciliation` takes `statement: {account_guid, statement_date, ending_balance}`.
The date is ISO YYYY-MM-DD and the balance is an exact decimal string. It includes
the selected account's splits through that date, without descendants or later entries.
The preview returns `matches`, `can_reconcile`, `pending_count`, blockers, exact
balances, `book_token` and `revision`. A matching zero-net statement can still have
pending splits. Supply the user's actual statement balance; never infer it from the book.

`reconcile_account` takes the same statement, book token, `expected_revision` and
UUID `request_id`. It changes only n/c splits to y and sets their reconciliation date
to the statement date. Existing y/f/v states, other accounts, later entries, values,
transaction descriptions and split GUIDs are preserved. On success the account's
last reconciliation date is updated and postponed reconciliation metadata cleared.
Read-only periods, edited transactions/accounts, stale plans, backward statement
dates and more than 2000 eligible splits are blocked before writing.

Engine commits are **per transaction**, not an atomic batch. Inspect `completed`
and `changed_splits`; backend failure can leave earlier changes persisted. Partial
outcomes are receipted, so an identical retry returns the same outcome. Inspect the
book and preview the remaining work before starting a different request.

## Uploaded CSV transactions

The chat client reads the uploaded attachment and supplies its **original bytes as
canonical base64**, plus `settings_name`, in the `upload` argument. No local server
path or ChatGPT sandbox path is required. The MCP server cannot fetch ChatGPT
attachments by filename or attachment ID itself; the host must be able to read the
attachment bytes. Preserve legacy encoding bytes; conversion uses the saved preset.

1. Call `list_csv_import_settings` and use an exact returned name.
2. Call `preview_csv_import` with
   `upload: {settings_name, csv_base64, default_transfer_account_guid?}`.
3. Review the returned rows, account GUIDs, amounts, errors and possible duplicates.
4. Call `import_csv_transactions` with the identical upload, preview's `book_token`
   and `expected_revision`, a UUID `request_id`, and optional `skip_rows` numbers.

The original GnuCash CSV tokenizer, property parser, account mappings and saved
settings handle separators, quoting, charset, dates, decimal separators and header/
footer selection. The parse-only extraction creates no transaction or split objects.
Only ordinary same-currency two-split, single-row transactions are currently supported.
The transfer account must come from the CSV/mapping or an explicitly provided default;
the importer does not guess expense accounts. Description, number, notes, actions
and memos are preserved. Amount/account/currency precision is checked without rounding.
Multi-line/multi-currency, void/reconciliation state imports and investment/business
accounts are rejected. Limits: 512 KiB original bytes, 500 selected rows, nine
decimal places; NUL-containing encodings such as UTF-16 must be exported differently.

Potential duplicates are conservatively flagged for an existing same-account,
same-date, same-amount split or an earlier matching row in the same upload. These
rows must be explicitly excluded with `skip_rows`; there is no silent matching,
clearing or replacement of existing transactions. CSV `UNIQUE_ID` is a grouping
column, not a banking FITID and not a durable deduplication identifier.

Commit reparses the original bytes with current settings and rechecks the preview
revision before writing. `completed`, `imported` (row/GUID pairs) and `error` describe
the outcome. The batch is not atomic: SQL/backend errors can leave an imported
prefix. Receipt replay prevents reimport during the same bridge session. After a
restart, inspect the book and preview again. No tests import into a real user book.

**Persistence follows GnuCash's backend:** SQL may persist immediately on
commit; XML generally remains dirty until GUI Save. The adapter does not call
Save automatically. New writes must be reviewed and authorized through the
MCP client's normal tool controls.
