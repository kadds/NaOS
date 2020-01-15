#pragma once

#include "common.hpp"
#include "device.hpp"
namespace io
{
struct request_t;
} // namespace io

namespace dev
{
class driver;
num_t add_driver(driver *driver);
void remove_driver(driver *driver);
class driver
{
    type driver_type;
    const char *name;
    num_t id;
    friend num_t add_driver(driver *driver);
    friend void remove_driver(driver *driver);

  public:
    driver(type dri_type, const char *name)
        : driver_type(dri_type)
        , name(name)
    {
    }
    virtual ~driver() {}
    /// setup device
    virtual bool setup(device *dev) = 0;
    virtual void cleanup(device *dev) = 0;

    /// call when a request is sent
    virtual void on_io_request(::io::request_t *request) = 0;

    num_t get_id() { return id; }
    const char *get_name() { return name; }
};

using driver_map_t = util::hash_map<num_t, driver *>;
using driver_id_gen_t = util::id_generator;

void init_driver();

driver *get_driver(num_t dri);

} // namespace dev
