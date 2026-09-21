#include "pci.h"

#include <stddef.h>

#include <camkes/io.h>
#include <platsupport/io.h>

#define PCI_CONFIG_ADDRESS 0xcf8
#define PCI_CONFIG_DATA 0xcfc

/* Set in the address port to mean that a configuration cycle is wanted. */
#define PCI_CONFIG_ENABLE 0x80000000u

#define PCI_VENDOR_ID 0x00
#define PCI_COMMAND 0x04
#define PCI_REVISION_AND_CLASS 0x08
#define PCI_HEADER_TYPE 0x0c
#define PCI_BAR0 0x10

#define PCI_COMMAND_BUS_MASTER 0x0004u

/* Set in the header type when the device has functions beyond the first. */
#define PCI_HEADER_MULTIFUNCTION 0x80u

#define PCI_VENDOR_NONE 0xffffu

/* A memory base address register holds an address rather than an I/O port when
 * bit 0 is clear, and spans two registers when the type field says 64 bit. */
#define PCI_BAR_IS_IO 0x1u
#define PCI_BAR_TYPE_MASK 0x6u
#define PCI_BAR_TYPE_64BIT 0x4u
#define PCI_BAR_ADDRESS_MASK 0xfffffff0u

#define PCI_MAX_BUS 256
#define PCI_MAX_DEVICE 32
#define PCI_MAX_FUNCTION 8

static ps_io_port_ops_t port_ops;
static int ops_ready;

static bool ports_ready(void)
{
    if (!ops_ready) {
        if (camkes_io_port_ops(&port_ops) != 0) {
            return false;
        }
        ops_ready = 1;
    }

    return true;
}

static uint32_t address_of(pci_address_t address, uint8_t offset)
{
    return PCI_CONFIG_ENABLE |
           ((uint32_t)address.bus << 16) |
           ((uint32_t)(address.device & 0x1f) << 11) |
           ((uint32_t)(address.function & 0x07) << 8) |
           /* The bottom two bits address a byte within the word, and a
            * configuration cycle is always a whole word. */
           (offset & 0xfcu);
}

uint32_t pci_read32(pci_address_t address, uint8_t offset)
{
    uint32_t value = 0xffffffffu;

    if (!ports_ready()) {
        return value;
    }
    if (ps_io_port_out(&port_ops, PCI_CONFIG_ADDRESS, 4, address_of(address, offset)) != 0) {
        return 0xffffffffu;
    }
    if (ps_io_port_in(&port_ops, PCI_CONFIG_DATA, 4, &value) != 0) {
        return 0xffffffffu;
    }

    return value;
}

void pci_write32(pci_address_t address, uint8_t offset, uint32_t value)
{
    if (!ports_ready()) {
        return;
    }
    if (ps_io_port_out(&port_ops, PCI_CONFIG_ADDRESS, 4, address_of(address, offset)) != 0) {
        return;
    }
    (void)ps_io_port_out(&port_ops, PCI_CONFIG_DATA, 4, value);
}

static bool scan_class(uint32_t class_code, unsigned int wanted, pci_address_t *found,
                       unsigned int *total)
{
    unsigned int seen = 0;
    bool got = false;

    for (int bus = 0; bus < PCI_MAX_BUS; bus++) {
        for (int device = 0; device < PCI_MAX_DEVICE; device++) {
            pci_address_t first = { (uint8_t)bus, (uint8_t)device, 0 };
            uint32_t identity = pci_read32(first, PCI_VENDOR_ID);
            int functions;

            if ((identity & 0xffffu) == PCI_VENDOR_NONE) {
                continue;
            }
            functions = (pci_read32(first, PCI_HEADER_TYPE) & (PCI_HEADER_MULTIFUNCTION << 16)) != 0
                        ? PCI_MAX_FUNCTION : 1;

            for (int function = 0; function < functions; function++) {
                pci_address_t address = { (uint8_t)bus, (uint8_t)device, (uint8_t)function };

                if ((pci_read32(address, PCI_VENDOR_ID) & 0xffffu) == PCI_VENDOR_NONE) {
                    continue;
                }
                /* Class, subclass and programming interface sit in the top three
                 * bytes; the bottom byte is the revision. */
                if ((pci_read32(address, PCI_REVISION_AND_CLASS) >> 8) == class_code) {
                    if (seen == wanted) {
                        if (found != NULL) {
                            *found = address;
                        }
                        got = true;
                    }
                    seen++;
                }
            }
        }
    }

    if (total != NULL) {
        *total = seen;
    }

    return got;
}

bool pci_find_class(uint32_t class_code, unsigned int index, pci_address_t *found)
{
    return scan_class(class_code, index, found, NULL);
}

unsigned int pci_count_class(uint32_t class_code)
{
    unsigned int total = 0;

    (void)scan_class(class_code, (unsigned int)-1, NULL, &total);

    return total;
}

uint64_t pci_bar64(pci_address_t address, uint8_t bar_index)
{
    uint8_t offset = (uint8_t)(PCI_BAR0 + bar_index * 4);
    uint32_t low = pci_read32(address, offset);

    if ((low & PCI_BAR_IS_IO) != 0) {
        return 0;
    }
    if ((low & PCI_BAR_TYPE_MASK) == PCI_BAR_TYPE_64BIT) {
        uint64_t high = pci_read32(address, (uint8_t)(offset + 4));

        return (high << 32) | (low & PCI_BAR_ADDRESS_MASK);
    }

    return low & PCI_BAR_ADDRESS_MASK;
}

void pci_enable_bus_master(pci_address_t address)
{
    uint32_t command = pci_read32(address, PCI_COMMAND);

    pci_write32(address, PCI_COMMAND, command | PCI_COMMAND_BUS_MASTER);
}
