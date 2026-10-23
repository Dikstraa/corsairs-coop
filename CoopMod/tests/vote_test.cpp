// Unit test of time-skip votes.
#include "../src/coop_vote.h"

#include <cstdio>

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

int main()
{
    const double kHours8 = 8 * 3600;
    VoteResult r;

    std::printf("[1] everybody agrees\n");
    VoteBook book;
    uint32_t id = book.Open(1, 77, kHours8, {0, 1, 2}, 0.0);
    CHECK(id != 0 && book.Active() && book.WaitingFor(0) && book.WaitingFor(2) && !book.WaitingFor(1));
    CHECK(!book.Poll(1.0, r));
    CHECK(book.Open(2, 1, kHours8, {0, 1, 2}, 1.0) == 0); // busy
    book.Answer(id, 0, true);
    CHECK(!book.Poll(2.0, r));
    book.Answer(id, 1, true); // the requester doesn't vote: ignored
    book.Answer(id, 2, true);
    CHECK(book.Poll(3.0, r) && r.approved && r.id == id && r.requester == 1 && r.requesterTag == 77 &&
          r.seconds == kHours8);
    CHECK(!book.Active() && !book.Poll(4.0, r));

    std::printf("[2] one refusal\n");
    id = book.Open(0, 5, kHours8, {0, 1, 2}, 10.0);
    book.Answer(id, 1, true);
    book.Answer(id, 2, false);
    CHECK(book.Poll(11.0, r) && !r.approved);

    std::printf("[3] timeout\n");
    id = book.Open(0, 6, kHours8, {0, 1}, 20.0);
    CHECK(!book.Poll(20.0 + VoteBook::kTimeout - 1, r));
    CHECK(book.Poll(20.0 + VoteBook::kTimeout, r) && !r.approved);

    std::printf("[4] stale answers are ignored\n");
    uint32_t old = id;
    id = book.Open(0, 7, kHours8, {0, 1}, 100.0);
    book.Answer(old, 1, false);
    CHECK(!book.Poll(101.0, r));
    book.Answer(id, 1, true);
    CHECK(book.Poll(102.0, r) && r.approved);

    std::printf("[5] short skips and lone players need no vote\n");
    book.Open(1, 8, 1800, {0, 1}, 200.0);
    CHECK(book.Poll(200.0, r) && r.approved);
    book.Open(0, 9, kHours8, {0}, 201.0);
    CHECK(book.Poll(201.0, r) && r.approved);

    std::printf("[6] players leaving\n");
    id = book.Open(0, 10, kHours8, {0, 1, 2}, 300.0);
    book.Answer(id, 1, true);
    book.PlayerLeft(2); // the last one we waited for
    CHECK(book.Poll(301.0, r) && r.approved);
    id = book.Open(1, 11, kHours8, {0, 1}, 400.0);
    book.PlayerLeft(1); // the requester
    CHECK(book.Poll(401.0, r) && !r.approved);

    std::printf(g_failures ? "\n%d CHECK(S) FAILED\n" : "\nALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
