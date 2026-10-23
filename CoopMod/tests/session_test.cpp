// Loopback test of the UDP session: one host, three clients, 30% simulated packet loss.
#include "../src/session.h"
#include "../src/wire.h"

#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace coopnet;

static int g_failures = 0;
#define CHECK(cond)                                                                                                   \
    do                                                                                                                \
    {                                                                                                                 \
        if (!(cond))                                                                                                  \
        {                                                                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                               \
            ++g_failures;                                                                                             \
        }                                                                                                             \
    } while (0)

struct Got
{
    int from;
    uint16_t type;
    Reader fields;
    std::string payload;
};

struct Node
{
    const char *name;
    std::unique_ptr<Session> s = CreateUdpSession();
    std::vector<Got> got;

    void Pump()
    {
        Message m;
        while (s->Poll(m))
        {
            uint16_t type = 0;
            Reader(m.payload).U16(type);
            got.push_back(Got{m.from, type, {}, std::move(m.payload)});
        }
    }
    // Moves out received messages of the given type, with `fields` positioned after the type.
    std::vector<Got> Take(uint16_t type)
    {
        std::vector<Got> out, rest;
        for (auto &g : got)
            (g.type == type ? out : rest).push_back(std::move(g));
        got = std::move(rest);
        for (auto &g : out)
        {
            g.fields = Reader(g.payload);
            uint16_t t;
            g.fields.U16(t);
        }
        return out;
    }
    size_t Count(uint16_t type) const
    {
        size_t n = 0;
        for (auto &g : got)
            n += g.type == type;
        return n;
    }
};

