DefinitionBlock ("", "SSDT", 2, "NAOS  ", "QEMUEDU ", 1)
{
    External (\_SB.PCI0.S18_, DeviceObj)

    Scope (\_SB.PCI0.S18_)
    {
        // Synthetic firmware identity exercises PlatformPciTopology-driven
        // driver selection; it is not a claim about the real EDU device.
        Name (_CID, "QEMU0002")

        // QEMU's q35 firmware assigns the fixed-address EDU BAR0 at this
        // address for the test's pinned slot (00:03.0); this is test firmware
        // evidence, not a production BAR allocation or config-space write.
        Name (_CRS, ResourceTemplate ()
        {
            QWordMemory (ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                NonCacheable, ReadWrite,
                0x0000000000000000,
                0x00000000FEA00000,
                0x00000000FEAFFFFF,
                0x0000000000000000,
                0x0000000000100000)
        })
    }
}
