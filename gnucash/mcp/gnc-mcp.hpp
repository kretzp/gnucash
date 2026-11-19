// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <boost/json.hpp>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include "qofbook.h"

namespace gnc::mcp
{
namespace json = boost::json;
// Called only on the engine/GTK thread. Never retains engine objects between calls.
class BookApi
{
public:
    json::value call(QofBook*, const std::string&, const json::object&);
    void invalidate();
private:
    QofBook* m_book = nullptr;
    std::string m_token;
    struct Receipt { std::string request; json::value result; };
    std::unordered_map<std::string, Receipt> m_receipts;
    size_t m_receipt_bytes = 0;
};

class LiveBridge
{
public:
    LiveBridge(const std::string& discovery_file, std::function<QofBook*()> book,
               std::function<void(bool)> refresh = {});
    ~LiveBridge();
    void invalidate_book();
    LiveBridge(const LiveBridge&) = delete;
    LiveBridge& operator=(const LiveBridge&) = delete;
private:
    struct State;
    std::shared_ptr<State> m_state;
};
}
