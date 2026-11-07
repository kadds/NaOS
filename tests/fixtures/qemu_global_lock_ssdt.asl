DefinitionBlock ("", "SSDT", 2, "NAOS  ", "QEMUGLCK", 1)
{
    External (\_SB, DeviceObj)

    Scope (\_SB)
    {
        Device (GLKT)
        {
            Name (_HID, "NAOS0001")
            Name (_CID, "ACME0001")
            Name (_UID, 0x2A)
            Name (_GLK, One)

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }

            Method (_INI, 0, NotSerialized)
            {
                Acquire (\_GL_, 0xFFFF)
                Release (\_GL_)
            }
        }
    }
}
