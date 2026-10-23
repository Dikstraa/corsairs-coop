// UDP implementation of coopnet::Session.
//
// Packet: 'C' 'N' version kind, then kind-specific data. Reliable messages are split into
// fragments, each with a per-peer sequence number; the receiver acks every fragment and
// delivers them in order, the sender resends until acked. A background thread receives
// packets and drives resends, pings and timeouts; all state is guarded by one mutex.
#include "session.h"
#include "wire.h"

#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <thread>

#pragma comment(lib, "ws2_32.lib")

namespace coopnet
{
namespace
{

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Ms = std::chrono::milliseconds;

constexpr uint8_t kProtoVersion = 1;
constexpr size_t kMaxPacket = 1200;
constexpr size_t kHeaderSize = 4;
constexpr size_t kMaxFragment = kMaxPacket - kHeaderSize - 4 - 1; // kRel adds seq + flags
constexpr size_t kMaxMessage = 16 * 1024 * 1024;
constexpr size_t kMaxEarly = 4096;  // out-of-order fragments kept per peer
constexpr int kResendBudget = 256; // resends per peer per tick
constexpr Ms kResendInterval{150};
constexpr Ms kHelloInterval{500};
constexpr Ms kPingInterval{1000};
constexpr Ms kTimeout{10000};

enum Kind : uint8_t
{
    kHello = 1, // client -> host: u32 nonce, str name
    kWelcome,   // host -> client: u32 nonce, u16 id, str hostName
    kReject,    // host -> client: u8 reason (layout fixed across versions)
    kRel,       // u32 seq, u8 flags, fragment bytes
    kUnrel,     // message bytes
    kAck,       // u32 seq
    kPing,      // u64 sender time ms
    kPong,      // u64 echoed time ms
    kBye,
};
constexpr uint8_t kFlagLast = 1;

struct Pending
{
    std::string packet;
    TimePoint lastSend;
};

struct Peer
{
    int id = -1;
    sockaddr_in addr{};
    std::string name;
    uint32_t nonce = 0;
    TimePoint lastRecv{};
    TimePoint lastPing{};
    int rttMs = -1;
    uint32_t nextOutSeq = 0;
    std::map<uint32_t, Pending> unacked;
    uint32_t nextInSeq = 0;
    std::map<uint32_t, std::pair<uint8_t, std::string>> early;
    std::string partial; // fragments of the reliable message being reassembled
};

bool SameAddr(const sockaddr_in &a, const sockaddr_in &b)
{
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

uint64_t NowMs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<Ms>(Clock::now().time_since_epoch()).count());
}

Writer Begin(Kind kind)
{
    Writer w;
    w.U8('C');
    w.U8('N');
    w.U8(kProtoVersion);
    w.U8(kind);
    return w;
}

Writer Event(uint16_t type)
{
    Writer w;
    w.U16(type);
    return w;
}

class UdpSession final : public Session
{
  public:
    UdpSession() : rng_(std::random_device{}())
    {
        WSADATA wsa;
        wsaOk_ = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }

    ~UdpSession() override
    {
        Disconnect();
        if (wsaOk_)
            WSACleanup();
    }

    bool Host(uint16_t port, int maxPeers, const std::string &name, std::string &err) override
    {
        Disconnect();
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        local.sin_port = htons(port);
        if (!OpenSocket(local, err))
            return false;

        std::lock_guard<std::mutex> lock(mu_);
        state_ = State::Hosting;
        myId_ = kHostId;
        maxPeers_ = maxPeers;
        name_ = name;
        nextPeerId_ = 1;
        StartThread();
        return true;
    }

    bool Connect(const std::string &host, uint16_t port, const std::string &name, std::string &err) override
    {
        Disconnect();
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo *res = nullptr;
        if (!wsaOk_ || getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res)
        {
            err = "cannot resolve '" + host + "'";
            return false;
        }
        Peer hostPeer;
        hostPeer.id = kHostId;
        hostPeer.addr = *reinterpret_cast<sockaddr_in *>(res->ai_addr);
        hostPeer.addr.sin_port = htons(port);
        freeaddrinfo(res);

        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        if (!OpenSocket(local, err))
            return false;

        std::lock_guard<std::mutex> lock(mu_);
        peers_[kHostId] = std::move(hostPeer);
        state_ = State::Connecting;
        myId_ = -1;
        name_ = name;
        nonce_ = static_cast<uint32_t>(rng_()) | 1;
        connectStart_ = Clock::now();
        lastHello_ = TimePoint{};
        StartThread();
        return true;
    }

