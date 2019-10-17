#pragma once
#include "common.hpp"

namespace clock
{
class clock_event;
class clock_source
{
    // note: I'm so Lazy just using base class protected member variables...
  protected:
    clock_event *event;

  public:
    virtual void init() = 0;
    virtual void destroy() = 0;
    virtual u64 current() = 0;

    void reinit()
    {
        destroy();
        init();
    }

    virtual void calibrate(clock_source *cs) = 0;

    clock_event *get_event() const { return event; }

    void set_event(clock_event *ev) { event = ev; }
};
void init();
} // namespace clock
