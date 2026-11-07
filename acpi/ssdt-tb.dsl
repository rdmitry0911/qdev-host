/*
 * SSDT for stock QEMU q35: Thunderbolt-flavoured ACPI properties, equivalent
 * to what hw/pci-bridge/thunderbolt_root_port.c and
 * thunderbolt_pcie_pci_bridge.c generate in the qemu-thunderbolt fork.
 *
 * Root port:  -device pcie-root-port,id=tbrp0,bus=pcie.0,addr=0x05  -> \_SB.PCI0.S28
 * Bridge:     -device pcie-pci-bridge,id=tbbr0,bus=tbrp0            -> \_SB.PCI0.S38.S00
 * (QEMU names PCI devices S<devfn hex>; devfn = slot << 3 | func.)
 *
 * Load with: -acpitable file=ssdt-tb.aml   (or via OpenCore ACPI/Add on macOS)
 */
DefinitionBlock ("", "SSDT", 2, "QEMUTB", "TBHOTPLG", 0x00000001)
{
    External (\_SB.PCI0.S28, DeviceObj)
    External (\_SB.PCI0.S38.S00, DeviceObj)

    Scope (\_SB.PCI0.S28)
    {
        Name (_DSD, Package ()
        {
            ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
            Package ()
            {
                Package () { "PCI-Thunderbolt", One },
                Package () { "pci-supports-link-change", One },
            }
        })
    }

    Scope (\_SB.PCI0.S38.S00)
    {
        Name (_DSD, Package ()
        {
            ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
            Package ()
            {
                Package () { "IOPCITunnelled", One },
            }
        })
    }
}
