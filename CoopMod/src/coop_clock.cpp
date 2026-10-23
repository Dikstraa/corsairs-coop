#include "coop_clock.h"

#include <algorithm>
#include <cmath>

namespace coopnet
{
namespace
{

constexpr double kDay = 86400.0;

bool IsLeap(int year) { return year % 400 == 0 || (year % 100 != 0 && year % 4 == 0); }

int YearDays(int year) { return IsLeap(year) ? 366 : 365; }

// Days from 0001-01-01 to January 1st of `year`.
int64_t DaysBeforeYear(int year)
{
    int64_t y = year - 1;
    return y * 365 + y / 4 - y / 100 + y / 400;
}

} // namespace

int MonthDays(int month, int year)
{
    static const int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12)
        return 30; // calendar.c's fallback
    return month == 2 && IsLeap(year) ? 29 : kDays[month - 1];
}

double ToSeconds(const GameDate &d)
{
    int64_t days = DaysBeforeYear(d.year);
    for (int m = 1; m < d.month; ++m)
        days += MonthDays(m, d.year);
    days += d.day - 1;
    return static_cast<double>(days) * kDay + d.hours * 3600.0;
}

GameDate FromSeconds(double seconds)
{
    if (seconds < 0)
        seconds = 0;
    int64_t days = static_cast<int64_t>(std::floor(seconds / kDay));
    GameDate d;
    d.hours = (seconds - static_cast<double>(days) * kDay) / 3600.0;

    d.year = static_cast<int>(days / 366) + 1; // lower bound, then walk forward
    while (DaysBeforeYear(d.year + 1) <= days)
        ++d.year;
    days -= DaysBeforeYear(d.year);
    d.month = 1;
    while (days >= MonthDays(d.month, d.year))
        days -= MonthDays(d.month++, d.year);
    d.day = static_cast<int>(days) + 1;
    return d;
}

void RateMeter::Sample(int source, double proposedHours, double realNow)
{
    if (!valid_ || source != source_)
    {
        valid_ = true;
        source_ = source;
        prevHours_ = proposedHours;
        prevReal_ = realNow;
        return;
    }
    double dt = realNow - prevReal_;
    double dh = proposedHours - prevHours_;
    if (dh < -12)
        dh += 24; // midnight
    prevHours_ = proposedHours; // replaced by Applied() when the caller overrides the time
    prevReal_ = realNow;
    if (dt <= 0 || dh < 0 || dh > 6)
        return; // a jump (skip, load), not a pace
    double sample = dh * 3600.0 / dt;
    rate_ = rate_ <= 0 ? sample : rate_ * 0.7 + sample * 0.3;
}

void Clock::StartHost(const GameDate &now, double realNow, uint32_t epoch)
{
    auto players = std::move(players_);
    Reset();
    players_ = std::move(players);
    valid_ = true;
    host_ = true;
    base_ = ToSeconds(now);
    baseReal_ = realNow;
    epoch_ = epoch;
}

double Clock::PaceFor(const std::map<int, PlayerTime> &players)
{
    double pace = 0;
    for (auto &[id, p] : players)
    {
        if (p.battle)
            return 0;
        if (p.mode == Mode::Menu || p.rate <= 0)
            continue; // menus, dialogs (engine time scale 0) and unmeasured players don't hold time back
        pace = pace <= 0 ? p.rate : std::min(pace, p.rate);
    }
    return pace;
}

void Clock::AdvanceHost(double realNow)
{
    if (!valid_ || !host_)
        return;
    base_ = Now(realNow);
    baseReal_ = realNow;
    rate_ = PaceFor(players_);
}

void Clock::Skip(double gameSeconds, double realNow)
{
    if (!valid_ || !host_)
        return;
    base_ = Now(realNow) + gameSeconds;
    baseReal_ = realNow;
    ++epoch_;
}

void Clock::OnHostClock(double gameSeconds, double rate, uint32_t epoch, double realNow)
{
    valid_ = true;
    host_ = false;
    base_ = gameSeconds;
    baseReal_ = realNow;
    rate_ = rate;
    epoch_ = epoch;
}

void Clock::Reset()
{
    valid_ = false;
    host_ = false;
    base_ = baseReal_ = rate_ = 0;
    epoch_ = 0;
    players_.clear();
}

double Clock::Now(double realNow) const
{
    double dt = std::max(0.0, realNow - baseReal_);
    if (!host_)
        dt = std::min(dt, kStaleAfter);
    return base_ + rate_ * dt;
}

} // namespace coopnet
