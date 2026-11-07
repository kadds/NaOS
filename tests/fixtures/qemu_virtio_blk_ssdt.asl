DefinitionBlock ("", "SSDT", 2, "NAOS  ", "QEMUVBLK", 1)
{
    External (\_SB.PCI0.S18_, DeviceObj)

    Scope (\_SB.PCI0.S18_)
    {
        // The opt-in QEMU launch pins modern virtio-blk at 00:03.0. QEMU
        // assigns the common/notify/device configuration BAR4 at FE000000 and
        // the MSI-X table/PBA BAR1 at FEBD5000 for this fixed device topology.
        // Both windows must be attested so the kernel can reserve the MSI-X
        // table privately while the child maps the unrelated transport BAR.
        Name (_CRS, ResourceTemplate ()
        {
            QWordMemory (ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                NonCacheable, ReadWrite,
                0x0000000000000000,
                0x00000000FE000000,
                0x00000000FE003FFF,
                0x0000000000000000,
                0x0000000000004000)
            QWordMemory (ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                NonCacheable, ReadWrite,
                0x0000000000000000,
                0x00000000FEBD5000,
                0x00000000FEBD5FFF,
                0x0000000000000000,
                0x0000000000001000)
        })
    }
}
