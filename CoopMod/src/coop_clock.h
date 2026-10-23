// Shared game clock (engine-independent, unit-tested).
//
// The host owns the clock: it runs at the pace of the slowest player that currently needs time
// to pass, and stops while anyone is in a battle. Clients follow the host's broadcasts.
// Each player's natural pace is measured from how fast its engine tries to move the time
// (world map, weather on land/sea, time acceleration), so no per-mode speed table is needed.
#pragma once

#include <cstdint>
#include <map>

namespace coopnet
{

struct GameDate
{
    int year = 1;
    int month = 1;
    int day = 1;
    double hours = 0; // 0..24
};

// Gregorian calendar, same rules as GetMonthDays() in Program/calendar.c.
int MonthDays(int month, int year);
double ToSeconds(const GameDate &d); // game seconds since 0001-01-01 00:00
GameDate FromSeconds(double seconds);

enum class Mode : int32_t
{
    Menu = 0, // main menu, loading, interfaces: does not hold the clock back
    WorldMap = 1,
    Sea = 2,
    Land = 3,
};

struct PlayerTime
{
    Mode mode = Mode::Menu;
    bool battle = false;
    double rate = 0; // measured game seconds per real second, 0 = unknown
};

// Measures the pace at which the local engine moves time. The engine steps from the time it was
// last given, which may be the shared clock rather than its own proposal, so each step is taken
// relative to the hours passed to Applied().
class RateMeter
{
  public:
    // source: which engine part proposed the time (world map / weather); hours of day.
    void Sample(int source, double proposedHours, double realNow);
    // The hours actually handed back to the engine after Sample().
    void Applied(double hours) { prevHours_ = hours; }
    void Reset() { valid_ = false; rate_ = 0; }
    double Rate() const { return rate_; }

  private:
    bool valid_ = false;
    int source_ = 0;
    double prevHours_ = 0;
    double prevReal_ = 0;
    double rate_ = 0;
};

class Clock
{
  public:
    // ---- host ----
    // epoch: bumped by the host on every restart (new game, load) so clients see a deliberate jump.
    void StartHost(const GameDate &now, double realNow, uint32_t epoch = 0);
    void SetPlayer(int id, const PlayerTime &t) { players_[id] = t; }
    void RemovePlayer(int id) { players_.erase(id); }
    // Folds elapsed time in and recomputes the pace from the players' states.
    void AdvanceHost(double realNow);
    // An approved time skip: moves the clock forward and starts a new epoch.
    void Skip(double gameSeconds, double realNow);

    // ---- client ----
    void OnHostClock(double gameSeconds, double rate, uint32_t epoch, double realNow);

    // ---- both ----
    void Reset();
    bool Valid() const { return valid_; }
    bool IsHost() const { return host_; }
    double Rate() const { return rate_; }
    uint32_t Epoch() const { return epoch_; }
    double Now(double realNow) const; // shared time, game seconds

    // Policy used by the host, exposed for tests.
    static double PaceFor(const std::map<int, PlayerTime> &players);

    // Clients stop extrapolating when the host has been silent this long.
    static constexpr double kStaleAfter = 1.5;
    // Pace assumed for a player on land/sea whose pace isn't measured yet (measured: ~10 game s/s).
    static constexpr double kDefaultLandPace = 10.0;

  private:
    bool valid_ = false;
    bool host_ = false;
    double base_ = 0;     // game seconds at baseReal_
    double baseReal_ = 0; // real seconds
    double rate_ = 0;
    uint32_t epoch_ = 0;
    std::map<int, PlayerTime> players_;
};

} // namespace coopnet
