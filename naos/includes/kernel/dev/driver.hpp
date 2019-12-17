#pragma once
#include "common.hpp"
#include "device.hpp"

namespace dev
{
class driver;
num_t add_driver(driver *driver);
void remove_driver(driver *driver);
class driver
{
    const char *name;
    type driver_type;
    num_t id;
    friend num_t add_driver(driver *driver);
    friend void remove_driver(driver *driver);

  public:
    driver(type dri_type)
        : driver_type(dri_type)
    {
    }

    virtual bool setup(device *dev) = 0;
    virtual void cleanup(device *dev) = 0;

    num_t get_id() { return id; }
    const char *get_name() { return name; }
};

class block_driver : public driver
{
  public:
    block_driver()
        : driver(type::block){};

    virtual u64 read(device *dev, u64 offset, byte *buffer, u64 max_len) = 0;
    virtual u64 write(device *dev, u64 offset, byte *buffer, u64 len) = 0;
};

class char_driver : public driver
{
  public:
    char_driver()
        : driver(type::chr){};

    virtual u64 read(device *dev, byte *buffer, u64 max_len) = 0;
    virtual u64 write(device *dev, byte *buffer, u64 len) = 0;
};

class network_driver : public driver
{
  public:
    network_driver()
        : driver(type::network){};
    virtual u64 read(device *dev, byte *buffer, u64 max_len) = 0;
    virtual u64 write(device *dev, byte *buffer, u64 len) = 0;
};

struct hash_func
{
    u64 operator()(num_t num) { return num; }
};

using driver_map_t = util::hash_map<num_t, driver *, hash_func>;
using driver_id_gen_t = util::id_generator;

void init_driver();

} // namespace dev
