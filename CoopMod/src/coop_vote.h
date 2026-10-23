// Time-skip votes, decided by the host (engine-independent, unit-tested).
//
// One vote at a time. Every player present when the vote opens, except the requester, must
// agree before the deadline; a single "no", a timeout or nobody left to ask ends it.
#pragma once

#include <cstdint>
#include <set>
#include <vector>

namespace coopnet
{

struct VoteResult
{
    uint32_t id = 0;
    int requester = -1;
    uint32_t requesterTag = 0; // the requester's own handle for the request
    double seconds = 0;        // game time to skip
    bool approved = false;
};

class VoteBook
{
  public:
    static constexpr double kTimeout = 30.0;      // real seconds to answer
    static constexpr double kNoVoteBelow = 3600.0; // shorter skips need no vote

    // Opens a vote. Returns 0 if another vote is running (caller rejects the request).
    // A skip shorter than kNoVoteBelow, or one with nobody else to ask, is decided at once.
    uint32_t Open(int requester, uint32_t requesterTag, double seconds, const std::vector<int> &players,
                  double realNow);
    void Answer(uint32_t id, int player, bool yes);
    void PlayerLeft(int player);

    bool Active() const { return active_; }
    uint32_t ActiveId() const { return active_ ? current_.id : 0; }
    bool WaitingFor(int player) const { return active_ && pending_.count(player) != 0; }

    // Returns true once when the running vote is decided (or timed out).
    bool Poll(double realNow, VoteResult &out);

  private:
    void Finish(bool approved);

    bool active_ = false;
    bool decided_ = false;
    uint32_t nextId_ = 1;
    VoteResult current_;
    std::set<int> pending_;
    double deadline_ = 0;
};

} // namespace coopnet