    void Disconnect() override
    {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (sock_ == INVALID_SOCKET)
                return;
            if (state_ == State::Connected || state_ == State::Hosting)
            {
                Writer bye = Begin(kBye);
                for (auto &[id, p] : peers_)
                    for (int i = 0; i < 3; ++i)
                        SendRaw(p.addr, bye.Data());
            }
        }
        stop_ = true;
        if (thread_.joinable())
            thread_.join();

        std::lock_guard<std::mutex> lock(mu_);
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
        peers_.clear();
        inbox_.clear();
        state_ = State::Offline;
        myId_ = -1;
    }

    State GetState() const override
    {
        std::lock_guard<std::mutex> lock(mu_);
        return state_;
    }

    int MyId() const override
    {
        std::lock_guard<std::mutex> lock(mu_);
        return myId_;
    }

    std::vector<int> Peers() const override
    {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<int> ids;
        if (state_ == State::Connected || state_ == State::Hosting)
            for (auto &[id, p] : peers_)
                ids.push_back(id);
        return ids;
    }

    std::string PeerName(int id) const override
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (id == myId_)
            return name_;
        auto it = peers_.find(id);
        return it == peers_.end() ? std::string() : it->second.name;
    }

    int PingMs(int id) const override
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = peers_.find(id);
        return it == peers_.end() ? -1 : it->second.rttMs;
    }

    bool Send(int to, const std::string &payload, bool reliable) override
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (state_ != State::Connected && state_ != State::Hosting)
            return false;
        if (payload.size() > kMaxMessage || (!reliable && payload.size() > kMaxPacket - kHeaderSize))
            return false;

        bool sent = false;
        for (auto &[id, p] : peers_)
        {
            if (to != kBroadcast && to != id)
                continue;
            if (reliable)
                SendReliable(p, payload);
            else
            {
                Writer w = Begin(kUnrel);
                w.Raw(payload.data(), payload.size());
                SendRaw(p.addr, w.Data());
            }
            sent = true;
        }
        return sent;
    }

    bool Poll(Message &out) override
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (inbox_.empty())
            return false;
        out = std::move(inbox_.front());
        inbox_.pop_front();
        return true;
    }

    void SetSimulatedLoss(float fraction) override
    {
        std::lock_guard<std::mutex> lock(mu_);
        loss_ = fraction;
    }

  private:
    bool OpenSocket(const sockaddr_in &local, std::string &err)
    {
        if (!wsaOk_)
        {
            err = "WSAStartup failed";
            return false;
        }
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET)
        {
            err = "socket() failed: " + std::to_string(WSAGetLastError());
            return false;
        }
        if (bind(s, reinterpret_cast<const sockaddr *>(&local), sizeof local) == SOCKET_ERROR)
        {
            err = "bind() failed: " + std::to_string(WSAGetLastError());
            closesocket(s);
            return false;
        }
        u_long nonBlocking = 1;
        ioctlsocket(s, FIONBIO, &nonBlocking);
        // Otherwise an ICMP "port unreachable" makes the next recvfrom fail with WSAECONNRESET.
        BOOL reportReset = FALSE;
        DWORD bytes = 0;
        WSAIoctl(s, SIO_UDP_CONNRESET, &reportReset, sizeof reportReset, nullptr, 0, &bytes, nullptr, nullptr);

        std::lock_guard<std::mutex> lock(mu_);
        sock_ = s;
        stop_ = false;
        return true;
    }

    void StartThread() { thread_ = std::thread([this] { Loop(); }); }

    void Loop()
    {
        char buf[2048];
        while (!stop_)
        {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(sock_, &fds);
            timeval tv{0, 5000};
            int ready = select(0, &fds, nullptr, nullptr, &tv);

            std::lock_guard<std::mutex> lock(mu_);
            TimePoint now = Clock::now();
            while (ready > 0)
            {
                sockaddr_in from{};
                int fromLen = sizeof from;
                int n = recvfrom(sock_, buf, sizeof buf, 0, reinterpret_cast<sockaddr *>(&from), &fromLen);
                if (n == SOCKET_ERROR)
                {
                    if (WSAGetLastError() == WSAEMSGSIZE)
                        continue;
                    break;
                }
                HandlePacket(from, buf, static_cast<size_t>(n), now);
            }
            Tick(now);
        }
    }

    void SendRaw(const sockaddr_in &to, const std::string &packet)
    {
        if (loss_ > 0 && std::uniform_real_distribution<float>(0, 1)(rng_) < loss_)
            return;
        sendto(sock_, packet.data(), static_cast<int>(packet.size()), 0, reinterpret_cast<const sockaddr *>(&to),
               sizeof to);
    }

    void SendReliable(Peer &p, const std::string &payload)
    {
        size_t off = 0;
        do
        {
            size_t n = std::min(kMaxFragment, payload.size() - off);
            uint32_t seq = p.nextOutSeq++;
            Writer w = Begin(kRel);
            w.U32(seq);
            w.U8(off + n == payload.size() ? kFlagLast : 0);
            w.Raw(payload.data() + off, n);
            off += n;

            Pending &pending = p.unacked[seq];
            pending.packet = std::move(w.Data());
            pending.lastSend = Clock::now();
            SendRaw(p.addr, pending.packet);
        } while (off < payload.size());
    }

    void Push(int from, Writer &w) { inbox_.push_back(Message{from, std::move(w.Data())}); }

    Peer *FindPeer(const sockaddr_in &addr)
    {
        for (auto &[id, p] : peers_)
            if (SameAddr(p.addr, addr))
                return &p;
        return nullptr;
    }

    void HandlePacket(const sockaddr_in &from, const char *data, size_t len, TimePoint now)
    {
        Reader r(data, len);
        uint8_t c, n, ver, kind;
        if (!r.U8(c) || !r.U8(n) || !r.U8(ver) || !r.U8(kind) || c != 'C' || n != 'N')
            return;
        Peer *p = FindPeer(from);

        if (ver != kProtoVersion && kind != kReject)
        {
            if (state_ == State::Hosting && kind == kHello)
            {
                Writer w = Begin(kReject);
                w.U8(kReasonVersion);
                SendRaw(from, w.Data());
            }
            return;
        }
        if (state_ == State::Hosting && kind == kHello)
        {
            OnHello(from, p, r, now);
            return;
        }
        if (state_ == State::Connecting)
        {
            if (p && (kind == kWelcome || kind == kReject))
                OnHandshakeReply(*p, kind, r, now);
            return;
        }
        if (!p || (state_ != State::Connected && state_ != State::Hosting))
            return;

        p->lastRecv = now;
        switch (kind)
        {
        case kRel:
            OnReliable(*p, r);
            break;
        case kUnrel: {
            Writer w;
            w.Data() = r.Rest();
            Push(p->id, w);
            break;
        }
        case kAck: {
            uint32_t seq;
            if (r.U32(seq))
                p->unacked.erase(seq);
            break;
        }
        case kPing: {
            uint64_t t;
            if (r.U64(t))
            {
                Writer w = Begin(kPong);
                w.U64(t);
                SendRaw(p->addr, w.Data());
            }
            break;
        }
        case kPong: {
            uint64_t t;
            if (r.U64(t))
                p->rttMs = static_cast<int>(NowMs() - t);
            break;
        }
        case kBye:
            if (state_ == State::Hosting)
                DropPeer(p->id, kReasonClosed);
            else
                LoseHost(kReasonClosed);
            break;
        default:
            break;
        }
    }

    void OnHello(const sockaddr_in &from, Peer *p, Reader &r, TimePoint now)
    {
        uint32_t nonce;
        std::string name;
        if (!r.U32(nonce) || !r.Str(name))
            return;
        if (p && p->nonce == nonce)
        {
            SendWelcome(*p); // our Welcome was lost
            return;
        }
        if (p)
            DropPeer(p->id, kReasonClosed); // same address, new session: the old one is gone
        if (static_cast<int>(peers_.size()) >= maxPeers_)
        {
            Writer w = Begin(kReject);
            w.U8(kReasonFull);
            SendRaw(from, w.Data());
            return;
        }

        int id = nextPeerId_++;
        Peer &added = peers_[id];
        added.id = id;
        added.addr = from;
        added.name = name;
        added.nonce = nonce;
        added.lastRecv = added.lastPing = now;
        SendWelcome(added);

        Writer ev = Event(kEvtPeerJoined);
        WriteInt(ev, id);
        WriteString(ev, name);
        Push(id, ev);
    }

    void SendWelcome(const Peer &p)
    {
        Writer w = Begin(kWelcome);
        w.U32(p.nonce);
        w.U16(static_cast<uint16_t>(p.id));
        w.Str(name_);
        SendRaw(p.addr, w.Data());
    }

    void OnHandshakeReply(Peer &host, uint8_t kind, Reader &r, TimePoint now)
    {
        if (kind == kReject)
        {
            uint8_t reason = kReasonUnreachable;
            r.U8(reason);
            FailConnect(reason);
            return;
        }
        uint32_t nonce;
        uint16_t id;
        std::string hostName;
        if (!r.U32(nonce) || !r.U16(id) || !r.Str(hostName) || nonce != nonce_)
            return;
        myId_ = id;
        host.name = hostName;
        host.lastRecv = host.lastPing = now;
        state_ = State::Connected;

        Writer ev = Event(kEvtConnected);
        WriteInt(ev, id);
        Push(kHostId, ev);
    }

    void OnReliable(Peer &p, Reader &r)
    {
        uint32_t seq;
        uint8_t flags;
        if (!r.U32(seq) || !r.U8(flags))
            return;
        Writer ack = Begin(kAck);
        ack.U32(seq);
        SendRaw(p.addr, ack.Data());

        if (seq < p.nextInSeq)
            return; // duplicate
        if (seq > p.nextInSeq)
        {
            if (p.early.size() < kMaxEarly)
                p.early.emplace(seq, std::make_pair(flags, r.Rest()));
            return;
        }

        if (!Deliver(p, flags, r.Rest()))
            return;
        for (auto it = p.early.find(p.nextInSeq); it != p.early.end(); it = p.early.find(p.nextInSeq))
        {
            auto [f, body] = std::move(it->second);
            p.early.erase(it);
            if (!Deliver(p, f, std::move(body)))
                return;
        }
    }

    // Appends an in-order fragment; returns false if the peer was dropped (p is then invalid).
    bool Deliver(Peer &p, uint8_t flags, std::string body)
    {
        ++p.nextInSeq;
        if (p.partial.size() + body.size() > kMaxMessage)
        {
            if (state_ == State::Hosting)
                DropPeer(p.id, kReasonClosed);
            else
                LoseHost(kReasonClosed);
            return false;
        }
        p.partial += body;
        if (flags & kFlagLast)
        {
            inbox_.push_back(Message{p.id, std::move(p.partial)});
            p.partial.clear();
        }
        return true;
    }

    void Tick(TimePoint now)
    {
        if (state_ == State::Connecting)
        {
            if (now - connectStart_ >= kTimeout)
                FailConnect(kReasonUnreachable);
            else if (now - lastHello_ >= kHelloInterval)
            {
                lastHello_ = now;
                Writer w = Begin(kHello);
                w.U32(nonce_);
                w.Str(name_);
                SendRaw(peers_[kHostId].addr, w.Data());
            }
            return;
        }
        if (state_ != State::Connected && state_ != State::Hosting)
            return;

        std::vector<int> timedOut;
        for (auto &[id, p] : peers_)
        {
            if (now - p.lastRecv >= kTimeout)
            {
                timedOut.push_back(id);
                continue;
            }
            int budget = kResendBudget;
            for (auto &[seq, pending] : p.unacked)
            {
                if (budget == 0)
                    break;
                if (now - pending.lastSend >= kResendInterval)
                {
                    pending.lastSend = now;
                    SendRaw(p.addr, pending.packet);
                    --budget;
                }
            }
            if (now - p.lastPing >= kPingInterval)
            {
                p.lastPing = now;
                Writer w = Begin(kPing);
                w.U64(NowMs());
                SendRaw(p.addr, w.Data());
            }
        }
        for (int id : timedOut)
        {
            if (state_ == State::Hosting)
                DropPeer(id, kReasonTimeout);
            else
            {
                LoseHost(kReasonTimeout);
                break;
            }
        }
    }

    void DropPeer(int id, int32_t reason)
    {
        peers_.erase(id);
        Writer ev = Event(kEvtPeerLeft);
        WriteInt(ev, id);
        WriteInt(ev, reason);
        Push(id, ev);
    }

    void LoseHost(int32_t reason)
    {
        peers_.clear();
        state_ = State::Offline;
        myId_ = -1;
        Writer ev = Event(kEvtDisconnected);
        WriteInt(ev, reason);
        Push(kHostId, ev);
    }

    void FailConnect(int32_t reason)
    {
        peers_.clear();
        state_ = State::Offline;
        Writer ev = Event(kEvtConnectFailed);
        WriteInt(ev, reason);
        Push(kHostId, ev);
    }

    mutable std::mutex mu_;
    bool wsaOk_ = false;
    SOCKET sock_ = INVALID_SOCKET;
    std::thread thread_;
    std::atomic<bool> stop_{false};

    State state_ = State::Offline;
    int myId_ = -1;
    int maxPeers_ = 0;
    int nextPeerId_ = 1;
    std::string name_;
    std::map<int, Peer> peers_;
    std::deque<Message> inbox_;

    uint32_t nonce_ = 0;
    TimePoint connectStart_{};
    TimePoint lastHello_{};

    float loss_ = 0;
    std::mt19937 rng_;
};

} // namespace

std::unique_ptr<Session> CreateUdpSession() { return std::make_unique<UdpSession>(); }

static void (*g_sessionLog)(const char *) = nullptr;

void SetSessionLog(void (*log)(const char *line)) { g_sessionLog = log; }

void SessionLog(const char *fmt, ...)
{
    if (!g_sessionLog)
        return;
    char line[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    g_sessionLog(line);
}

} // namespace coopnet
