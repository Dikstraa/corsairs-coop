#include "coop_vote.h"

namespace coopnet
{

uint32_t VoteBook::Open(int requester, uint32_t requesterTag, double seconds, const std::vector<int> &players,
                        double realNow)
{
    if (active_)
        return 0;
    active_ = true;
    decided_ = false;
    current_ = VoteResult{nextId_++, requester, requesterTag, seconds, false};
    pending_.clear();
    for (int p : players)
        if (p != requester)
            pending_.insert(p);
    deadline_ = realNow + kTimeout;
    if (seconds < kNoVoteBelow || pending_.empty())
        Finish(true);
    return current_.id;
}

void VoteBook::Answer(uint32_t id, int player, bool yes)
{
    if (!active_ || decided_ || id != current_.id || !pending_.count(player))
        return;
    if (!yes)
    {
        Finish(false);
        return;
    }
    pending_.erase(player);
    if (pending_.empty())
        Finish(true);
}

void VoteBook::PlayerLeft(int player)
{
    if (!active_ || decided_)
        return;
    if (player == current_.requester)
    {
        Finish(false);
        return;
    }
    pending_.erase(player);
    if (pending_.empty())
        Finish(true);
}

bool VoteBook::Poll(double realNow, VoteResult &out)
{
    if (!active_)
        return false;
    if (!decided_ && realNow >= deadline_)
        Finish(false);
    if (!decided_)
        return false;
    out = current_;
    active_ = false;
    return true;
}

void VoteBook::Finish(bool approved)
{
    decided_ = true;
    current_.approved = approved;
    pending_.clear();
}

} // namespace coopnet
