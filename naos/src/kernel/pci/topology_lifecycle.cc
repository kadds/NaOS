#include "kernel/pci/topology_lifecycle.hpp"

namespace pci
{
namespace
{
topology_lifecycle lifetime;
}

topology_lifecycle &topology_lifetime() noexcept { return lifetime; }

} // namespace pci
