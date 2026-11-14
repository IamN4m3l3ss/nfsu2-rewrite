// TCP server on 127.0.0.1: one JSON object per line in, one per line out.
//   request:  {"id": 1, "method": "read_mem", "params": {...}}
//   response: {"id": 1, "result": ...}  or  {"id": 1, "error": "message"}
// Runs on its own thread. Never calls game code here: anything that must touch the game/device goes through
// run_on_main().
#include "probe.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <stdexcept>
#include <thread>

namespace probe {
namespace {

uintptr_t parse_addr(const json& v) {
    if (v.is_number_unsigned() || v.is_number_integer()) return static_cast<uintptr_t>(v.get<int64_t>());
    if (v.is_string()) return static_cast<uintptr_t>(std::stoull(v.get<std::string>(), nullptr, 0));
    throw std::runtime_error("address must be a number or a string like \"0x400000\"");
}

json dispatch(const std::string& method, const json& p) {
    if (method == "ping") {
        return {{"pong", true}, {"version", kVersion}, {"core_build", core_build()}, {"reload_count", api().reload_count()},
                {"loader", api().loader_version}, {"pid", GetCurrentProcessId()},
                {"uptime_ms", GetTickCount64()}, {"frames", d3d_info()["frames"]}};
    }
    if (method == "info") {
        const ExeInfo& e = exe();
        json j = d3d_info();
        j["exe_base"] = hex_addr(e.base);
        j["exe_image_size"] = e.image_size;
        j["exe_sha256"] = e.sha256;
        j["exe_hash_ok"] = e.hash_ok;
        j["init_thread"] = e.init_thread;
        j["version"] = kVersion;
        return j;
    }
    if (method == "read_mem") {
        uintptr_t addr = parse_addr(p.at("addr"));
        size_t size = p.value("size", 16u);
        if (size == 0 || size > (1u << 20)) throw std::runtime_error("size must be 1..1048576");
        std::vector<uint8_t> buf(size);
        if (!mem_read(addr, buf.data(), size)) throw std::runtime_error("unreadable memory at " + hex_addr(addr));
        return {{"addr", hex_addr(addr)}, {"hex", to_hex(buf.data(), size)}};
    }
    if (method == "write_mem") {
        uintptr_t addr = parse_addr(p.at("addr"));
        std::vector<uint8_t> data = from_hex(p.at("hex").get<std::string>());
        if (data.empty()) throw std::runtime_error("no bytes");
        if (!mem_write(addr, data.data(), data.size())) throw std::runtime_error("write failed at " + hex_addr(addr));
        logf("write_mem %s (%zu bytes)", hex_addr(addr).c_str(), data.size());
        return {{"addr", hex_addr(addr)}, {"written", data.size()}};
    }
    if (method == "aob_scan") {
        const ExeInfo& e = exe();
        uintptr_t start = p.contains("start") ? parse_addr(p["start"]) : e.base;
        uintptr_t end = p.contains("end") ? parse_addr(p["end"]) : e.base + e.image_size;
        size_t max = p.value("max", 64u);
        auto hits = aob_scan(p.at("pattern").get<std::string>(), start, end, max);
        json arr = json::array();
        for (auto h : hits) arr.push_back(hex_addr(h));
        return {{"count", hits.size()}, {"hits", arr}};
    }
    if (method == "log_tail") {
        return log_tail(p.value("n", 50u), p.value("since", uint64_t{0}));
    }
    if (method == "keep_active") return keep_active(p);
    if (method == "reload") {
        api().request_reload();
        return {{"requested", true}, {"reload_count", api().reload_count()}};
    }
    if (method == "no_traffic") return no_traffic(p);
    if (method == "key") return input_key(p);
    if (method == "release_all") return input_release_all();
    if (method == "input_state") return input_state();
    if (method == "car_state") return car_state();
    if (method == "schedule") return input_schedule(p);
    if (method == "schedule_status") return input_schedule_status();
    if (method == "record_start") return record_start(p);
    if (method == "record_stop") return record_stop();
    if (method == "trace_start") return trace_start(p);
    if (method == "trace_stop") return trace_stop();
    if (method == "screenshot") {
        int scale = p.value("scale", 1);
        unsigned timeout = p.value("timeout_ms", 5000u);
        return run_on_main([scale] { return screenshot(scale); }, timeout);
    }
    throw std::runtime_error("unknown method: " + method);
}

bool send_all(SOCKET s, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        int n = send(s, data.data() + off, static_cast<int>(std::min<size_t>(data.size() - off, 1 << 20)), 0);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

void serve_client(SOCKET c) {
    std::string buf;
    char chunk[4096];
    for (;;) {
        int n = recv(c, chunk, sizeof chunk, 0);
        if (n <= 0) return;
        buf.append(chunk, static_cast<size_t>(n));
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (line.empty() || line == "\r") continue;
            json resp;
            try {
                json req = json::parse(line);
                resp["id"] = req.value("id", json());
                try {
                    resp["result"] = dispatch(req.at("method").get<std::string>(), req.value("params", json::object()));
                } catch (const std::exception& e) {
                    resp["error"] = e.what();
                }
            } catch (const std::exception& e) {
                resp["error"] = std::string("bad request: ") + e.what();
            }
            if (!send_all(c, resp.dump(-1, ' ', false, json::error_handler_t::replace) + "\n")) return;
        }
    }
}

std::atomic<bool> g_stop{false};
std::atomic<SOCKET> g_listen{INVALID_SOCKET};
std::atomic<SOCKET> g_client{INVALID_SOCKET};
std::thread g_thread;

void server_thread(uint16_t port) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        logf("ERROR: WSAStartup failed");
        return;
    }
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOL yes = TRUE;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof yes);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // a reload can race the old socket's teardown: retry bind briefly
    int tries = 0;
    while (bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 && ++tries < 50 && !g_stop) Sleep(100);
    if (tries >= 50 || listen(ls, 4) != 0) {
        logf("ERROR: cannot listen on 127.0.0.1:%u (wsa %d)", port, WSAGetLastError());
        closesocket(ls);
        WSACleanup();
        return;
    }
    g_listen = ls;
    logf("listening on 127.0.0.1:%u", port);
    while (!g_stop) {
        SOCKET c = accept(ls, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;  // closed by server_stop, or transient
        if (g_stop) {
            closesocket(c);
            break;
        }
        int one = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
        g_client = c;
        serve_client(c);  // one client at a time; the bridge connects per call
        g_client = INVALID_SOCKET;
        closesocket(c);
    }
    WSACleanup();
}

}  // namespace

void server_start(uint16_t port) {
    g_stop = false;
    g_thread = std::thread(server_thread, port);
}

void server_stop() {
    g_stop = true;
    SOCKET ls = g_listen.exchange(INVALID_SOCKET);
    if (ls != INVALID_SOCKET) closesocket(ls);  // unblocks accept
    SOCKET c = g_client.load();
    if (c != INVALID_SOCKET) shutdown(c, SD_BOTH);  // unblocks recv; serve_client returns
    if (g_thread.joinable()) g_thread.join();
}

}  // namespace probe
