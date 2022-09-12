#pragma once
#include "common.hpp"

namespace timeclock
{
class clock_event;
class clock_source
{
  protected:
    clock_event *event = nullptr;

  public:
    clock_source(const char *name)
        : name_(name)
    {
    }
    virtual void init() = 0;
    virtual void destroy() = 0;
    virtual u64 current() = 0;
    virtual u64 jiff() { return 0; };
    const char *name() const { return name_; }

    void reinit()
    {
        destroy();
        init();
    }

    virtual void calibrate(clock_source *cs) = 0;

    clock_event *get_event() const { return event; }

    void set_event(clock_event *ev) { event = ev; }

  private:
    const char *name_;
};
void init();
} // namespace clock
