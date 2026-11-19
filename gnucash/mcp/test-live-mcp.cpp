// SPDX-License-Identifier: GPL-2.0-or-later
#include <config.h>
#include "gnc-mcp.hpp"
#include "qof.h"
#include "cashobjects.h"
#include "Account.h"
#include "Transaction.h"
#include "Split.h"
#include "gnc-commodity.h"
#include "gnc-session.h"
#include "gnc-imp-settings-csv-tx.hpp"
#include <iostream>
#include <stdexcept>

using namespace gnc::mcp;
static void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<typename Function> static void rejects(Function fn, const char* message)
{
    bool rejected = false;
    try { fn(); } catch (const std::exception&) { rejected = true; }
    require(rejected, message);
}
static std::string id(const GncGUID* guid)
{
    auto value = guid_to_string(guid); std::string result(value); g_free(value); return result;
}
int main(int argc, char** argv)
{
    qof_init(); cashobjects_register();
    auto session = qof_session_new(qof_book_new());
    gnc_set_current_session(session);
    auto book = qof_session_get_book(session);
    try
    {
        auto table = gnc_commodity_table_get_table(book);
        auto eur = gnc_commodity_table_lookup(table, "CURRENCY", "EUR");
        if (!eur) eur = gnc_commodity_table_insert(table, gnc_commodity_new(book, "Euro", "CURRENCY", "EUR", "978", 100));
        auto root = xaccMallocAccount(book); xaccAccountSetType(root, ACCT_TYPE_ROOT); gnc_book_set_root_account(book, root);
        auto bank = xaccMallocAccount(book), expense = xaccMallocAccount(book);
        for (auto account : {bank, expense})
        {
            xaccAccountBeginEdit(account); xaccAccountSetType(account, ACCT_TYPE_BANK);
            xaccAccountSetCommodity(account, eur); gnc_account_append_child(root, account); xaccAccountCommitEdit(account);
        }
        xaccAccountSetName(bank, "Bank"); xaccAccountSetName(expense, "Expense");
        CsvTransImpSettings preset;
        preset.m_name = "MCP synthetic CSV";
        preset.m_base_account = bank;
        preset.m_currency_format = 2;
        preset.m_date_format = 1; // dd/mm/yyyy
        preset.m_separators = ";";
        preset.m_skip_start_lines = 1;
        preset.m_column_types = {GncTransPropType::DATE, GncTransPropType::DESCRIPTION,
            GncTransPropType::AMOUNT, GncTransPropType::TACCOUNT, GncTransPropType::NUM,
            GncTransPropType::NOTES, GncTransPropType::MEMO};
        require(!preset.save(), "Synthetic CSV setting could not be saved");
        auto legacy = preset;
        legacy.m_name = "MCP synthetic CP1252";
        legacy.m_encoding = "WINDOWS-1252";
        require(!legacy.save(), "Synthetic legacy CSV setting could not be saved");
        if (argc == 3 && std::string(argv[1]) == "--serve")
        {
            LiveBridge server(argv[2], [book] { return book; });
            auto loop = g_main_loop_new(nullptr, FALSE);
            g_main_loop_run(loop);
            g_main_loop_unref(loop);
            return 0;
        }
        BookApi api;
        rejects([&]{ api.call(nullptr, "get_book", {}); }, "No-book request accepted");
        auto token = api.call(book, "get_book", {}).as_object().at("book_token");
        auto accounts = api.call(book, "list_accounts", {}).as_object();
        require(accounts.at("total").as_int64() == 2, "Account enumeration failed");
        json::object draft{{"book_token", token}, {"request_id", "create-1"}, {"date", "2026-11-18"},
            {"description", "MCP synthetic test"}, {"currency", "EUR"}, {"splits", json::array{
                json::object{{"account_guid", id(xaccAccountGetGUID(bank))}, {"value", "-12.34"}, {"memo", "test"}},
                json::object{{"account_guid", id(xaccAccountGetGUID(expense))}, {"value", "12.34"}, {"memo", "test"}}}}};
        auto invalid = draft; invalid["book_token"] = "stale-book";
        rejects([&]{ api.call(book, "create_transaction", invalid); }, "Wrong book accepted");
        invalid = draft; invalid.at("splits").as_array()[1].as_object()["value"] = "12.35";
        rejects([&]{ api.call(book, "create_transaction", invalid); }, "Imbalance accepted");
        invalid = draft; invalid.at("splits").as_array()[0].as_object()["value"] = "-12.345";
        invalid.at("splits").as_array()[1].as_object()["value"] = "12.345";
        rejects([&]{ api.call(book, "create_transaction", invalid); }, "Currency precision loss accepted");
        invalid = draft; invalid["date"] = "2026-02-30";
        rejects([&]{ api.call(book, "create_transaction", invalid); }, "Invalid date accepted");
        invalid = draft; invalid.at("splits").as_array()[0].as_object()["value"] = "999999999999999999999999";
        rejects([&]{ api.call(book, "create_transaction", invalid); }, "Numeric overflow accepted");
        xaccAccountSetPlaceholder(expense, TRUE);
        rejects([&]{ api.call(book, "create_transaction", draft); }, "Placeholder account accepted");
        xaccAccountSetPlaceholder(expense, FALSE);
        xaccAccountSetType(expense, ACCT_TYPE_RECEIVABLE);
        rejects([&]{ api.call(book, "create_transaction", draft); }, "Business account accepted");
        xaccAccountSetType(expense, ACCT_TYPE_EXPENSE);
        require(api.call(book, "list_transactions", {}).as_object().at("total").as_uint64() == 0, "Rejected drafts mutated the book");
        auto created = api.call(book, "create_transaction", draft).as_object();
        require(api.call(book, "create_transaction", draft) == created, "Idempotent retry failed");
        require(api.call(book, "list_transactions", {}).as_object().at("total").as_uint64() == 1, "Retry duplicated a transaction");
        invalid = draft; invalid["description"] = "different";
        rejects([&]{ api.call(book, "create_transaction", invalid); }, "request_id reused for different payload");
        auto update = draft; update["request_id"] = "update-1"; update["guid"] = created.at("guid");
        update["expected_revision"] = created.at("revision"); update["description"] = "Edited by MCP";
        auto changed = api.call(book, "update_transaction", update).as_object();
        require(changed.at("description") == "Edited by MCP", "Update failed");
        update["request_id"] = "stale-update";
        rejects([&]{ api.call(book, "update_transaction", update); }, "Stale revision accepted");
        GncGUID txn_guid; string_to_guid(std::string(changed.at("guid").as_string()).c_str(), &txn_guid);
        auto txn = xaccTransLookup(&txn_guid, book);
        update["expected_revision"] = changed.at("revision"); update["request_id"] = "open-update";
        xaccTransBeginEdit(txn);
        rejects([&]{ api.call(book, "update_transaction", update); }, "GUI-edited transaction accepted");
        xaccTransRollbackEdit(txn);
        json::object statement{{"account_guid", id(xaccAccountGetGUID(bank))},
            {"statement_date", "2026-11-18"}, {"ending_balance", "-12.34"}};
        auto preview = api.call(book, "preview_reconciliation", statement).as_object();
        require(preview.at("matches").as_bool(), "Statement balance did not match");
        auto reconciliation = statement;
        reconciliation["book_token"] = token;
        reconciliation["request_id"] = "reconcile-1";
        reconciliation["expected_revision"] = preview.at("revision");
        auto wrong = reconciliation; wrong["ending_balance"] = "0";
        rejects([&]{ api.call(book, "reconcile_account", wrong); }, "Mismatching statement accepted");
        xaccTransBeginEdit(txn);
        rejects([&]{ api.call(book, "reconcile_account", reconciliation); }, "Open transaction reconciled");
        xaccTransRollbackEdit(txn);
        auto reconciled = api.call(book, "reconcile_account", reconciliation);
        require(reconciled.as_object().at("completed").as_bool(), "Reconciliation failed");
        require(api.call(book, "reconcile_account", reconciliation) == reconciled, "Reconciliation retry failed");
        for (auto node = xaccTransGetSplitList(txn); node; node = node->next)
        {
            auto split = static_cast<Split*>(node->data);
            require(xaccSplitGetReconcile(split) == (xaccSplitGetAccount(split) == bank ? YREC : NREC),
                    "Reconciliation changed the wrong account");
        }
        xaccTransBeginEdit(txn); xaccSplitSetReconcile(xaccTransGetSplit(txn, 0), YREC); xaccTransCommitEdit(txn);
        auto protected_txn = api.call(book, "get_transaction", {{"guid", changed.at("guid")}}).as_object();
        update["expected_revision"] = protected_txn.at("revision"); update["request_id"] = "protected-update";
        rejects([&]{ api.call(book, "update_transaction", update); }, "Reconciled transaction accepted");
        auto zero = draft;
        zero["request_id"] = "frozen-zero";
        zero.at("splits").as_array()[0].as_object()["value"] = "0";
        zero.at("splits").as_array()[1].as_object()["value"] = "0";
        auto frozen = api.call(book, "create_transaction", zero).as_object();
        GncGUID frozen_id; string_to_guid(std::string(frozen.at("guid").as_string()).c_str(), &frozen_id);
        auto frozen_txn = xaccTransLookup(&frozen_id, book);
        Split* frozen_split = nullptr;
        xaccTransBeginEdit(frozen_txn);
        for (auto node = xaccTransGetSplitList(frozen_txn); node; node = node->next)
        {
            auto split = static_cast<Split*>(node->data);
            if (xaccSplitGetAccount(split) == bank)
            { frozen_split = split; xaccSplitSetReconcile(split, FREC); xaccSplitSetDateReconciledSecs(split, 123456); }
        }
        xaccTransCommitEdit(frozen_txn);
        auto current_plan = api.call(book, "preview_reconciliation", statement).as_object();
        reconciliation["request_id"] = "postponed-reconcile";
        reconciliation["expected_revision"] = current_plan.at("revision");
        xaccAccountSetReconcilePostponeDate(bank, 456789);
        xaccAccountSetReconcilePostponeBalance(bank, gnc_numeric_create(1234, 100));
        rejects([&]{ api.call(book, "reconcile_account", reconciliation); }, "Changed postponement metadata accepted");
        reconciliation["expected_revision"] = api.call(book, "preview_reconciliation", statement).as_object().at("revision");
        require(api.call(book, "reconcile_account", reconciliation).as_object().at("completed").as_bool(), "Postponed reconciliation failed");
        time64 postponed = 0;
        require(!xaccAccountGetReconcilePostponeDate(bank, &postponed), "Postponement not cleared");
        require(frozen_split && xaccSplitGetReconcile(frozen_split) == FREC && xaccSplitGetDateReconciled(frozen_split) == 123456,
                "Frozen reconciliation metadata changed");
        qof_book_mark_readonly(book); draft["request_id"] = "readonly-create";
        rejects([&]{ api.call(book, "create_transaction", draft); }, "Read-only book accepted");
        require(!api.call(book, "preview_reconciliation", statement).as_object().at("can_reconcile").as_bool(), "Read-only reconciliation preview accepted");
        api.invalidate();
        require(api.call(book, "get_book", {}).as_object().at("book_token") != token, "Book reopening did not invalidate token");
        std::cout << "Live MCP engine invariants passed\n";
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; gnc_clear_current_session(); qof_close(); return 1; }
    gnc_clear_current_session(); qof_close(); return 0;
}
