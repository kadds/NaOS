DefinitionBlock ("", "SSDT", 2, "NAOS  ", "NADISR  ", 1)
{
    External (\_SB, DeviceObj)

    Scope (\_SB)
    {
        Device (OFF0)
        {
            Name (_HID, "PNP0A08")
            Name (_SEG, Zero)
            Name (_BBN, 0x40)
            Name (_CRS, ResourceTemplate ()
            {
                WordBusNumber (ResourceProducer, MinFixed, MaxFixed, PosDecode,
                    0x0000, 0x0040, 0x007F, 0x0000, 0x0040)
            })

            Method (_STA, 0, NotSerialized)
            {
                Return (Zero)
            }
        }
    }
}