static bool WaitFor(std::vector<Node *> nodes, int timeoutMs, const std::function<bool()> &done)
{
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end)
    {
        for (Node *n : nodes)
            n->Pump();
        if (done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

static std::string Msg(uint16_t type, const std::function<void(Writer &)> &fill)
{
    Writer w;
    w.U16(type);
    fill(w);
    return std::move(w.Data());
}

int main()
{
    const uint16_t port = 47600;
    Node host{"host"}, alice{"alice"}, bob{"bob"}, carl{"carl"};
    std::vector<Node *> all = {&host, &alice, &bob, &carl};
    std::string err;

    for (Node *n : all)
        n->s->SetSimulatedLoss(0.3f);

    std::printf("[1] handshake, max 2 peers\n");
    CHECK(host.s->Host(port, 2, "Host", err));
    CHECK(alice.s->Connect("127.0.0.1", port, "Alice", err));
    CHECK(WaitFor(all, 5000, [&] { return alice.Count(kEvtConnected) == 1; }));
    CHECK(bob.s->Connect("localhost", port, "Bob", err));
    CHECK(WaitFor(all, 5000, [&] { return bob.Count(kEvtConnected) == 1 && host.Count(kEvtPeerJoined) == 2; }));
    CHECK(carl.s->Connect("127.0.0.1", port, "Carl", err));
    CHECK(WaitFor(all, 8000, [&] { return carl.Count(kEvtConnectFailed) == 1; }));
    {
        auto failed = carl.Take(kEvtConnectFailed);
        int32_t reason = -1;
        CHECK(!failed.empty() && ReadInt(failed[0].fields, reason) && reason == kReasonFull);
        CHECK(carl.s->GetState() == State::Offline);
    }
    int aliceId = alice.s->MyId(), bobId = bob.s->MyId();
    CHECK(aliceId > 0 && bobId > 0 && aliceId != bobId);
    CHECK(host.s->Peers().size() == 2);
    CHECK(host.s->PeerName(aliceId) == "Alice");
    CHECK(alice.s->PeerName(kHostId) == "Host");
    {
        auto joined = host.Take(kEvtPeerJoined);
        int32_t id = -1;
        std::string name;
        CHECK(joined.size() == 2 && ReadInt(joined[0].fields, id) && ReadString(joined[0].fields, name));
        CHECK(id == aliceId && name == "Alice");
    }
    alice.Take(kEvtConnected);
    bob.Take(kEvtConnected);

    std::printf("[2] reliable ordered client->host, 300 small + 100 KB message\n");
    const int kSmall = 300;
    for (int i = 0; i < kSmall; ++i)
        CHECK(alice.s->Send(kHostId, Msg(100, [&](Writer &w) { WriteInt(w, i); WriteFloat(w, i * 0.5f); }), true));
    std::string big(100 * 1024, '\0');
    for (size_t i = 0; i < big.size(); ++i)
        big[i] = static_cast<char>('a' + i % 26);
    CHECK(alice.s->Send(kHostId, Msg(101, [&](Writer &w) { w.Raw(big.data(), big.size()); }), true));
    CHECK(WaitFor(all, 15000, [&] { return host.Count(100) == kSmall && host.Count(101) == 1; }));
    {
        auto small = host.Take(100);
        bool ordered = small.size() == kSmall;
        for (int i = 0; ordered && i < kSmall; ++i)
        {
            int32_t v;
            float f;
            ordered = small[i].from == aliceId && ReadInt(small[i].fields, v) && v == i &&
                      ReadFloat(small[i].fields, f) && f == i * 0.5f;
        }
        CHECK(ordered);
        auto bigGot = host.Take(101);
        CHECK(bigGot.size() == 1 && bigGot[0].fields.Rest() == big);
    }

    std::printf("[3] reliable broadcast host->clients\n");
    for (int i = 0; i < 100; ++i)
        CHECK(host.s->Send(kBroadcast, Msg(102, [&](Writer &w) { WriteString(w, "n" + std::to_string(i)); }), true));
    CHECK(WaitFor(all, 15000, [&] { return alice.Count(102) == 100 && bob.Count(102) == 100; }));
    for (Node *n : {&alice, &bob})
    {
        auto got = n->Take(102);
        bool ordered = got.size() == 100;
        for (int i = 0; ordered && i < 100; ++i)
        {
            std::string s;
            ordered = ReadString(got[i].fields, s) && s == "n" + std::to_string(i);
        }
        CHECK(ordered);
    }

    std::printf("[4] unreliable, client may not address another client\n");
    for (int i = 0; i < 200; ++i)
        alice.s->Send(kHostId, Msg(103, [&](Writer &w) { WriteInt(w, i); }), false);
    WaitFor(all, 1000, [] { return false; });
    size_t unrel = host.Take(103).size();
    std::printf("    unreliable delivered: %zu/200\n", unrel);
    CHECK(unrel > 50 && unrel < 200);
    CHECK(!alice.s->Send(bobId, Msg(104, [](Writer &) {}), true));
    CHECK(!alice.s->Send(kHostId, std::string(2000, 'x'), false)); // too big for one packet

    std::printf("[5] ping\n");
    CHECK(WaitFor(all, 5000, [&] { return host.s->PingMs(aliceId) >= 0 && alice.s->PingMs(kHostId) >= 0; }));

    std::printf("[6] graceful disconnect\n");
    for (Node *n : all)
        n->s->SetSimulatedLoss(0);
    bob.s->Disconnect();
    CHECK(bob.s->GetState() == State::Offline);
    CHECK(WaitFor(all, 3000, [&] { return host.Count(kEvtPeerLeft) == 1; }));
    {
        auto left = host.Take(kEvtPeerLeft);
        int32_t id = -1, reason = -1;
        CHECK(left.size() == 1 && ReadInt(left[0].fields, id) && ReadInt(left[0].fields, reason));
        CHECK(id == bobId && reason == kReasonClosed);
    }

    std::printf("[7] timeout (alice goes silent: host drops her after ~10 s, she notices ~10 s later)\n");
    alice.s->SetSimulatedLoss(1.0f);
    CHECK(WaitFor(all, 13000, [&] { return host.Count(kEvtPeerLeft) == 1; }));
    CHECK(WaitFor(all, 13000, [&] { return alice.Count(kEvtDisconnected) == 1; }));
    {
        auto left = host.Take(kEvtPeerLeft);
        int32_t id = -1, reason = -1;
        CHECK(left.size() == 1 && ReadInt(left[0].fields, id) && ReadInt(left[0].fields, reason));
        CHECK(id == aliceId && reason == kReasonTimeout);
        auto lost = alice.Take(kEvtDisconnected);
        CHECK(lost.size() == 1 && ReadInt(lost[0].fields, reason) && reason == kReasonTimeout);
    }
    CHECK(host.s->Peers().empty());

    std::printf("[8] host closes, client notified\n");
    alice.s->SetSimulatedLoss(0);
    CHECK(alice.s->Connect("127.0.0.1", port, "Alice", err));
    CHECK(WaitFor(all, 5000, [&] { return alice.Count(kEvtConnected) == 1; }));
    host.s->Disconnect();
    CHECK(WaitFor(all, 3000, [&] { return alice.Count(kEvtDisconnected) == 1; }));
    CHECK(alice.s->GetState() == State::Offline);

    std::printf(g_failures ? "\n%d CHECK(S) FAILED\n" : "\nALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
