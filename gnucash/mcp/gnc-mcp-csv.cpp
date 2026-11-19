// SPDX-License-Identifier: GPL-2.0-or-later
#include <config.h>
#include "gnc-mcp-private.hpp"
#include "gnc-import-tx.hpp"
#include "gnc-ui-util.h"
#include "Split.h"
#include <set>
#include <sstream>
#include <locale>

namespace gnc::mcp::detail
{
namespace
{
void current_book(QofBook* book)
{
    if (gnc_get_current_book() != book) fail("CSV settings must belong to the currently open GUI book.");
}

std::string exact_decimal(gnc_numeric value)
{
    // Convert without rounding, then serialize with a fixed decimal point, never the UI locale.
    auto converted = gnc_numeric_convert(value, 1000000000, GNC_HOW_RND_NEVER);
    if (gnc_numeric_check(converted)) fail("CSV amount cannot be represented exactly with nine decimal places.");
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << GncNumeric(converted);
    return stream.str();
}

bool possible_duplicate(QofBook* book, const json::object& draft)
{
    const auto& first = draft.at("splits").as_array()[0].as_object();
    auto id = parse_guid(text(first, "account_guid", 32));
    auto amount = decimal(text(first, "value", 30));
    auto account = xaccAccountLookup(&id, book);
    // Deliberately conservative: description differences do not hide a same-day/same-amount match.
    for (auto node = xaccAccountGetSplitList(account); node; node = node->next)
    {
        auto split = static_cast<Split*>(node->data);
        auto txn = xaccSplitGetParent(split);
        if (txn && date_string(txn) == draft.at("date").as_string() && gnc_numeric_equal(amount, xaccSplitGetAmount(split))) return true;
    }
    return false;
}
}

json::object csv_settings(QofBook* book)
{
    current_book(book);
    json::array settings;
    for (const auto& preset : get_import_presets_trans())
        settings.push_back(json::object{{"name", preset->m_name}, {"load_error", preset->m_load_error},
            {"multi_split", preset->m_multi_split}, {"encoding", preset->m_encoding},
            {"base_account_guid", preset->m_base_account ? guid(xaccAccountGetGUID(preset->m_base_account)) : ""}});
    return {{"settings", settings}};
}

json::object csv_preview(QofBook* book, const json::object& args)
{
    current_book(book);
    auto name = text(args, "settings_name", 256);
    std::shared_ptr<CsvTransImpSettings> settings;
    for (const auto& preset : get_import_presets_trans())
        if (preset->m_name == name) { if (settings) fail("CSV setting name is ambiguous."); settings = preset; }
    if (!settings || settings->m_load_error) fail("CSV setting was not found or could not be loaded. Call list_csv_import_settings.");
    if (settings->m_multi_split || settings->m_file_format != GncImpFileFormat::CSV)
        fail("MCP CSV import currently supports single-row, two-split CSV settings only.");
    auto encoded = text(args, "csv_base64", 700000);
    gsize size = 0;
    auto bytes = g_base64_decode(encoded.c_str(), &size);
    std::unique_ptr<guchar, decltype(&g_free)> buffer(bytes, g_free);
    auto canonical = g_base64_encode(bytes, size);
    bool valid = encoded == canonical;
    g_free(canonical);
    if (!valid || size == 0 || size > 512 * 1024) fail("CSV must be canonical base64 containing 1 to 524288 bytes.");
    std::string contents(reinterpret_cast<char*>(bytes), size);
    if (contents.find('\0') != std::string::npos) fail("CSV containing NUL bytes is unsupported. Export UTF-8 or the setting's legacy encoding.");
    Account* transfer = nullptr;
    if (args.if_contains("default_transfer_account_guid"))
    {
        auto id = parse_guid(text(args, "default_transfer_account_guid", 32));
        transfer = xaccAccountLookup(&id, book);
        if (!transfer) fail("Default transfer account was not found.");
    }
    GncTxImport importer(GncImpFileFormat::CSV);
    importer.m_tokenizer->load_data(contents, settings->m_encoding);
    importer.settings(*settings);
    importer.tokenize(false);
    auto verification = importer.verify(true);
    json::array rows;
    auto readonly = qof_book_is_readonly(book) || !qof_book_is_open(book);
    bool ready = verification.empty() && !readonly;
    size_t index = 0, count = 0;
    std::set<std::string> fingerprints;
    for (auto& line : importer.m_parsed_lines)
    {
        ++index;
        if (std::get<PL_SKIP>(line)) continue;
        if (++count > 500) fail("At most 500 CSV transactions per request are supported.");
        json::object row{{"row", index}};
        try
        {
            if (!std::get<PL_ERROR>(line).empty()) fail("CSV row contains parse or account mapping errors.");
            auto data = std::get<PL_PRESPLIT>(line)->single_currency_data(transfer);
            if (!ordinary_account(data.account) || !ordinary_account(data.transfer_account) ||
                xaccAccountGetPlaceholder(data.account) || xaccAccountGetPlaceholder(data.transfer_account))
                fail("CSV row requires ordinary non-placeholder accounts.");
            for (auto account : {data.account, data.transfer_account})
                if (gnc_numeric_check(gnc_numeric_convert(data.amount, xaccAccountGetCommoditySCU(account), GNC_HOW_RND_NEVER)))
                    fail("CSV amount exceeds account precision.");
            if (gnc_numeric_check(gnc_numeric_convert(data.amount, gnc_commodity_get_fraction(data.currency), GNC_HOW_RND_NEVER)))
                fail("CSV amount exceeds currency precision.");
            auto posted = date(data.date);
            if (qof_book_uses_autoreadonly(book) && g_date_compare(&posted, qof_book_get_autoreadonly_gdate(book)) < 0)
                fail("CSV date is in a read-only accounting period.");
            json::object draft{{"date", data.date}, {"description", data.description}, {"number", data.number},
                {"notes", data.notes}, {"currency", str(gnc_commodity_get_mnemonic(data.currency))},
                {"splits", json::array{
                    json::object{{"account_guid", guid(xaccAccountGetGUID(data.account))}, {"value", exact_decimal(data.amount)}, {"memo", data.memo}, {"action", data.action}},
                    json::object{{"account_guid", guid(xaccAccountGetGUID(data.transfer_account))}, {"value", exact_decimal(gnc_numeric_neg(data.amount))}, {"memo", data.transfer_memo}, {"action", data.transfer_action}}}}};
            // Use date/source amount/account, including within-file duplicates.
            auto fingerprint = data.date + guid(xaccAccountGetGUID(data.account)) + exact_decimal(data.amount);
            for (const auto* key : {"date", "description", "number", "notes", "currency"}) text(draft, key);
            for (const auto& split : draft.at("splits").as_array())
                for (const auto* key : {"memo", "action"}) text(split.as_object(), key);
            bool duplicate = possible_duplicate(book, draft) || !fingerprints.insert(fingerprint).second;
            row["draft"] = draft;
            row["possible_duplicate"] = duplicate;
        }
        catch (const std::exception& error) { row["error"] = str(error.what()).substr(0, 4096); ready = false; }
        rows.push_back(row);
    }
    if (rows.empty()) ready = false;
    json::object result{{"settings_name", name}, {"rows", rows}, {"ready", ready},
        {"read_only", static_cast<bool>(readonly)}, {"verification", verification.substr(0, 4096)}};
    // All input bytes/settings/drafts and current duplicate results participate in optimistic concurrency.
    auto canonical_result = json::serialize(result);
    auto checksum = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(checksum, reinterpret_cast<const guchar*>(contents.data()), contents.size());
    g_checksum_update(checksum, reinterpret_cast<const guchar*>(canonical_result.data()), canonical_result.size());
    result["revision"] = g_checksum_get_string(checksum);
    g_checksum_free(checksum);
    return result;
}
}
