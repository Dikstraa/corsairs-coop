// Steam implementation of coopnet::Session, for playing over the internet through Spacewar (AppID 480).
//
// Players find each other through a public Steam lobby tagged "corsairs_coop" (Spacewar is shared by
// many hobby projects, so untagged lobbies are ignored), or through a Steam friends invite. Traffic
// goes over ISteamNetworkingMessages: P2P with Valve relays, so no port forwarding; Steam provides
// reliability and ordering. Everything runs on the thread that calls Poll(), which also pumps the
// Steam callbacks.
//
// Message: u8 kind, then kind-specific data.
#include "session.h"
#include "wire.h"

#include <steam/steam_api.h>
#include <steam/isteamnetworkingmessages.h>

#include <windows.h>
#pragma comment(lib, "advapi32.lib") // RegGetValueA

#include <chrono>
#include <cstdio>
#include <deque>
#include <map>
#include <random>

namespace coopnet
{
namespace
{

constexpr uint32_t kAppId = 480; // Spacewar
constexpr uint32_t kProtoVersion = 1;
constexpr const char *kLobbyTag = "corsairs_coop";
constexpr int kChannel = 0;
constexpr double kHelloInterval = 0.5;
constexpr double kPingInterval = 1.0;
constexpr double kTimeout = 15.0;
constexpr double kConnectTimeout = 20.0;

enum Kind : uint8_t
{
    kHello = 1, // u32 proto, u32 nonce, str name
    kWelcome,   // u32 nonce, u16 id, str hostName
    kReject,    // u8 reason
    kData,      // message bytes
    kBye,
    kPing, // u64 sender time ms
    kPong, // u64 echoed time ms
};

double Now()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

uint64_t NowMs() { return static_cast<uint64_t>(Now() * 1000.0); }

// Steam names are UTF-8; the game shows Windows-1251.
std::string PersonaName()
{
    const char *utf8 = SteamFriends()->GetPersonaName();
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    std::wstring wide(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide.data(), n);
    int m = WideCharToMultiByte(1251, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(m, '\0');
    WideCharToMultiByte(1251, 0, wide.c_str(), -1, out.data(), m, nullptr, nullptr);
    out.resize(m > 0 ? m - 1 : 0);
    return out;
}

SteamNetworkingIdentity Identity(CSteamID id)
{
    SteamNetworkingIdentity identity;
    identity.SetSteamID(id);
    return identity;
}

struct Peer
{
    CSteamID steamId;
    std::string name;
    uint32_t nonce = 0;
    double lastRecv = 0;
    double lastPing = 0;
    int rttMs = -1;
};

class SteamSession final : public Session
{
  public:
    SteamSession() : rng_(std::random_device{}()) {}
    ~SteamSession() override { Disconnect(); }

    bool Host(uint16_t, int maxPeers, const std::string &name, std::string &err) override
    {
        Disconnect();
        SteamAPICall_t call = SteamMatchmaking()->CreateLobby(k_ELobbyTypePublic, maxPeers + 1);
        if (call == k_uAPICallInvalid)
        {
            err = "CreateLobby failed";
            return false;
        }
        lobbyCreated_.Set(call, this, &SteamSession::OnLobbyCreated);
        name_ = name.empty() ? PersonaName() : name;
        maxPeers_ = maxPeers;
        state_ = State::Hosting;
        myId_ = kHostId;
        nextPeerId_ = 1;
        return true;
    }

    // target: "" = a lobby of a friend (else any tagged lobby), "lobby:<id>" = that lobby,
    // anything else = the lobby whose host's name contains it.
    bool Connect(const std::string &target, uint16_t, const std::string &name, std::string &err) override
    {
        Disconnect();
        name_ = name.empty() ? PersonaName() : name;
        target_ = target;
        state_ = State::Connecting;
        myId_ = -1;
        nonce_ = static_cast<uint32_t>(rng_()) | 1;
        connectStart_ = Now();
        if (target.rfind("lobby:", 0) == 0)
        {
            JoinLobby(CSteamID(static_cast<uint64>(_strtoui64(target.c_str() + 6, nullptr, 10))));
            return true;
        }
        SteamMatchmaking()->AddRequestLobbyListStringFilter(kLobbyTag, "1", k_ELobbyComparisonEqual);
        SteamMatchmaking()->AddRequestLobbyListDistanceFilter(k_ELobbyDistanceFilterWorldwide);
        SteamAPICall_t call = SteamMatchmaking()->RequestLobbyList();
        if (call == k_uAPICallInvalid)
        {
            err = "RequestLobbyList failed";
            state_ = State::Offline;
            return false;
        }
        lobbyList_.Set(call, this, &SteamSession::OnLobbyList);
        return true;
    }

    std::string LocalUid() const override
    {
        return "steam" + std::to_string(SteamUser()->GetSteamID().ConvertToUint64());
    }

    bool OpenInviteDialog() override
    {
        if (state_ == State::Hosting && lobby_.IsValid())
            SteamFriends()->ActivateGameOverlayInviteDialog(lobby_);
        else
            SteamFriends()->ActivateGameOverlay("Friends");
        return true;
    }

    void Disconnect() override
    {
        if (state_ == State::Connected || state_ == State::Hosting)
            for (auto &[id, p] : peers_)
            {
                SendKind(p.steamId, kBye, nullptr, 0, true);
                SteamNetworkingMessages()->CloseSessionWithUser(Identity(p.steamId));
            }
        if (lobby_.IsValid())
            SteamMatchmaking()->LeaveLobby(lobby_);
        lobby_ = CSteamID();
        peers_.clear();
        inbox_.clear();
        state_ = State::Offline;
        myId_ = -1;
    }

    State GetState() const override { return state_; }
    int MyId() const override { return myId_; }

    std::vector<int> Peers() const override
    {
        std::vector<int> ids;
        if (state_ == State::Connected || state_ == State::Hosting)
            for (auto &[id, p] : peers_)
                ids.push_back(id);
        return ids;
    }

    std::string PeerName(int id) const override
    {
        if (id == myId_)
            return name_;
        auto it = peers_.find(id);
        return it == peers_.end() ? std::string() : it->second.name;
    }

    int PingMs(int id) const override
    {
        auto it = peers_.find(id);
        return it == peers_.end() ? -1 : it->second.rttMs;
    }

    bool Send(int to, const std::string &payload, bool reliable) override
    {
        if (state_ != State::Connected && state_ != State::Hosting)
            return false;
        bool sent = false;
        for (auto &[id, p] : peers_)
        {
            if (to != kBroadcast && to != id)
                continue;
            SendKind(p.steamId, kData, payload.data(), payload.size(), reliable);
            sent = true;
        }
        return sent;
    }

    bool Poll(Message &out) override
    {
        SteamAPI_RunCallbacks();
        if (!overlayLogged_)
        {
            bool on = SteamUtils()->IsOverlayEnabled();
            if (on || Now() - created_ > 60.0)
            {
                overlayLogged_ = true;
                SessionLog(on ? "steam: overlay enabled (Shift+Tab)" : "steam: overlay still not enabled after 60 s");
            }
        }
        Receive();
        Tick();
        if (inbox_.empty())
            return false;
        out = std::move(inbox_.front());
        inbox_.pop_front();
        return true;
    }

    void SetSimulatedLoss(float) override {}

  private:
    void SendKind(CSteamID to, Kind kind, const void *data, size_t size, bool reliable)
    {
        std::string packet(1, static_cast<char>(kind));
        packet.append(static_cast<const char *>(data), size);
        SteamNetworkingMessages()->SendMessageToUser(
            Identity(to), packet.data(), static_cast<uint32>(packet.size()),
            (reliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_Unreliable) |
                k_nSteamNetworkingSend_AutoRestartBrokenSession,
            kChannel);
    }

    void SendWriter(CSteamID to, Kind kind, Writer &w) { SendKind(to, kind, w.Data().data(), w.Data().size(), true); }

    void Push(int from, Writer &w) { inbox_.push_back(Message{from, std::move(w.Data())}); }

    Peer *FindPeer(CSteamID id, int *outId = nullptr)
    {
        for (auto &[pid, p] : peers_)
            if (p.steamId == id)
            {
                if (outId)
                    *outId = pid;
                return &p;
            }
        return nullptr;
    }

    // ---- lobby ----

    void OnLobbyCreated(LobbyCreated_t *r, bool ioFailure)
    {
        if (state_ != State::Hosting)
            return;
        if (ioFailure || r->m_eResult != k_EResultOK)
        {
            SessionLog("steam: CreateLobby failed, result %d", ioFailure ? -1 : static_cast<int>(r->m_eResult));
            Fail(kEvtConnectFailed, kReasonUnreachable);
            return;
        }
        lobby_ = CSteamID(r->m_ulSteamIDLobby);
        SessionLog("steam: lobby %llu created", lobby_.ConvertToUint64());
        SteamMatchmaking()->SetLobbyData(lobby_, kLobbyTag, "1");
        SteamMatchmaking()->SetLobbyData(lobby_, "host", name_.c_str());
        SteamMatchmaking()->SetLobbyData(lobby_, "proto", std::to_string(kProtoVersion).c_str());
    }

    void OnLobbyList(LobbyMatchList_t *r, bool ioFailure)
    {
        if (state_ != State::Connecting)
            return;
        CSteamID best;
        bool bestIsFriend = false;
        SessionLog("steam: %u tagged lobbies found", ioFailure ? 0u : r->m_nLobbiesMatching);
        for (uint32 i = 0; !ioFailure && i < r->m_nLobbiesMatching; ++i)
        {
            CSteamID lobby = SteamMatchmaking()->GetLobbyByIndex(i);
            std::string host = SteamMatchmaking()->GetLobbyData(lobby, "host");
            CSteamID owner = SteamMatchmaking()->GetLobbyOwner(lobby);
            bool isFriend = owner.IsValid() && SteamFriends()->HasFriend(owner, k_EFriendFlagImmediate);
            if (!target_.empty() && host.find(target_) == std::string::npos)
                continue;
            if (!best.IsValid() || (isFriend && !bestIsFriend))
            {
                best = lobby;
                bestIsFriend = isFriend;
            }
        }
        if (!best.IsValid())
        {
            Fail(kEvtConnectFailed, kReasonUnreachable);
            return;
        }
        JoinLobby(best);
    }

    void JoinLobby(CSteamID lobby)
    {
        SteamAPICall_t call = SteamMatchmaking()->JoinLobby(lobby);
        lobbyEntered_.Set(call, this, &SteamSession::OnLobbyEntered);
    }

    void OnLobbyEntered(LobbyEnter_t *r, bool ioFailure)
    {
        if (state_ != State::Connecting)
            return;
        if (ioFailure || r->m_EChatRoomEnterResponse != k_EChatRoomEnterResponseSuccess)
        {
            Fail(kEvtConnectFailed, kReasonUnreachable);
            return;
        }
        lobby_ = CSteamID(r->m_ulSteamIDLobby);
        SessionLog("steam: entered lobby %llu", lobby_.ConvertToUint64());
        Peer host;
        host.steamId = SteamMatchmaking()->GetLobbyOwner(lobby_);
        if (host.steamId == SteamUser()->GetSteamID())
        {
            SessionLog("steam: that lobby is our own (same Steam account), can't join it");
            Fail(kEvtConnectFailed, kReasonUnreachable);
            return;
        }
        host.name = SteamMatchmaking()->GetLobbyData(lobby_, "host");
        host.lastRecv = Now();
        peers_[kHostId] = host;
        lastHello_ = 0;
    }

    STEAM_CALLBACK(SteamSession, OnJoinRequested, GameLobbyJoinRequested_t);
    STEAM_CALLBACK(SteamSession, OnLobbyChatUpdate, LobbyChatUpdate_t);
    STEAM_CALLBACK(SteamSession, OnSessionRequest, SteamNetworkingMessagesSessionRequest_t);
    STEAM_CALLBACK(SteamSession, OnSessionFailed, SteamNetworkingMessagesSessionFailed_t);

    // ---- traffic ----

    void Receive()
    {
        SteamNetworkingMessage_t *msgs[64];
        int n;
        while ((n = SteamNetworkingMessages()->ReceiveMessagesOnChannel(kChannel, msgs, 64)) > 0)
            for (int i = 0; i < n; ++i)
            {
                Handle(msgs[i]->m_identityPeer.GetSteamID(), static_cast<const char *>(msgs[i]->m_pData),
                       static_cast<size_t>(msgs[i]->m_cbSize));
                msgs[i]->Release();
            }
    }

    void Handle(CSteamID from, const char *data, size_t len)
    {
        if (len < 1)
            return;
        Kind kind = static_cast<Kind>(data[0]);
        Reader r(data + 1, len - 1);
        int id = -1;
        Peer *p = FindPeer(from, &id);
        double now = Now();

        if (state_ == State::Hosting && kind == kHello)
        {
            OnHello(from, p, r, now);
            return;
        }
        if (state_ == State::Connecting)
        {
            if (p && id == kHostId)
                OnHandshakeReply(*p, kind, r, now);
            return;
        }
        if (!p)
            return;
        p->lastRecv = now;
        switch (kind)
        {
        case kData: {
            Writer w;
            w.Data() = r.Rest();
            Push(id, w);
            break;
        }
        case kPing: {
            uint64_t t;
            if (r.U64(t))
                SendKind(from, kPong, &t, sizeof t, false);
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
                DropPeer(id, kReasonClosed);
            else
                Fail(kEvtDisconnected, kReasonClosed);
            break;
        default:
            break;
        }
    }

    void OnHello(CSteamID from, Peer *p, Reader &r, double now)
    {
        uint32_t proto, nonce;
        std::string name;
        if (!r.U32(proto) || !r.U32(nonce) || !r.Str(name))
            return;
        if (proto != kProtoVersion)
        {
            uint8_t reason = kReasonVersion;
            SendKind(from, kReject, &reason, 1, true);
            return;
        }
        if (p && p->nonce == nonce)
        {
            SendWelcome(*p, FindId(from));
            return;
        }
        if (p)
            DropPeer(FindId(from), kReasonClosed);
        if (static_cast<int>(peers_.size()) >= maxPeers_)
        {
            uint8_t reason = kReasonFull;
            SendKind(from, kReject, &reason, 1, true);
            return;
        }
        int id = nextPeerId_++;
        Peer &added = peers_[id];
        added.steamId = from;
        added.name = name;
        added.nonce = nonce;
        added.lastRecv = added.lastPing = now;
        SendWelcome(added, id);

        Writer ev;
        ev.U16(kEvtPeerJoined);
        WriteInt(ev, id);
        WriteString(ev, name);
        Push(id, ev);
    }

    int FindId(CSteamID from)
    {
        int id = -1;
        FindPeer(from, &id);
        return id;
    }

    void SendWelcome(const Peer &p, int id)
    {
        Writer w;
        w.U32(p.nonce);
        w.U16(static_cast<uint16_t>(id));
        w.Str(name_);
        SendWriter(p.steamId, kWelcome, w);
    }

    void OnHandshakeReply(Peer &host, Kind kind, Reader &r, double now)
    {
        if (kind == kReject)
        {
            uint8_t reason = kReasonUnreachable;
            r.U8(reason);
            Fail(kEvtConnectFailed, reason);
            return;
        }
        uint32_t nonce;
        uint16_t id;
        std::string hostName;
        if (kind != kWelcome || !r.U32(nonce) || !r.U16(id) || !r.Str(hostName) || nonce != nonce_)
            return;
        myId_ = id;
        host.name = hostName;
        host.lastRecv = host.lastPing = now;
        state_ = State::Connected;
        Writer ev;
        ev.U16(kEvtConnected);
        WriteInt(ev, id);
        Push(kHostId, ev);
    }

    void Tick()
    {
        double now = Now();
        if (state_ == State::Connecting)
        {
            if (now - connectStart_ >= kConnectTimeout)
                Fail(kEvtConnectFailed, kReasonUnreachable);
            else if (peers_.count(kHostId) && now - lastHello_ >= kHelloInterval)
            {
                lastHello_ = now;
                Writer w;
                w.U32(kProtoVersion);
                w.U32(nonce_);
                w.Str(name_);
                SendWriter(peers_[kHostId].steamId, kHello, w);
            }
            return;
        }
        if (state_ != State::Connected && state_ != State::Hosting)
            return;
        std::vector<int> timedOut;
        for (auto &[id, p] : peers_)
        {
            if (now - p.lastRecv >= kTimeout)
                timedOut.push_back(id);
            else if (now - p.lastPing >= kPingInterval)
            {
                p.lastPing = now;
                uint64_t t = NowMs();
                SendKind(p.steamId, kPing, &t, sizeof t, false);
            }
        }
        for (int id : timedOut)
        {
            if (state_ == State::Hosting)
                DropPeer(id, kReasonTimeout);
            else
            {
                Fail(kEvtDisconnected, kReasonTimeout);
                break;
            }
        }
    }

    void DropPeer(int id, int32_t reason)
    {
        auto it = peers_.find(id);
        if (it == peers_.end())
            return;
        SteamNetworkingMessages()->CloseSessionWithUser(Identity(it->second.steamId));
        peers_.erase(it);
        Writer ev;
        ev.U16(kEvtPeerLeft);
        WriteInt(ev, id);
        WriteInt(ev, reason);
        Push(id, ev);
    }

    // Ends a client session (or a host whose lobby failed) with the given event.
    void Fail(uint16_t event, int32_t reason)
    {
        if (lobby_.IsValid())
            SteamMatchmaking()->LeaveLobby(lobby_);
        lobby_ = CSteamID();
        peers_.clear();
        state_ = State::Offline;
        myId_ = -1;
        Writer ev;
        ev.U16(event);
        WriteInt(ev, reason);
        Push(kHostId, ev);
    }

    State state_ = State::Offline;
    int myId_ = -1;
    int maxPeers_ = 0;
    int nextPeerId_ = 1;
    std::string name_;
    std::string target_;
    CSteamID lobby_;
    std::map<int, Peer> peers_;
    std::deque<Message> inbox_;
    uint32_t nonce_ = 0;
    double connectStart_ = 0;
    double lastHello_ = 0;
    std::mt19937 rng_;
    double created_ = Now();
    bool overlayLogged_ = false; // when the overlay comes up (or doesn't), logged once

    CCallResult<SteamSession, LobbyCreated_t> lobbyCreated_;
    CCallResult<SteamSession, LobbyMatchList_t> lobbyList_;
    CCallResult<SteamSession, LobbyEnter_t> lobbyEntered_;
};

// A friend's invite accepted in the Steam overlay or friends list.
void SteamSession::OnJoinRequested(GameLobbyJoinRequested_t *r)
{
    // A host with players doesn't drop its game for somebody else's; an empty one (mode=auto found no
    // friend's game and hosts) leaves its lobby for the friend's (Connect disconnects first).
    if (state_ == State::Hosting && !peers_.empty())
        return;
    std::string err;
    Connect("lobby:" + std::to_string(r->m_steamIDLobby.ConvertToUint64()), 0, name_, err);
}

void SteamSession::OnLobbyChatUpdate(LobbyChatUpdate_t *r)
{
    if (CSteamID(r->m_ulSteamIDLobby) != lobby_)
        return;
    const uint32 gone = k_EChatMemberStateChangeLeft | k_EChatMemberStateChangeDisconnected |
                        k_EChatMemberStateChangeKicked | k_EChatMemberStateChangeBanned;
    if (!(r->m_rgfChatMemberStateChange & gone))
        return;
    CSteamID user(r->m_ulSteamIDUserChanged);
    int id = FindId(user);
    if (state_ == State::Hosting && id > 0)
        DropPeer(id, kReasonClosed);
    else if (state_ == State::Connected && id == kHostId)
        Fail(kEvtDisconnected, kReasonClosed);
}

void SteamSession::OnSessionRequest(SteamNetworkingMessagesSessionRequest_t *r)
{
    CSteamID from = r->m_identityRemote.GetSteamID();
    bool fromHost = peers_.count(kHostId) && peers_[kHostId].steamId == from;
    if (state_ == State::Hosting || fromHost)
        SteamNetworkingMessages()->AcceptSessionWithUser(r->m_identityRemote);
}

void SteamSession::OnSessionFailed(SteamNetworkingMessagesSessionFailed_t *r)
{
    int id = FindId(r->m_info.m_identityRemote.GetSteamID());
    if (id < 0)
        return;
    if (state_ == State::Hosting)
        DropPeer(id, kReasonTimeout);
    else
        Fail(state_ == State::Connecting ? kEvtConnectFailed : kEvtDisconnected, kReasonUnreachable);
}

} // namespace

namespace
{

// Steam API up as Spacewar; once per process.
bool InitSteamApi(std::string &err)
{
    static bool initialized = false;
    if (initialized)
        return true;
    // steam_api64.dll is delay-loaded: without it (or without Steam running) the game still works over UDP.
    if (!LoadLibraryA("steam_api64.dll"))
    {
        err = "steam_api64.dll not found next to Maelstrom.exe";
        return false;
    }
    if (GetFileAttributesA("steam_appid.txt") == INVALID_FILE_ATTRIBUTES)
        if (FILE *f = fopen("steam_appid.txt", "w"))
        {
            fprintf(f, "%u", kAppId);
            fclose(f);
        }
    // Launched from the Steam library as a non-Steam game, Steam hands the process the shortcut's own
    // ids; the API reads these first, so the game stays Spacewar either way.
    char id[16];
    snprintf(id, sizeof id, "%u", kAppId);
    SetEnvironmentVariableA("SteamAppId", id);
    SetEnvironmentVariableA("SteamGameId", id);
    SteamErrMsg msg = {};
    if (SteamAPI_InitEx(&msg) != k_ESteamAPIInitResult_OK)
    {
        err = std::string("Steam: ") + msg;
        return false;
    }
    initialized = true;
    return true;
}

// The Steam client puts its overlay only into games it starts itself. Loaded from the Steam folder
// before the game creates its Direct3D device, the overlay hooks it all the same and shows (Shift+Tab)
// for the running app, Spacewar.
void LoadSteamOverlay()
{
    if (GetModuleHandleA("GameOverlayRenderer64.dll"))
    {
        SessionLog("steam: overlay already in the game (started from Steam)");
        return;
    }
    char path[MAX_PATH] = {};
    DWORD size = sizeof path;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath", RRF_RT_REG_SZ, nullptr, path, &size) !=
        ERROR_SUCCESS)
    {
        SessionLog("steam: Steam folder not found in the registry, no overlay");
        return;
    }
    std::string dll = std::string(path) + "\\GameOverlayRenderer64.dll";
    if (LoadLibraryA(dll.c_str()))
        SessionLog("steam: overlay loaded from %s", dll.c_str());
    else
        SessionLog("steam: overlay %s failed to load, error %lu", dll.c_str(), GetLastError());
}

} // namespace

bool StartSteamEarly(std::string &err)
{
    if (!InitSteamApi(err))
        return false;
    LoadSteamOverlay();
    return true;
}

std::unique_ptr<Session> CreateSteamSession(std::string &err)
{
    if (!InitSteamApi(err))
        return nullptr;
    SteamNetworkingUtils()->InitRelayNetworkAccess();
    return std::make_unique<SteamSession>();
}
} // namespace coopnet
