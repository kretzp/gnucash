// SPDX-License-Identifier: GPL-2.0-or-later
#include <config.h>
#include "gnc-mcp.hpp"
#include <boost/json/src.hpp>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <array>
#include <fcntl.h>
#include <random>
#include <stdexcept>
#include <unordered_set>
#ifdef G_OS_WIN32
#include <windows.h>
#include <sddl.h>
#else
#include <unistd.h>
#endif

namespace gnc::mcp
{
namespace
{
constexpr size_t max_request = 1024 * 1024, max_response = 4 * 1024 * 1024;
std::string secret()
{
    std::random_device random;
    const char* digits = "0123456789abcdef";
    std::string result;
    for (int i = 0; i < 32; ++i)
    {
        auto byte = random() & 255;
        result += digits[byte >> 4]; result += digits[byte & 15];
    }
    return result;
}
bool authenticate(const json::object& request, const std::string& token)
{
    auto value = request.if_contains("token");
    if (!value || !value->is_string() || value->as_string().size() != token.size()) return false;
    unsigned difference = 0;
    for (size_t i = 0; i < token.size(); ++i) difference |= value->as_string()[i] ^ token[i];
    return difference == 0;
}
void write_discovery(const std::string& path, const std::string& data)
{
    auto directory = g_path_get_dirname(path.c_str());
    auto mkdir_result = g_mkdir_with_parents(directory, 0700);
    g_free(directory);
    if (mkdir_result) throw std::runtime_error("Cannot create MCP discovery directory.");
#ifdef G_OS_WIN32
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(
            "D:P(A;;FA;;;OW)(A;;FA;;;SY)(A;;FA;;;BA)", SDDL_REVISION_1, &descriptor, nullptr))
        throw std::runtime_error("Cannot secure MCP discovery file.");
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    auto wide = g_utf8_to_utf16(path.c_str(), -1, nullptr, nullptr, nullptr);
    auto handle = CreateFileW(reinterpret_cast<LPCWSTR>(wide), GENERIC_WRITE, 0, &attributes,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    g_free(wide); LocalFree(descriptor);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("MCP discovery file already exists or cannot be created.");
    DWORD written = 0;
    bool ok = WriteFile(handle, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size();
    CloseHandle(handle);
#else
    auto handle = g_open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (handle < 0) throw std::runtime_error("MCP discovery file already exists or cannot be created.");
    bool ok = write(handle, data.data(), data.size()) == static_cast<ssize_t>(data.size());
    close(handle);
#endif
    if (!ok) { g_unlink(path.c_str()); throw std::runtime_error("Cannot write MCP discovery file."); }
}
}

struct LiveBridge::State : std::enable_shared_from_this<LiveBridge::State>
{
    struct Connection
    {
        std::shared_ptr<State> state;
        GSocketConnection* socket;
        GCancellable* cancel = g_cancellable_new();
        guint timeout = 0;
        std::array<guint8, 4> header{};
        std::vector<char> body;
        std::string output;
        Connection(std::shared_ptr<State> owner, GSocketConnection* conn) : state(std::move(owner)), socket(G_SOCKET_CONNECTION(g_object_ref(conn)))
        {
            state->connections.insert(this);
            timeout = g_timeout_add_seconds(10, [](gpointer data) -> gboolean {
                auto self = static_cast<Connection*>(data);
                self->timeout = 0; g_cancellable_cancel(self->cancel); return G_SOURCE_REMOVE;
            }, this);
        }
        ~Connection()
        {
            if (timeout) g_source_remove(timeout);
            g_io_stream_close(G_IO_STREAM(socket), nullptr, nullptr);
            g_object_unref(socket); g_object_unref(cancel);
            state->connections.erase(this);
        }
        static bool complete(GObject* stream, GAsyncResult* result, size_t expected)
        {
            gsize count = 0;
            GError* error = nullptr;
            bool ok = g_input_stream_read_all_finish(G_INPUT_STREAM(stream), result, &count, &error);
            g_clear_error(&error);
            return ok && count == expected;
        }
        void start()
        {
            g_input_stream_read_all_async(g_io_stream_get_input_stream(G_IO_STREAM(socket)), header.data(), 4,
                G_PRIORITY_DEFAULT, cancel, [](GObject* stream, GAsyncResult* result, gpointer data) {
                    auto self = static_cast<Connection*>(data);
                    if (!complete(stream, result, 4)) { delete self; return; }
                    uint32_t size = (uint32_t(self->header[0]) << 24) | (uint32_t(self->header[1]) << 16) |
                                    (uint32_t(self->header[2]) << 8) | uint32_t(self->header[3]);
                    if (!size || size > max_request) { delete self; return; }
                    self->body.resize(size);
                    g_input_stream_read_all_async(G_INPUT_STREAM(stream), self->body.data(), size, G_PRIORITY_DEFAULT,
                        self->cancel, [](GObject* input, GAsyncResult* response, gpointer payload) {
                            auto conn = static_cast<Connection*>(payload);
                            if (!complete(input, response, conn->body.size())) { delete conn; return; }
                            conn->dispatch();
                        }, self);
                }, this);
        }
        void dispatch()
        {
            json::object response;
            try
            {
                if (!state->active) throw std::runtime_error("GnuCash is shutting down.");
                auto request = json::parse(std::string_view(body.data(), body.size())).as_object();
                if (!authenticate(request, state->token)) throw std::runtime_error("Authentication failed.");
                if (state->executing) throw std::runtime_error("Another GnuCash request is still in progress.");
                struct Busy
                {
                    bool& flag;
                    explicit Busy(bool& value) : flag(value) { flag = true; }
                    ~Busy() { flag = false; }
                } busy(state->executing);
                auto method = request.at("method").as_string();
                const auto& args = request.at("arguments").as_object();
                // GIO callbacks run on the GTK default context; all QOF work stays here.
                struct Refresh
                {
                    std::function<void(bool)>& function;
                    explicit Refresh(std::function<void(bool)>& fn) : function(fn) { if (function) function(true); }
                    ~Refresh() { if (function) function(false); }
                } guard(state->refresh);
                response["result"] = state->api.call(state->book(), std::string(method), args);
            }
            catch (const std::exception& error) { response = {{"error", error.what()}}; }
            output = json::serialize(response);
            if (output.size() > max_response) output = "{\"error\":\"Response too large; use a smaller page.\"}";
            auto size = static_cast<uint32_t>(output.size());
            std::string frame(4, '\0');
            for (int i = 0; i < 4; ++i) frame[i] = static_cast<char>((size >> (24 - i * 8)) & 255);
            output.insert(0, frame);
            g_output_stream_write_all_async(g_io_stream_get_output_stream(G_IO_STREAM(socket)), output.data(), output.size(),
                G_PRIORITY_DEFAULT, cancel, [](GObject* stream, GAsyncResult* result, gpointer data) {
                    gsize count = 0; GError* error = nullptr;
                    g_output_stream_write_all_finish(G_OUTPUT_STREAM(stream), result, &count, &error);
                    g_clear_error(&error); delete static_cast<Connection*>(data);
                }, this);
        }
    };
    bool active = true;
    bool executing = false;
    GSocketService* service = nullptr;
    std::string discovery, token;
    std::function<QofBook*()> book;
    std::function<void(bool)> refresh;
    BookApi api;
    std::unordered_set<Connection*> connections;
    ~State() { if (service) g_object_unref(service); }
};

LiveBridge::LiveBridge(const std::string& file, std::function<QofBook*()> book, std::function<void(bool)> refresh)
    : m_state(std::make_shared<State>())
{
    auto& state = *m_state;
    state.book = std::move(book); state.refresh = std::move(refresh); state.token = secret();
    state.service = g_socket_service_new();
    auto address = g_inet_address_new_from_string("127.0.0.1");
    auto endpoint = g_inet_socket_address_new(address, 0);
    GSocketAddress* bound = nullptr;
    GError* error = nullptr;
    auto ok = g_socket_listener_add_address(G_SOCKET_LISTENER(state.service), endpoint,
        G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP, nullptr, &bound, &error);
    g_object_unref(endpoint); g_object_unref(address);
    if (!ok) { g_clear_error(&error); throw std::runtime_error("Cannot bind the local MCP bridge."); }
    auto port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(bound));
    g_object_unref(bound);
    write_discovery(file, json::serialize(json::object{{"host", "127.0.0.1"}, {"port", port}, {"token", state.token}, {"version", 1}}));
    state.discovery = file;
    g_signal_connect(state.service, "incoming", G_CALLBACK(+[](GSocketService*, GSocketConnection* conn, GObject*, gpointer data) -> gboolean {
        auto owner = static_cast<State*>(data);
        if (!owner->active || owner->connections.size() >= 8) return FALSE;
        auto connection = new State::Connection(owner->shared_from_this(), conn);
        connection->start(); return TRUE;
    }), &state);
    g_socket_service_start(state.service);
}
LiveBridge::~LiveBridge()
{
    auto& state = *m_state;
    state.active = false;
    g_socket_service_stop(state.service);
    g_socket_listener_close(G_SOCKET_LISTENER(state.service));
    g_signal_handlers_disconnect_by_data(state.service, &state);
    for (auto connection : state.connections) g_cancellable_cancel(connection->cancel);
    if (!state.discovery.empty()) g_unlink(state.discovery.c_str());
}
void LiveBridge::invalidate_book() { m_state->api.invalidate(); }
}
