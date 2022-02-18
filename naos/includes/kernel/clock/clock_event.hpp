#pragma once
#include "common.hpp"
#include "kernel/time.hpp"
namespace timeclock
{
class clock_source;
class clock_event
{
  protected:
    clock_source *source;
    const u64 level;

  public:
    clock_event(u64 level)
        : level(level){};

    u64 get_level() const { return level; };

    virtual void init(u64 HZ) = 0;
    virtual void destroy() = 0;

    virtual void suspend() = 0;
    virtual void resume() = 0;

    virtual void wait_next_tick() = 0;

    virtual bool is_valid() = 0;

    clock_source *get_source() const { return source; }

    void set_source(clock_source *cs) { source = cs; }
};

} // namespace clock
