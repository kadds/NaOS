DefinitionBlock ("", "SSDT", 2, "NAOS  ", "QEMUGPE ", 1)
{
    External (\_GPE, DeviceObj)

    Scope (\_GPE)
    {
        // Isolate SCI/GPE dispatch from QEMU's CPU hotplug Notify policy.
        // The test triggers the real GPE.2 source, while hotplug policy itself
        // remains outside this smoke test's contract.
        Method (_E02, 0, NotSerialized)
        {
            NoOp
        }
    }
}
