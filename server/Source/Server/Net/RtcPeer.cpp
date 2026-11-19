#include <Server/Net/RtcPeer.h>

#include <rtc/rtc.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {

struct Peer
{
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel>    dc[2];
};

std::mutex                                     g_mu;
std::unordered_map<uint32_t, std::shared_ptr<Peer>> g_peers;
std::deque<rtcw_event_t>                       g_events;
rtc::Configuration                             g_config;
uint32_t                                       g_next = 1;
bool                                           g_ready;

void push(int kind, uint32_t peer, uint8_t channel, const void* data, size_t len, int text, const char* why)
{
    rtcw_event_t ev{};
    ev.kind    = kind;
    ev.peer    = peer;
    ev.channel = channel;
    ev.text    = (uint8_t) text;
    if (data != nullptr) {
        ev.data = (uint8_t*) std::malloc(len + 1);
        if (ev.data == nullptr) {
            return;
        }
        std::memcpy(ev.data, data, len);
        ev.data[len] = 0;
        ev.len       = len;
    }
    if (why != nullptr) {
        std::snprintf(ev.why, sizeof ev.why, "%s", why);
    }
    std::lock_guard<std::mutex> lock(g_mu);
    g_events.push_back(ev);
}

std::shared_ptr<Peer> find(uint32_t id)
{
    std::lock_guard<std::mutex> lock(g_mu);
    auto                        it = g_peers.find(id);
    return it == g_peers.end() ? nullptr : it->second;
}

void set_error(char* err, size_t size, const char* what)
{
    if (err != nullptr && size > 0) {
        std::snprintf(err, size, "%s", what);
    }
}

/* the contract's labels (docs/protocol-1.0.md, "WebRTC transport") */
int channel_of(const std::string& label)
{
    if (label == "reliable") {
        return 0;
    }
    if (label == "unreliable") {
        return 1;
    }
    return -1;
}

} // namespace

extern "C" int rtcw_init(uint16_t port, const rtcw_ice_t* ice, size_t n_ice, const char* cert_pem,
                         const char* key_pem, rtcw_log_fn log, char* err, size_t err_size)
{
    try {
        static rtcw_log_fn log_fn;
        log_fn = log;
        /* errors only: its warnings repeat per packet (a send to a peer
           that just left logged 66 lines in a second) */
        rtc::InitLogger(rtc::LogLevel::Error, [](rtc::LogLevel level, std::string message) {
            if (log_fn != nullptr) {
                log_fn(level <= rtc::LogLevel::Error ? 1 : 0, message.c_str());
            }
        });
        rtc::Preload();
        g_config = rtc::Configuration();
        for (size_t i = 0; i < n_ice; i++) {
            if (ice[i].url == nullptr || *ice[i].url == '\0') {
                continue;
            }
            /* one bad URL must not cost the browsers the others (a typo in
               a TURN entry left the server without STUN too) */
            try {
                rtc::IceServer server{std::string(ice[i].url)};
                if (server.type == rtc::IceServer::Type::Turn) {
                    /* the credentials of the config win over a URL's own
                       user:pass@ part only when given */
                    if (ice[i].username != nullptr && *ice[i].username != '\0') {
                        server.username = ice[i].username;
                    }
                    if (ice[i].credential != nullptr && *ice[i].credential != '\0') {
                        server.password = ice[i].credential;
                    }
                }
                g_config.iceServers.push_back(server);
            } catch (const std::exception& e) {
                if (log != nullptr) {
                    std::string what = std::string("ICE server ") + ice[i].url + " skipped: " + e.what();
                    log(1, what.c_str());
                }
            }
        }
        /* every peer on one port: one hole in a firewall, one line in a
           router's forwarding */
        g_config.enableIceUdpMux    = true;
        g_config.portRangeBegin     = port;
        g_config.portRangeEnd       = port;
        g_config.certificatePemFile = std::string(cert_pem);
        g_config.keyPemFile         = std::string(key_pem);
        g_ready                     = true;
        return 1;
    } catch (const std::exception& e) {
        set_error(err, err_size, e.what());
        return 0;
    }
}

extern "C" void rtcw_shutdown(void)
{
    std::unordered_map<uint32_t, std::shared_ptr<Peer>> peers;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        peers.swap(g_peers);
    }
    for (auto& p : peers) {
        try {
            p.second->pc->close();
        } catch (...) {
        }
    }
    peers.clear();
    if (g_ready) {
        rtc::Cleanup().wait_for(std::chrono::seconds(2));
    }
    std::lock_guard<std::mutex> lock(g_mu);
    for (auto& ev : g_events) {
        std::free(ev.data);
    }
    g_events.clear();
    g_ready = false;
}

