DefinitionBlock ("", "SSDT", 2, "NAOS  ", "QEMUDUO ", 1)
{
    External (\_SB.PCI0.S18_, DeviceObj)
    External (\_SB.PCI0.S20_, DeviceObj)

    Scope (\_SB.PCI0.S18_)
    {
        Name (_CID, "QEMU0002")
        Name (_CRS, ResourceTemplate ()
        {
            QWordMemory (ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                NonCacheable, ReadWrite,
                0x0000000000000000,
                0x00000000FE900000,
                0x00000000FE9FFFFF,
                0x0000000000000000,
                0x0000000000100000)
        })
    }

    Scope (\_SB.PCI0.S20_)
    {
        Name (_CID, "QEMU0002")
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
