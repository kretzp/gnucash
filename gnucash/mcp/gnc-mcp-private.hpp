// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "gnc-mcp.hpp"
#include "Account.h"
#include "Transaction.h"
#include "gnc-numeric.h"

namespace gnc::mcp::detail
{
[[noreturn]] void fail(const char*);
std::string str(const char*);
std::string text(const json::object&, const char*, size_t maximum = 4096);
std::string guid(const GncGUID*);
GncGUID parse_guid(const std::string&);
json::object number(gnc_numeric);
gnc_numeric decimal(const std::string&);
GDate date(const std::string&);
std::string date_string(Transaction*);
json::object transaction(Transaction*, bool);
bool ordinary_account(Account*);
json::object csv_settings(QofBook*);
json::object csv_preview(QofBook*, const json::object&);
}