extern "C" uint32_t rtcw_peer_create(const char* offer_sdp, char* err, size_t err_size)
{
    if (!g_ready) {
        set_error(err, err_size, "webrtc not started");
        return 0;
    }
    uint32_t id;
    auto     peer = std::make_shared<Peer>();
    {
        std::lock_guard<std::mutex> lock(g_mu);
        id = g_next++;
        if (g_next == 0) {
            g_next = 1;
        }
    }
    try {
        peer->pc = std::make_shared<rtc::PeerConnection>(g_config);
        /* not trickled: the answer goes out once it holds every candidate */
        peer->pc->onGatheringStateChange([id](rtc::PeerConnection::GatheringState state) {
            if (state == rtc::PeerConnection::GatheringState::Complete) {
                push(RTCW_GATHERED, id, 0, nullptr, 0, 0, nullptr);
            }
        });
        peer->pc->onStateChange([id](rtc::PeerConnection::State state) {
            if (state == rtc::PeerConnection::State::Failed) {
                push(RTCW_CLOSED, id, 0, nullptr, 0, 0, "failed");
            } else if (state == rtc::PeerConnection::State::Disconnected) {
                push(RTCW_CLOSED, id, 0, nullptr, 0, 0, "disconnected");
            } else if (state == rtc::PeerConnection::State::Closed) {
                push(RTCW_CLOSED, id, 0, nullptr, 0, 0, "closed");
            }
        });
        peer->pc->onDataChannel([id](std::shared_ptr<rtc::DataChannel> dc) {
            int ch = channel_of(dc->label());
            if (ch < 0) {
                dc->close();
                return;
            }
            std::shared_ptr<Peer> p = find(id);
            if (p == nullptr) {
                return;
            }
            uint8_t c = (uint8_t) ch;
            dc->onOpen([id, c]() { push(RTCW_CHANNEL_OPEN, id, c, nullptr, 0, 0, nullptr); });
            dc->onClosed([id]() { push(RTCW_CLOSED, id, 0, nullptr, 0, 0, "channel_closed"); });
            dc->onMessage([id, c](rtc::message_variant message) {
                if (std::holds_alternative<rtc::binary>(message)) {
                    const rtc::binary& b = std::get<rtc::binary>(message);
                    push(RTCW_MESSAGE, id, c, b.data(), b.size(), 0, nullptr);
                } else {
                    const std::string& s = std::get<std::string>(message);
                    push(RTCW_MESSAGE, id, c, s.data(), s.size(), 1, nullptr);
                }
            });
            std::lock_guard<std::mutex> lock(g_mu);
            p->dc[ch] = dc;
            /* a channel may open before its callback was set */
            if (dc->isOpen()) {
                rtcw_event_t ev{};
                ev.kind    = RTCW_CHANNEL_OPEN;
                ev.peer    = id;
                ev.channel = c;
                g_events.push_back(ev);
            }
        });
        {
            std::lock_guard<std::mutex> lock(g_mu);
            g_peers[id] = peer;
        }
        peer->pc->setRemoteDescription(rtc::Description(std::string(offer_sdp), rtc::Description::Type::Offer));
        return id;
    } catch (const std::exception& e) {
        set_error(err, err_size, e.what());
        std::lock_guard<std::mutex> lock(g_mu);
        g_peers.erase(id);
        return 0;
    }
}

extern "C" char* rtcw_local_sdp(uint32_t id)
{
    std::shared_ptr<Peer> p = find(id);
    if (p == nullptr) {
        return nullptr;
    }
    try {
        auto d = p->pc->localDescription();
        if (!d.has_value()) {
            return nullptr;
        }
        std::string s = std::string(*d);
        return strdup(s.c_str());
    } catch (...) {
        return nullptr;
    }
}

extern "C" int rtcw_send(uint32_t id, uint8_t channel, const void* data, size_t len, int text)
{
    std::shared_ptr<rtc::DataChannel> dc;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto                        it = g_peers.find(id);
        if (it == g_peers.end() || channel > 1) {
            return -1;
        }
        dc = it->second->dc[channel];
        /* the unreliable channel not open (yet): the reliable one carries
           the packet, late rather than never */
        if ((dc == nullptr || !dc->isOpen()) && channel == 1) {
            dc = it->second->dc[0];
        }
    }
    if (dc == nullptr || !dc->isOpen()) {
        return -1;
    }
    try {
        if (text) {
            dc->send(std::string((const char*) data, len));
        } else {
            dc->send((const std::byte*) data, len);
        }
        return 0;
    } catch (...) {
        return -1;
    }
}

extern "C" size_t rtcw_buffered(uint32_t id)
{
    std::shared_ptr<Peer> p = find(id);
    if (p == nullptr) {
        return 0;
    }
    size_t n = 0;
    for (auto& dc : p->dc) {
        try {
            n += dc != nullptr ? dc->bufferedAmount() : 0;
        } catch (...) {
        }
    }
    return n;
}

extern "C" uint32_t rtcw_rtt(uint32_t id)
{
    std::shared_ptr<Peer> p = find(id);
    if (p == nullptr) {
        return 0;
    }
    try {
        auto rtt = p->pc->rtt();
        return rtt.has_value() ? (uint32_t) rtt->count() : 0;
    } catch (...) {
        return 0;
    }
}

extern "C" void rtcw_remote_address(uint32_t id, char* out, size_t size)
{
    out[0]                  = '\0';
    std::shared_ptr<Peer> p = find(id);
    if (p == nullptr) {
        return;
    }
    try {
        auto a = p->pc->remoteAddress();
        if (a.has_value()) {
            std::snprintf(out, size, "%s", a->c_str());
        }
    } catch (...) {
    }
}

extern "C" void rtcw_close(uint32_t id)
{
    std::shared_ptr<Peer> p;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto                        it = g_peers.find(id);
        if (it == g_peers.end()) {
            return;
        }
        p = it->second;
        g_peers.erase(it);
    }
    try {
        p->pc->close();
    } catch (...) {
    }
}

extern "C" int rtcw_poll(rtcw_event_t* ev)
{
    std::lock_guard<std::mutex> lock(g_mu);
    while (!g_events.empty()) {
        rtcw_event_t e = g_events.front();
        g_events.pop_front();
        if (g_peers.find(e.peer) == g_peers.end()) {
            std::free(e.data); /* closed since */
            continue;
        }
        *ev = e;
        return 1;
    }
    return 0;
}
