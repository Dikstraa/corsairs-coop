// Unit test of the shared clock: calendar conversion, pace policy, rate measurement.
#include "../src/coop_clock.h"

#include <cmath>
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

static bool Near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

static bool SameDate(const GameDate &a, int y, int m, int d, double h)
{
    return a.year == y && a.month == m && a.day == d && Near(a.hours, h, 1e-6);
}

int main()
{
    std::printf("[1] calendar\n");
    CHECK(MonthDays(2, 1600) == 29 && MonthDays(2, 1700) == 28 && MonthDays(2, 1704) == 29 && MonthDays(2, 1705) == 28);
    CHECK(MonthDays(4, 1650) == 30 && MonthDays(13, 1650) == 30);
    CHECK(Near(ToSeconds({1, 1, 1, 0}), 0));
    CHECK(Near(ToSeconds({1, 1, 2, 1.5}), 86400 + 5400));
    {
        GameDate d = FromSeconds(ToSeconds({1665, 12, 31, 23.5}) + 3600);
        CHECK(SameDate(d, 1666, 1, 1, 0.5));
        d = FromSeconds(ToSeconds({1704, 2, 28, 12}) + 86400);
        CHECK(SameDate(d, 1704, 2, 29, 12));
        d = FromSeconds(ToSeconds({1700, 2, 28, 12}) + 86400);
        CHECK(SameDate(d, 1700, 3, 1, 12));
    }
    bool roundTrip = true;
    for (int y = 1600; y <= 1800 && roundTrip; ++y)
        for (int m = 1; m <= 12 && roundTrip; ++m)
            for (int day = 1; day <= MonthDays(m, y) && roundTrip; day += 7)
            {
                GameDate d = FromSeconds(ToSeconds({y, m, day, 17.25}));
                roundTrip = SameDate(d, y, m, day, 17.25);
                if (!roundTrip)
                    std::printf("    round trip broke at %d-%d-%d\n", y, m, day);
            }
    CHECK(roundTrip);

    std::printf("[2] pace policy\n");
    std::map<int, PlayerTime> p;
    CHECK(Clock::PaceFor(p) == 0);
    p[0] = {Mode::WorldMap, false, 3600};
    CHECK(Clock::PaceFor(p) == 3600);
    p[1] = {Mode::Land, false, 350};
    CHECK(Clock::PaceFor(p) == 350);
    p[2] = {Mode::Menu, false, 10};
    CHECK(Clock::PaceFor(p) == 350); // menus don't hold time back
    p[3] = {Mode::Land, false, 0};
    CHECK(Clock::PaceFor(p) == 350); // dialog (time scale 0) doesn't either
    p[1].battle = true;
    CHECK(Clock::PaceFor(p) == 0); // battle stops everyone

    std::printf("[3] host clock\n");
    Clock host;
    host.StartHost({1665, 6, 1, 8}, 100.0);
    host.SetPlayer(0, {Mode::WorldMap, false, 3600});
    host.SetPlayer(1, {Mode::Land, false, 360});
    host.AdvanceHost(100.0);
    CHECK(host.Rate() == 360);
    host.AdvanceHost(110.0); // 10 s at 360 = 1 game hour
    CHECK(SameDate(FromSeconds(host.Now(110.0)), 1665, 6, 1, 9));
    host.SetPlayer(1, {Mode::Sea, true, 360});
    host.AdvanceHost(120.0); // still 360 for the elapsed 10 s, then stopped
    CHECK(SameDate(FromSeconds(host.Now(120.0)), 1665, 6, 1, 10));
    CHECK(host.Rate() == 0);
    CHECK(Near(host.Now(500.0), host.Now(120.0)));
    host.RemovePlayer(1);
    host.AdvanceHost(500.0);
    CHECK(host.Rate() == 3600);
    uint32_t epoch = host.Epoch();
    double before = host.Now(500.0);
    host.Skip(8 * 3600, 500.0);
    CHECK(Near(host.Now(500.0), before + 8 * 3600) && host.Epoch() == epoch + 1);

    std::printf("[4] client follows, stops extrapolating when the host is silent\n");
    Clock client;
    CHECK(!client.Valid());
    client.OnHostClock(1000.0, 100.0, 3, 50.0);
    CHECK(client.Valid() && !client.IsHost() && client.Epoch() == 3);
    CHECK(Near(client.Now(51.0), 1100.0));
    CHECK(Near(client.Now(60.0), 1000.0 + 100.0 * Clock::kStaleAfter));

    std::printf("[5] rate meter\n");
    RateMeter meter;
    double hours = 8.0;
    for (int i = 0; i <= 20; ++i)
        meter.Sample(1, hours + i * 0.1, i * 1.0); // 0.1 h per second = 360 game s/s
    CHECK(Near(meter.Rate(), 360, 1e-6));
    meter.Sample(1, 23.95, 21.0); // a jump (skip): ignored
    meter.Sample(1, 0.05, 22.0);  // across midnight: 0.1 h
    CHECK(Near(meter.Rate(), 360, 1e-6));
    meter.Sample(2, 12.0, 23.0); // other source: new reference, rate kept
    CHECK(Near(meter.Rate(), 360, 1e-6));

    std::printf("[6] rate meter while the time is overridden (paused shared clock)\n");
    RateMeter held;
    const double shared = 11.25; // the clock does not move; the engine keeps stepping 0.1 h/s from it
    held.Sample(2, shared, 0.0);
    held.Applied(shared);
    for (int i = 1; i <= 20; ++i)
    {
        held.Sample(2, shared + 0.1, i * 1.0);
        held.Applied(shared);
    }
    CHECK(Near(held.Rate(), 360, 1e-6));

    std::printf(g_failures ? "\n%d CHECK(S) FAILED\n" : "\nALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
