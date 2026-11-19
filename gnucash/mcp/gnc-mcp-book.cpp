// SPDX-License-Identifier: GPL-2.0-or-later
#include <config.h>
#include "gnc-mcp.hpp"
#include "gnc-mcp-private.hpp"
#include "Account.h"
#include "Transaction.h"
#include "Split.h"
#include "gnc-commodity.h"
#include "gnc-numeric.h"
#include "guid.h"
#include "qof.h"
#include <algorithm>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <vector>

namespace gnc::mcp
{
namespace detail
{
[[noreturn]] void fail(const char* message) { throw std::runtime_error(message); }
std::string str(const char* value) { return value ? value : ""; }
std::string text(const json::object& obj, const char* key, size_t maximum)
{
    auto value = obj.if_contains(key);
    if (!value || !value->is_string()) fail("Missing or invalid string argument.");
    std::string result(value->as_string());
    if (result.size() > maximum || result.find('\0') != std::string::npos ||
        !g_utf8_validate(result.data(), result.size(), nullptr)) fail("Invalid text argument.");
    return result;
}
std::string guid(const GncGUID* value)
{
    char buffer[GUID_ENCODING_LENGTH + 1];
    guid_to_string_buff(value, buffer);
    return buffer;
}
GncGUID parse_guid(const std::string& value)
{
    GncGUID result;
    if (value.size() != GUID_ENCODING_LENGTH || !string_to_guid(value.c_str(), &result))
        fail("Invalid GnuCash GUID.");
    return result;
}
json::object number(gnc_numeric value)
{
    if (gnc_numeric_check(value)) fail("Invalid numeric value in book.");
    return {{"numerator", std::to_string(gnc_numeric_num(value))},
            {"denominator", std::to_string(gnc_numeric_denom(value))}};
}
gnc_numeric decimal(const std::string& value)
{
    if (value.empty() || value.size() > 30) fail("Amount must be an exact decimal string.");
    size_t pos = value[0] == '-' ? 1 : 0;
    bool point = false, digit = false;
    int fraction = 0;
    int64_t num = 0, den = 1;
    for (; pos < value.size(); ++pos)
    {
        char c = value[pos];
        if (c == '.' && !point && digit && pos + 1 < value.size()) { point = true; continue; }
        if (c < '0' || c > '9') fail("Amount must use digits and a decimal point, without exponent or separators.");
        digit = true;
        if (num > (std::numeric_limits<int64_t>::max() - (c - '0')) / 10)
            fail("Amount exceeds the numeric range.");
        num = num * 10 + c - '0';
        if (point) { if (++fraction > 9) fail("At most nine decimal places are supported."); den *= 10; }
    }
    if (!digit) fail("Invalid decimal amount.");
    return gnc_numeric_create(value[0] == '-' ? -num : num, den);
}
GDate date(const std::string& value)
{
    if (value.size() != 10 || value[4] != '-' || value[7] != '-') fail("Date must be YYYY-MM-DD.");
    for (size_t i = 0; i < value.size(); ++i)
        if (i != 4 && i != 7 && (value[i] < '0' || value[i] > '9')) fail("Invalid date.");
    auto year = std::stoi(value.substr(0, 4)), month = std::stoi(value.substr(5, 2)), day = std::stoi(value.substr(8, 2));
    if (year < 1400 || year > 9999 || !g_date_valid_dmy(day, static_cast<GDateMonth>(month), year)) fail("Invalid calendar date.");
    GDate result;
    g_date_clear(&result, 1);
    g_date_set_dmy(&result, day, static_cast<GDateMonth>(month), year);
    return result;
}
std::string date_string(Transaction* txn)
{
    auto d = xaccTransGetDatePostedGDate(txn);
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "%04u-%02u-%02u", g_date_get_year(&d), g_date_get_month(&d), g_date_get_day(&d));
    return buffer;
}
json::object transaction(Transaction* txn, bool details)
{
    auto currency = xaccTransGetCurrency(txn);
    json::object result{{"guid", guid(xaccTransGetGUID(txn))}, {"date", date_string(txn)},
        {"description", str(xaccTransGetDescription(txn))}, {"notes", str(xaccTransGetNotes(txn))},
        {"number", str(xaccTransGetNum(txn))},
        {"currency", currency ? str(gnc_commodity_get_mnemonic(currency)) : ""},
        {"read_only", xaccTransGetReadOnly(txn) != nullptr || xaccTransIsReadonlyByPostedDate(txn)}};
    json::array splits;
    for (auto node = xaccTransGetSplitList(txn); node; node = node->next)
    {
        auto split = static_cast<Split*>(node->data);
        auto acct = xaccSplitGetAccount(split);
        splits.push_back(json::object{{"guid", guid(xaccSplitGetGUID(split))},
            {"account_guid", acct ? guid(xaccAccountGetGUID(acct)) : ""},
            {"value", number(xaccSplitGetValue(split))}, {"amount", number(xaccSplitGetAmount(split))},
            {"memo", str(xaccSplitGetMemo(split))}, {"action", str(xaccSplitGetAction(split))},
            {"reconcile", std::string(1, xaccSplitGetReconcile(split))}});
        splits.back().as_object()["reconcile_date"] = std::to_string(xaccSplitGetDateReconciled(split));
    }
    result["splits"] = std::move(splits);
    auto serialized = json::serialize(result);
    auto checksum = g_compute_checksum_for_string(G_CHECKSUM_SHA256, serialized.c_str(), serialized.size());
    result["revision"] = checksum;
    g_free(checksum);
    if (!details) result.erase("splits");
    return result;
}
static int page_argument(const json::object& args, const char* key, int fallback, int maximum)
{
    auto value = args.if_contains(key);
    if (!value) return fallback;
    if (!value->is_int64() || value->as_int64() < 0 || value->as_int64() > maximum)
        fail("Invalid pagination argument.");
    return static_cast<int>(value->as_int64());
}
struct DraftSplit { Account* account; gnc_numeric value; std::string memo; std::string action; };
bool ordinary_account(Account* account)
{
    if (!account) return false;
    switch (xaccAccountGetType(account))
    {
    case ACCT_TYPE_BANK: case ACCT_TYPE_CASH: case ACCT_TYPE_ASSET:
    case ACCT_TYPE_CREDIT: case ACCT_TYPE_LIABILITY: case ACCT_TYPE_INCOME:
    case ACCT_TYPE_EXPENSE: case ACCT_TYPE_EQUITY: return true;
    default: return false;
    }
}

struct Reconciliation
{
    Account* account;
    time64 date;
    std::vector<Split*> pending;
    json::object preview;
};

static Reconciliation reconciliation(QofBook* book, const json::object& args)
{
    auto account_id = parse_guid(text(args, "account_guid", 32));
    auto account = xaccAccountLookup(&account_id, book);
    if (!ordinary_account(account) || xaccAccountGetPlaceholder(account) ||
        !xaccAccountGetCommodity(account) || !gnc_commodity_is_currency(xaccAccountGetCommodity(account)))
        fail("Reconciliation requires an ordinary non-placeholder currency account.");
    auto day = date(text(args, "statement_date", 10));
    auto end = gnc_dmy2time64_end(g_date_get_day(&day), g_date_get_month(&day), g_date_get_year(&day));
    auto expected = decimal(text(args, "ending_balance", 30));
    if (gnc_numeric_check(gnc_numeric_convert(expected, xaccAccountGetCommoditySCU(account), GNC_HOW_RND_NEVER)))
        fail("Statement balance exceeds account precision.");
    Reconciliation plan{account, end, {}, {}};
    auto balance = gnc_numeric_zero(), settled = gnc_numeric_zero();
    json::array snapshot, blockers;
    time64 last = 0;
    xaccAccountGetReconcileLastDate(account, &last);
    time64 postponed_date = 0;
    auto postponed_balance = gnc_numeric_zero();
    bool has_postponed_date = xaccAccountGetReconcilePostponeDate(account, &postponed_date);
    bool has_postponed_balance = xaccAccountGetReconcilePostponeBalance(account, &postponed_balance);
    if (qof_instance_get_editlevel(QOF_INSTANCE(account))) blockers.push_back("Account is being edited.");
    if (last > end) blockers.push_back("Statement predates the account's last reconciliation.");
    std::vector<Split*> ordered;
    for (auto node = xaccAccountGetSplitList(account); node; node = node->next)
        ordered.push_back(static_cast<Split*>(node->data));
    std::sort(ordered.begin(), ordered.end(), [](Split* a, Split* b) {
        return guid_compare(xaccSplitGetGUID(a), xaccSplitGetGUID(b)) < 0;
    });
    for (auto split : ordered)
    {
        auto txn = xaccSplitGetParent(split);
        if (!txn) fail("Account contains a split without a transaction.");
        auto posted = xaccTransGetDatePostedGDate(txn);
        if (g_date_compare(&posted, &day) > 0) continue;
        balance = gnc_numeric_add(balance, xaccSplitGetAmount(split), GNC_DENOM_AUTO, GNC_HOW_DENOM_EXACT);
        auto flag = xaccSplitGetReconcile(split);
        if (flag == YREC || flag == FREC)
            settled = gnc_numeric_add(settled, xaccSplitGetAmount(split), GNC_DENOM_AUTO, GNC_HOW_DENOM_EXACT);
        else if (flag == NREC || flag == CREC)
        {
            plan.pending.push_back(split);
            if (xaccTransIsOpen(txn) || xaccTransGetReadOnly(txn) || xaccTransIsReadonlyByPostedDate(txn))
                blockers.push_back("A candidate transaction is being edited or is read-only.");
        }
        else if (flag != VREC) fail("Unknown reconciliation state.");
        snapshot.push_back(json::object{{"split", guid(xaccSplitGetGUID(split))},
            {"transaction_revision", transaction(txn, true).at("revision")}, {"editing", static_cast<bool>(xaccTransIsOpen(txn))}});
    }
    if (plan.pending.size() > 2000) blockers.push_back("More than 2000 candidate splits; use a smaller statement period.");
    if (qof_book_is_readonly(book) || !qof_book_is_open(book)) blockers.push_back("Book is read-only or closed.");
    auto matches = gnc_numeric_equal(balance, expected);
    json::object state{{"account_guid", text(args, "account_guid", 32)}, {"statement_date", text(args, "statement_date", 10)},
        {"ending_balance", number(expected)}, {"last_reconcile_date", std::to_string(last)},
        {"postponed_date", std::to_string(postponed_date)}, {"has_postponed_date", has_postponed_date},
        {"postponed_balance", number(postponed_balance)}, {"has_postponed_balance", has_postponed_balance},
        {"scu", xaccAccountGetCommoditySCU(account)}, {"splits", std::move(snapshot)}, {"blockers", blockers}};
    auto serialized = json::serialize(state);
    auto checksum = g_compute_checksum_for_string(G_CHECKSUM_SHA256, serialized.c_str(), serialized.size());
    plan.preview = {{"balance", number(balance)}, {"settled_balance", number(settled)},
        {"ending_balance", number(expected)}, {"pending_count", plan.pending.size()}, {"matches", static_cast<bool>(matches)},
        {"can_reconcile", matches && blockers.empty()}, {"blockers", std::move(blockers)}, {"revision", checksum}};
    g_free(checksum);
    return plan;
}

static json::object commit_reconciliation(QofBook* book, const json::object& args)
{
    auto plan = reconciliation(book, args);
    if (plan.preview.at("revision").as_string() != text(args, "expected_revision", 64)) fail("Reconciliation plan changed; preview it again.");
    if (!plan.preview.at("can_reconcile").as_bool()) fail("Statement balance does not match or reconciliation is blocked.");
    // Engine commits are per transaction. Report and receipt partial outcomes instead of implying a batch rollback.
    std::vector<Transaction*> transactions;
    for (auto split : plan.pending)
    {
        auto txn = xaccSplitGetParent(split);
        if (std::find(transactions.begin(), transactions.end(), txn) == transactions.end()) transactions.push_back(txn);
    }
    json::array changed;
    for (auto txn : transactions)
    {
        xaccTransBeginEdit(txn);
        std::vector<GncGUID> ids;
        for (auto split : plan.pending)
            if (xaccSplitGetParent(split) == txn)
            {
                ids.push_back(*xaccSplitGetGUID(split));
                xaccSplitSetReconcile(split, YREC);
                xaccSplitSetDateReconciledSecs(split, plan.date);
            }
        xaccTransCommitEdit(txn);
        bool retained = true;
        for (const auto& id : ids)
        {
            auto split = xaccSplitLookup(&id, book);
            if (!split || xaccSplitGetReconcile(split) != YREC || xaccSplitGetDateReconciled(split) != plan.date) retained = false;
            else changed.push_back(json::value(guid(&id)));
        }
        if (!retained) return {{"completed", false}, {"changed_splits", changed},
            {"error", "Engine commit verification failed. Inspect the book before continuing; earlier transactions may be persisted."}};
    }
    xaccAccountBeginEdit(plan.account);
    xaccAccountSetReconcileLastDate(plan.account, plan.date);
    xaccAccountClearReconcilePostpone(plan.account);
    xaccAccountCommitEdit(plan.account);
    time64 retained_date = 0;
    time64 postponed = 0;
    auto postponed_balance = gnc_numeric_zero();
    bool completed = xaccAccountGetReconcileLastDate(plan.account, &retained_date) && retained_date == plan.date &&
        !xaccAccountGetReconcilePostponeDate(plan.account, &postponed) &&
        !xaccAccountGetReconcilePostponeBalance(plan.account, &postponed_balance);
    return {{"completed", completed}, {"changed_splits", changed}, {"statement_date", text(args, "statement_date", 10)},
        {"balance", plan.preview.at("balance")}};
}
}

