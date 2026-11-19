# GnuCash Live MCP — Development Release Notes

- Date: 2026-11-19
- Branch: `MCP`
- Python adapter version: `0.2.0`
- Source baseline: GnuCash 5.17 development (`7574033d12`)

This development branch adds an optional Model Context Protocol (MCP) server
for the book currently open in the GnuCash desktop application. It also includes
native Windows development build helpers. This is a fork development build,
not an official GnuCash release or a standalone Windows installer.

## New features

### Live desktop integration

- Read the open book, account balances and transaction details.
- Create balanced transactions and update eligible existing transactions.
- Execute all native book operations on the GTK main thread, using the active
  GUI session rather than opening a second writer on the accounting file.
- Preserve exact financial amounts with decimal-string inputs and rational
  outputs; reject amounts that would require rounding.

### Statement reconciliation

- Preview an account's balance through an explicit statement date and compare
  it with the supplied ending balance.
- Reconcile eligible uncleared/cleared splits only when the balance matches and
  the preview revision is still current.
- Preserve later entries, other accounts' split states, existing reconciled and
  frozen states, monetary values and transaction/split identities.
- Update the account's last reconciliation date and clear postponed
  reconciliation metadata after a successful operation.

### CSV transaction imports

- List and reuse named CSV import settings from the current GUI book.
- Accept original uploaded CSV bytes as base64, preserving the saved setting's
  character encoding, separators, date and number formats, and column mappings.
- Preview parsed transactions without creating transaction or split objects.
- Resolve transfer accounts from CSV data/account mappings or an explicitly
  supplied default transfer account.
- Flag possible duplicates and support explicit row exclusions before import.
- Preserve descriptions, numbers, notes, actions and memos.

## Available MCP tools

| Area | Tools |
|---|---|
| Read | `get_book`, `list_accounts`, `list_transactions`, `get_transaction` |
| Transactions | `create_transaction`, `update_transaction` |
| Reconciliation | `preview_reconciliation`, `reconcile_account` |
| CSV | `list_csv_import_settings`, `preview_csv_import`, `import_csv_transactions` |

## Safety and persistence contracts

- MCP is disabled by default. Build with `-DWITH_MCP=ON` and explicitly launch
  the GUI with `--mcp-file <private-discovery-file>` to enable the native bridge.
- The Python adapter uses the official MCP SDK over stdio. Its native bridge
  binds only to IPv4 loopback, with an ephemeral port and a random session token
  held in a restricted local discovery file.
- Writes require the current book-session token and a unique request ID.
  Identical retries reuse the same ID and arguments. Receipts are bounded and
  last only for the running bridge/book session.
- Updates and batch operations use preview/revision checks to reject stale
  state. Read-only books/periods and concurrent edits are protected.
- Reconciliation and CSV batches are **not atomic across transactions**.
  Inspect completion flags and returned changed/imported items after any error;
  earlier transactions may already have persisted.
- Backend persistence follows normal GnuCash behavior: SQL may save immediately;
  XML books generally require **Save** in the GUI. MCP does not automatically
  save the book.

## Current limits

- Transaction writes support ordinary same-currency accounts, 2–100 splits and
  up to nine decimal places. Replacing a transaction replaces all its splits.
  Protected, lot-linked, investment and business transactions are not supported
  by the transaction replacement operation.
- Reconciliation is limited to 2,000 eligible splits per request and does not
  include descendant accounts automatically.
- CSV imports support single-row, two-split, same-currency transactions, with
  at most 500 selected rows and 512 KiB of original input bytes.
- CSV multi-line transactions, foreign currency, investment/business accounts,
  void states and imported reconciliation states are rejected. NUL-containing
  input, such as UTF-16 exports, must be exported in a supported encoding.
- Possible duplicate CSV rows must be explicitly excluded. Duplicate screening
  is conservative and does not replace the interactive GnuCash import matcher.
- An MCP host must read attachment bytes and supply them to the upload tool.
  The server cannot retrieve a ChatGPT attachment by its name or attachment ID.
- Existing MCP clients may need to reconnect to load the new tool list.

## Native Windows development

`tools/build-windows.ps1` configures, builds, tests, installs and launches this
checkout. The environment separates a private GCC 15-compatible MinGW64 prefix
from MSYS2's CMake/Ninja tools to preserve compatibility with the supplied
WebKit runtime. The GnuCash-patched SWIG 4.2.1-2 is required for this setup.

```powershell
./tools/build-windows.ps1 configure
./tools/build-windows.ps1 build
./tools/build-windows.ps1 test-mcp
# Close GnuCash before replacing installed binaries.
./tools/build-windows.ps1 install
./tools/build-windows.ps1 mcp-run
```

See [Windows build instructions](tools/WINDOWS-BUILD.md) and
[MCP setup and API contracts](tools/mcp/README.md), including Python environment
setup and an example client configuration.

## Validation

- Native Windows application build and development installation verified.
- All **23 focused MCP tests passed**, using synthetic in-memory books and real
  MCP stdio initialization/tool calls. Coverage includes accounting invariants,
  precision, optimistic concurrency, retries, authentication, request bounds,
  statement cutoffs, frozen states, CSV duplicate protection, invalid rows,
  UTF-8 and Windows-1252 input.
- All eleven tools were discovered through a fresh connection to the installed
  GUI; read-only book/status and CSV-setting calls were verified.
- Python compilation and Git whitespace checks passed. A coverage percentage
  and injected SQL-backend failure tests were not measured/run.
- The previously recorded full Windows baseline was **101/129 tests passing**,
  with 28 existing failures, primarily locale-related. This branch does not
  claim the complete upstream Windows suite is green.

No real accounting book is used for automated tests. Local accounting data,
transaction logs, discovery tokens, machine-specific client registration and
build/toolchain artifacts are not part of this source release.
