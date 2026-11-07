DefinitionBlock ("", "SSDT", 2, "NAOS  ", "QEMURKR ", 1)
{
    External (\_SB.PCI0.S18_, DeviceObj)

    Scope (\_SB.PCI0.S18_)
    {
        Name (_CID, "QEMUROCK")
        Name (_CRS, ResourceTemplate ()
        {
            QWordMemory (ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                NonCacheable, ReadWrite,
                0x0000000000000000,
                0x00000000FEBD4000,
                0x00000000FEBD5FFF,
                0x0000000000000000,
                0x0000000000002000)
            QWordMemory (ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                NonCacheable, ReadWrite,
                0x0000000000000000,
                0x00000000FEBD6000,
                0x00000000FEBD7FFF,
                0x0000000000000000,
                0x0000000000002000)
        })
    }
}