using namespace detail;

void BookApi::invalidate() { m_book = nullptr; m_token.clear(); m_receipts.clear(); m_receipt_bytes = 0; }

json::value BookApi::call(QofBook* book, const std::string& method, const json::object& args)
{
    if (!book || qof_book_shutting_down(book)) fail("No book is open in GnuCash.");
    if (m_book != book)
    {
        invalidate(); m_book = book;
        auto nonce = g_uuid_string_random();
        m_token = guid(qof_book_get_guid(book)) + ":" + nonce;
        g_free(nonce);
    }
    if (method == "get_book")
        return json::object{{"book_token", m_token}, {"book_guid", guid(qof_book_get_guid(book))},
            {"read_only", static_cast<bool>(qof_book_is_readonly(book))},
            {"dirty", static_cast<bool>(qof_book_session_not_saved(book))}};
    if (method == "list_accounts")
    {
        json::array rows;
        auto accounts = gnc_account_get_descendants(gnc_book_get_root_account(book));
        auto offset = page_argument(args, "offset", 0, 1000000), limit = page_argument(args, "limit", 100, 1000);
        int index = 0;
        for (auto node = accounts; node; node = node->next, ++index)
        {
            if (index < offset || index >= offset + limit) continue;
            auto acct = static_cast<Account*>(node->data);
            auto name = gnc_account_get_full_name(acct);
            auto commodity = xaccAccountGetCommodity(acct);
            rows.push_back(json::object{{"guid", guid(xaccAccountGetGUID(acct))}, {"name", str(name)},
                {"commodity", commodity ? str(gnc_commodity_get_mnemonic(commodity)) : ""},
                {"type", static_cast<int>(xaccAccountGetType(acct))},
                {"placeholder", static_cast<bool>(xaccAccountGetPlaceholder(acct))},
                {"balance", number(xaccAccountGetBalance(acct))}});
            g_free(name);
        }
        g_list_free(accounts);
        return json::object{{"book_token", m_token}, {"accounts", std::move(rows)}, {"total", index}};
    }
    if (method == "get_transaction")
    {
        auto id = parse_guid(text(args, "guid", 32));
        auto txn = xaccTransLookup(&id, book);
        if (!txn) fail("Transaction not found in the current book.");
        return transaction(txn, true);
    }
    if (method == "list_transactions")
    {
        std::vector<Transaction*> txns;
        auto collection = qof_book_get_collection(book, GNC_ID_TRANS);
        qof_collection_foreach(collection, [](QofInstance* item, gpointer data) {
            static_cast<std::vector<Transaction*>*>(data)->push_back(GNC_TRANSACTION(item));
        }, &txns);
        std::sort(txns.begin(), txns.end(), [](Transaction* a, Transaction* b) {
            auto ad = xaccTransGetDate(a), bd = xaccTransGetDate(b);
            return ad == bd ? guid_compare(xaccTransGetGUID(a), xaccTransGetGUID(b)) < 0 : ad < bd;
        });
        auto offset = page_argument(args, "offset", 0, 1000000), limit = page_argument(args, "limit", 100, 1000);
        json::array rows;
        for (size_t i = offset; i < txns.size() && i < static_cast<size_t>(offset + limit); ++i)
            rows.push_back(transaction(txns[i], false));
        return json::object{{"book_token", m_token}, {"transactions", std::move(rows)}, {"total", txns.size()}};
    }
    if (method == "preview_reconciliation")
    {
        auto result = reconciliation(book, args).preview;
        result["book_token"] = m_token;
        return result;
    }
    if (method == "list_csv_import_settings") return csv_settings(book);
    if (method == "preview_csv_import")
    {
        auto result = csv_preview(book, args);
        result["book_token"] = m_token;
        return result;
    }
    bool updating = method == "update_transaction";
    bool reconciling = method == "reconcile_account";
    bool importing = method == "import_csv_transactions";
    if (!updating && !reconciling && !importing && method != "create_transaction") fail("Unknown operation.");
    if (text(args, "book_token", 128) != m_token) fail("The open book changed; read get_book again.");
    if (qof_book_is_readonly(book) || !qof_book_is_open(book)) fail("The current book is read-only or closed.");
    auto request_id = text(args, "request_id", 128);
    if (request_id.empty()) fail("A request_id is required for safe retries.");
    auto request = method + json::serialize(args);
    auto previous = m_receipts.find(request_id);
    if (previous != m_receipts.end())
    {
        if (previous->second.request != request) fail("request_id was already used for a different operation.");
        return previous->second.result;
    }
    if (m_receipts.size() >= 1024 || m_receipt_bytes >= 16 * 1024 * 1024)
        fail("This bridge session reached its mutation limit; restart the bridge before making more changes.");
    if (reconciling)
    {
        auto result = commit_reconciliation(book, args);
        m_receipt_bytes += request.size() + json::serialize(result).size();
        m_receipts.emplace(request_id, Receipt{request, result});
        return result;
    }
    if (importing)
    {
        auto plan = csv_preview(book, args);
        if (plan.at("revision").as_string() != text(args, "expected_revision", 64)) fail("CSV plan changed; preview again before importing.");
        if (!plan.at("ready").as_bool()) fail("CSV plan contains errors or the book is read-only.");
        std::vector<size_t> skip;
        if (auto value = args.if_contains("skip_rows"))
        {
            if (!value->is_array()) fail("skip_rows must be an array of row numbers.");
            for (const auto& row : value->as_array())
            {
                if (!row.is_int64() || row.as_int64() < 1 || row.as_int64() > 100000) fail("Invalid skipped row.");
                skip.push_back(static_cast<size_t>(row.as_int64()));
            }
        }
        size_t pending = 0;
        for (const auto& row : plan.at("rows").as_array())
        {
            auto index = row.as_object().at("row").as_uint64();
            if (std::find(skip.begin(), skip.end(), index) != skip.end()) continue;
            if (row.as_object().at("possible_duplicate").as_bool()) fail("Potential duplicate CSV rows must be explicitly excluded with skip_rows.");
            ++pending;
        }
        if (m_receipts.size() + pending + 1 > 1024) fail("Insufficient receipt capacity for this CSV batch; restart the bridge.");
        json::array imported;
        bool completed = true;
        std::string error;
        for (const auto& row : plan.at("rows").as_array())
        {
            auto index = row.as_object().at("row").as_uint64();
            if (std::find(skip.begin(), skip.end(), index) != skip.end()) continue;
            auto draft = row.as_object().at("draft").as_object();
            draft["book_token"] = m_token;
            draft["request_id"] = request_id + ":csv:" + std::to_string(index);
            try
            {
                auto created = call(book, "create_transaction", draft).as_object();
                imported.push_back(json::object{{"row", index}, {"guid", created.at("guid")}});
            }
            catch (const std::exception& failure)
            {
                completed = false;
                error = "CSV batch stopped at row " + std::to_string(index) + ": " + str(failure.what()) +
                    " Earlier rows may be persisted; inspect the book before a new attempt.";
                break;
            }
        }
        json::object result{{"completed", completed}, {"imported", imported}, {"error", error}};
        m_receipt_bytes += request.size() + json::serialize(result).size();
        m_receipts.emplace(request_id, Receipt{request, result});
        return result;
    }
    Transaction* txn = nullptr;
    if (updating)
    {
        auto id = parse_guid(text(args, "guid", 32));
        txn = xaccTransLookup(&id, book);
        if (!txn) fail("Transaction not found.");
        if (text(args, "expected_revision", 64) != transaction(txn, true).at("revision").as_string())
            fail("Transaction changed; read it again before updating.");
        if (xaccTransIsOpen(txn) || xaccTransGetReadOnly(txn) || xaccTransIsReadonlyByPostedDate(txn))
            fail("Transaction is being edited or is read-only.");
        if (xaccTransGetTxnType(txn) != TXN_TYPE_NONE) fail("Business transactions are not supported.");
        for (auto node = xaccTransGetSplitList(txn); node; node = node->next)
        {
            auto split = static_cast<Split*>(node->data);
            if (xaccSplitGetLot(split) || xaccSplitGetReconcile(split) != NREC)
                fail("Cleared, reconciled, voided, frozen or lot-linked transactions cannot be replaced.");
            if (!ordinary_account(xaccSplitGetAccount(split)) ||
                !gnc_commodity_equal(xaccAccountGetCommodity(xaccSplitGetAccount(split)), xaccTransGetCurrency(txn)))
                fail("Only ordinary same-currency transactions can be replaced.");
        }
    }
    auto posted = date(text(args, "date", 10));
    if (qof_book_uses_autoreadonly(book) && g_date_compare(&posted, qof_book_get_autoreadonly_gdate(book)) < 0)
        fail("The date is in a read-only accounting period.");
    auto description = text(args, "description");
    auto currency = gnc_commodity_table_lookup(gnc_commodity_table_get_table(book), "CURRENCY", text(args, "currency", 32).c_str());
    if (!currency) fail("Currency not found in the book.");
    auto value = args.if_contains("splits");
    if (!value || !value->is_array() || value->as_array().size() < 2 || value->as_array().size() > 100)
        fail("A transaction needs between two and one hundred splits.");
    std::vector<DraftSplit> drafts;
    auto total = gnc_numeric_zero();
    for (const auto& entry : value->as_array())
    {
        if (!entry.is_object()) fail("Invalid split object.");
        const auto& item = entry.as_object();
        auto id = parse_guid(text(item, "account_guid", 32));
        auto account = xaccAccountLookup(&id, book);
        if (!ordinary_account(account) || xaccAccountGetPlaceholder(account) || !gnc_commodity_equal(xaccAccountGetCommodity(account), currency))
            fail("Splits must reference non-placeholder accounts in the transaction currency.");
        auto amount = decimal(text(item, "value", 30));
        auto account_amount = gnc_numeric_convert(amount, xaccAccountGetCommoditySCU(account), GNC_HOW_RND_NEVER);
        auto currency_amount = gnc_numeric_convert(amount, gnc_commodity_get_fraction(currency), GNC_HOW_RND_NEVER);
        if (gnc_numeric_check(account_amount) || gnc_numeric_check(currency_amount)) fail("Amount exceeds account or currency precision.");
        total = gnc_numeric_add(total, amount, GNC_DENOM_AUTO, GNC_HOW_DENOM_EXACT);
        if (gnc_numeric_check(total)) fail("Transaction total exceeds the numeric range.");
        drafts.push_back({account, amount, text(item, "memo"), item.if_contains("action") ? text(item, "action") : ""});
    }
    if (!gnc_numeric_zero_p(total)) fail("Split values must sum exactly to zero.");
    auto notes = args.if_contains("notes") ? text(args, "notes") : str(txn ? xaccTransGetNotes(txn) : nullptr);
    auto num = args.if_contains("number") ? text(args, "number") : str(txn ? xaccTransGetNum(txn) : nullptr);
    if (!txn) txn = xaccMallocTransaction(book);
    auto txn_id = *xaccTransGetGUID(txn);
    xaccTransBeginEdit(txn);
    struct EditGuard
    {
        Transaction* transaction;
        bool committed = false;
        ~EditGuard() { if (!committed) xaccTransRollbackEdit(transaction); }
    } edit{txn};
    if (updating)
    {
        auto old_splits = g_list_copy(xaccTransGetSplitList(txn));
        for (auto node = old_splits; node; node = node->next) xaccSplitDestroy(static_cast<Split*>(node->data));
        g_list_free(old_splits);
    }
    xaccTransSetCurrency(txn, currency);
    xaccTransSetDatePostedGDate(txn, posted);
    xaccTransSetDescription(txn, description.c_str());
    xaccTransSetNotes(txn, notes.c_str());
    xaccTransSetNum(txn, num.c_str());
    for (const auto& draft : drafts)
    {
        auto split = xaccMallocSplit(book);
        xaccSplitSetParent(split, txn);
        xaccSplitSetAccount(split, draft.account);
        xaccSplitSetAmount(split, draft.value);
        xaccSplitSetValue(split, draft.value);
        xaccSplitSetMemo(split, draft.memo.c_str());
        xaccSplitSetAction(split, draft.action.c_str());
    }
    xaccTransCommitEdit(txn);
    edit.committed = true;
    txn = xaccTransLookup(&txn_id, book);
    if (!txn || str(xaccTransGetDescription(txn)) != description ||
        date_string(txn) != text(args, "date", 10) || !gnc_commodity_equal(xaccTransGetCurrency(txn), currency) ||
        xaccTransCountSplits(txn) != static_cast<int>(drafts.size()) || !gnc_numeric_zero_p(xaccTransGetImbalanceValue(txn)))
        fail("The engine did not retain the requested commit; inspect the GnuCash error message.");
    auto result = transaction(txn, true);
    std::vector<bool> matched(drafts.size(), false);
    for (auto node = xaccTransGetSplitList(txn); node; node = node->next)
    {
        auto split = static_cast<Split*>(node->data);
        bool found = false;
        for (size_t i = 0; i < drafts.size(); ++i)
            if (!matched[i] && xaccSplitGetAccount(split) == drafts[i].account &&
                gnc_numeric_equal(xaccSplitGetValue(split), drafts[i].value) &&
                gnc_numeric_equal(xaccSplitGetAmount(split), drafts[i].value) && str(xaccSplitGetMemo(split)) == drafts[i].memo &&
                str(xaccSplitGetAction(split)) == drafts[i].action)
            { matched[i] = true; found = true; break; }
        if (!found) fail("Commit verification failed; read the transaction before retrying.");
    }
    m_receipt_bytes += request.size() + json::serialize(result).size();
    m_receipts.emplace(request_id, Receipt{request, result});
    return result;
}
}
