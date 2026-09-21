/*
 * PCI configuration space, through the address and data ports.
 *
 * The driver needs this for two things that cannot be done any other way. The
 * controller's registers are somewhere the firmware decided, and the only way to
 * learn where is to ask its base address register. And a controller may not
 * fetch its own data structures until bus mastering is turned on, which is a bit
 * in its command register.
 *
 * The mechanism is the original one: write an address to 0xcf8, read or write
 * the word at 0xcfc. Memory mapped configuration space would be tidier but has
 * to be found through ACPI first, and this works on everything.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
} pci_address_t;

/* Class 0x0c, subclass 0x03, interface 0x30: a USB controller speaking xHCI. */
#define PCI_CLASS_XHCI 0x0c0330u

uint32_t pci_read32(pci_address_t address, uint8_t offset);
void pci_write32(pci_address_t address, uint8_t offset, uint32_t value);

/*
 * Find the nth function with the given class code, counting from zero, and say
 * how many there are in total.
 *
 * A machine may have several controllers of the same kind with different things
 * wired to each, so finding one is not the same as finding the right one.
 */
bool pci_find_class(uint32_t class_code, unsigned int index, pci_address_t *found);
unsigned int pci_count_class(uint32_t class_code);

/* Where a 64 bit memory base address register points, with the flag bits
 * removed. Returns 0 when the register does not hold a memory address. */
uint64_t pci_bar64(pci_address_t address, uint8_t bar_index);

/* Let the device read and write memory on its own. Nothing a controller is
 * asked to do happens until this is set. */
void pci_enable_bus_master(pci_address_t address);
