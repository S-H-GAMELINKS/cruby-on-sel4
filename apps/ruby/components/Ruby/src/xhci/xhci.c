#include "xhci.h"

#include "hid.h"
#include "pci.h"
#include "regs.h"

#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <camkes.h>
#include <camkes/dma.h>
#include <muslcsys/vsyscall.h>
#include <utils/util.h>

#include "../console/console.h"

/* Entries per ring. One page each, which is far more than a keyboard needs and
 * keeps every ring inside a single page so its physical address is contiguous
 * without asking. */
#define RING_ENTRIES 256

/* How long to wait for the controller to answer, in arbitrary spins. There is no
 * clock in reach here that is cheaper than reading a register, and this only has
 * to be long enough for hardware that is working and short enough not to hang
 * forever on hardware that is not. */
#define WAIT_SPINS 1000000

static volatile uint8_t *base;
static volatile uint8_t *op;
static volatile uint8_t *runtime;
static volatile uint32_t *doorbell;

static uint32_t max_slots;
static uint32_t max_ports;
static bool large_contexts;

/*
 * Everything the controller reads or writes for itself, reached through volatile
 * pointers.
 *
 * The compiler cannot see the other side of any of this. Left as ordinary memory
 * it may keep a value it read earlier rather than looking again -- which turns
 * the wait for an event into a spin on a stale word -- or decide that a buffer
 * still holds the zero it was cleared to. Volatile is what says otherwise, and
 * it has to be on the pointers rather than only at the places that seemed to
 * matter.
 */
static volatile uint64_t *dcbaa;
static volatile xhci_trb_t *command_ring;
static volatile xhci_trb_t *event_ring;
static volatile xhci_erst_entry_t *erst;

/*
 * Where the next entry goes and which cycle bit marks it as ours.
 *
 * A ring is never emptied; it is the cycle bit that says which entries the other
 * side has not yet seen. It flips each time the ring wraps, so producer and
 * consumer stay in step without either clearing anything.
 */
static uint32_t command_index;
static uint32_t command_cycle = 1;
static uintptr_t command_paddr;

static uint32_t event_index;
static uint32_t event_cycle = 1;
static uintptr_t event_paddr;

/* At most this many devices are enumerated. A keyboard is the only one wanted,
 * and a hub full of them would not help. */
#define MAX_DEVICES 8

/* One attached device, as far as this needs to know it. */
typedef struct {
    uint8_t slot;
    uint8_t port;
    uint8_t speed;
    volatile xhci_trb_t *ep0_ring;
    uint32_t ep0_index;
    uint32_t ep0_cycle;
    uint16_t vendor;
    uint16_t product;
    uint8_t device_class;
    /* How much of the descriptor actually arrived, and the packet size the
     * device said it uses. A transfer can report success and move nothing, and
     * these are what tell the two apart. */
    uint8_t received;
    uint8_t packet_size;

    /* The interrupt endpoint its reports arrive on, once one has been found and
     * configured. Zero for the device context index means there is none. */
    uint8_t dci;
    uint16_t report_size;
    volatile xhci_trb_t *ring;
    uint32_t ring_index;
    uint32_t ring_cycle;
    uintptr_t ring_paddr;
    volatile uint8_t *report;
    uintptr_t report_paddr;
    bool armed;
    /* Whether the endpoint's reports are a keyboard's, and the last one seen.
     * A report says what is held, so the previous one is what turns it into
     * what was pressed. */
    bool keyboard;
    uint8_t previous[8];
} device_t;

static device_t devices[MAX_DEVICES];
static int device_count;
static unsigned int controller_count;
static char controller_bars[64];

static bool running;
static bool initialised;

/*
 * Say what happened, both to the serial line and to the row the console keeps
 * out of the terminal's reach.
 *
 * The machine this is written for has no serial port, and the program that owns
 * the screen redraws it constantly, so anything merely printed is gone before it
 * can be read. The status row is the only place a message stays.
 */
/* Why the last port that failed did, kept short enough to sit in a summary
 * alongside the others. Set where the failure is noticed, read once the search
 * is over. */
static char failure[48];

static void note_failure(const char *what)
{
    snprintf(failure, sizeof(failure), "%s", what);
}

static void report(const char *fmt, ...)
{
    char line[160];
    va_list args;

    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    printf("xHCI: %s\n", line);
    console_fb_set_status(line);
}

/*
 * Order our accesses against the controller's.
 *
 * The rings and buffers are ordinary memory as far as the compiler is concerned:
 * it cannot see that another device writes them, and it cannot see that a write
 * to a doorbell register makes it read what was just prepared. Left to itself it
 * may keep the zero a buffer was cleared to rather than reading what arrived, or
 * move the preparation of a request after the doorbell that announces it. Both
 * produce a transfer that reports success and does nothing, and both come and go
 * with unrelated changes, because they depend on how the compiler happened to
 * inline things.
 *
 * Nothing stronger than a compiler barrier is needed. The memory is uncached and
 * x86 does not reorder stores against each other, so what is missing is only the
 * compiler's knowledge.
 */
static void dma_barrier(void)
{
    __asm__ volatile("" ::: "memory");
}

static uint32_t read32(volatile uint8_t *at, uint32_t offset)
{
    return *(volatile uint32_t *)(at + offset);
}

static void write32(volatile uint8_t *at, uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(at + offset) = value;
}

/*
 * A 64 bit register is written as two words, low first.
 *
 * The specification allows the controller to require this, and some do: writing
 * the high word first can make it act on a half-formed address.
 */
static void write64(volatile uint8_t *at, uint32_t offset, uint64_t value)
{
    *(volatile uint32_t *)(at + offset) = (uint32_t)value;
    *(volatile uint32_t *)(at + offset + 4) = (uint32_t)(value >> 32);
}

/*
 * Wait, roughly.
 *
 * There is no clock in reach that is cheaper than this. A read of a register on
 * uncached device memory is a bus transaction of its own and takes on the order
 * of a microsecond, which is close enough for delays the USB specification
 * states in milliseconds and means as minimums.
 */
/* Reads per millisecond. A register read on uncached device memory is a bus
 * transaction of its own, but how long one takes varies by an order of magnitude
 * between machines, so this errs long: the specification's figures are minimums
 * and waiting twice as long as needed costs nothing here. */
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define READS_PER_MS 20000u

static void delay_ms(unsigned int ms)
{
    for (unsigned int i = 0; i < ms * READS_PER_MS; i++) {
        (void)read32(op, XHCI_USBSTS);
    }
}

static bool wait_until_clear(volatile uint8_t *at, uint32_t offset, uint32_t mask)
{
    for (int spin = 0; spin < WAIT_SPINS; spin++) {
        if ((read32(at, offset) & mask) == 0) {
            return true;
        }
    }

    return false;
}

/*
 * Memory the controller reads for itself, so it has to be physically contiguous
 * and its physical address has to be known. A page at a time keeps both true.
 */
static volatile void *dma_page(size_t size, uintptr_t *paddr)
{
    void *memory = camkes_dma_alloc(size, 4096, false);

    if (memory == NULL) {
        return NULL;
    }
    memset(memory, 0, size);
    *paddr = camkes_dma_get_paddr(memory);
    if (*paddr == 0) {
        return NULL;
    }

    return memory;
}

/*
 * Put the controller back to a known state.
 *
 * It has to be halted before it can be reset, and it takes its time over both.
 * The not-ready bit covers the period after a reset when its registers may not
 * be read at all.
 */
static bool reset_controller(void)
{
    write32(op, XHCI_USBCMD, read32(op, XHCI_USBCMD) & ~XHCI_USBCMD_RUN);
    /* Halting is the one thing waited for by a bit becoming set rather than
     * clear, so it does not go through wait_until_clear. */
    for (int spin = 0; spin < WAIT_SPINS; spin++) {
        if ((read32(op, XHCI_USBSTS) & XHCI_USBSTS_HALTED) != 0) {
            break;
        }
    }

    write32(op, XHCI_USBCMD, read32(op, XHCI_USBCMD) | XHCI_USBCMD_RESET);
    if (!wait_until_clear(op, XHCI_USBCMD, XHCI_USBCMD_RESET)) {
        report("reset did not complete");
        return false;
    }
    if (!wait_until_clear(op, XHCI_USBSTS, XHCI_USBSTS_NOT_READY)) {
        report("controller stayed not-ready");
        return false;
    }

    return true;
}

/*
 * Some controllers want memory of their own, and say how many pages in the
 * structural parameters. It is handed over as an array of addresses in the first
 * slot of the device context array, and never touched again.
 */
static bool setup_scratchpad(void)
{
    uint32_t count = XHCI_HCSPARAMS2_MAX_SCRATCHPAD(read32(base, XHCI_HCSPARAMS2));
    uintptr_t array_paddr;
    volatile uint64_t *array;

    if (count == 0) {
        return true;
    }
    array = dma_page(count * sizeof(uint64_t), &array_paddr);
    if (array == NULL) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        uintptr_t page_paddr;

        if (dma_page(4096, &page_paddr) == NULL) {
            return false;
        }
        array[i] = page_paddr;
    }
    dcbaa[0] = array_paddr;

    return true;
}

/*
 * A ring is a run of transfer request blocks ending in one that points back to
 * the start, so the controller never reaches the end of it.
 */
static void link_ring(volatile xhci_trb_t *ring, uintptr_t paddr)
{
    volatile xhci_trb_t *last = &ring[RING_ENTRIES - 1];

    last->parameter = paddr;
    last->status = 0;
    /* Toggling the cycle bit here is what tells the consumer that the ring has
     * wrapped, which is how it knows which entries are new. */
    last->control = XHCI_TRB_TYPE(XHCI_TRB_LINK) | XHCI_TRB_TOGGLE_CYCLE;
}

static bool setup_rings(void)
{
    uintptr_t dcbaa_paddr;
    uintptr_t erst_paddr;

    dcbaa = dma_page((max_slots + 1) * sizeof(uint64_t), &dcbaa_paddr);
    command_ring = dma_page(RING_ENTRIES * sizeof(xhci_trb_t), &command_paddr);
    event_ring = dma_page(RING_ENTRIES * sizeof(xhci_trb_t), &event_paddr);
    erst = dma_page(sizeof(xhci_erst_entry_t), &erst_paddr);

    if (dcbaa == NULL || command_ring == NULL || event_ring == NULL || erst == NULL) {
        report("could not allocate rings");
        return false;
    }

    if (!setup_scratchpad()) {
        report("could not allocate scratchpad");
        return false;
    }

    write64(op, XHCI_DCBAAP, dcbaa_paddr);

    link_ring(command_ring, command_paddr);
    /* The low bits hold flags; the cycle bit says which value marks a command
     * the controller has not seen yet. */
    write64(op, XHCI_CRCR, command_paddr | XHCI_CRCR_RING_CYCLE);

    erst->address = event_paddr;
    erst->size = RING_ENTRIES;
    erst->reserved = 0;

    write32(runtime, XHCI_ERSTSZ, 1);
    /* The dequeue pointer has to be set before the table, or the controller may
     * start from wherever it was left. */
    write64(runtime, XHCI_ERDP, event_paddr);
    write64(runtime, XHCI_ERSTBA, erst_paddr);

    return true;
}

/*
 * Take the next event the controller has produced, if there is one.
 *
 * An event is ours when its cycle bit matches what we expect, which is what
 * distinguishes a fresh entry from the stale one left there last time round.
 * The dequeue pointer is handed back so the controller knows how much room it
 * has; the busy bit is written along with it, as the specification requires.
 */
static bool next_event(xhci_trb_t *into)
{
    volatile xhci_trb_t *event = &event_ring[event_index];

    if ((event->control & XHCI_TRB_CYCLE) != event_cycle) {
        return false;
    }

    into->parameter = event->parameter;
    into->status = event->status;
    into->control = event->control;

    event_index++;
    if (event_index == RING_ENTRIES) {
        event_index = 0;
        event_cycle ^= 1;
    }
    write64(runtime, XHCI_ERDP,
            (event_paddr + event_index * sizeof(xhci_trb_t)) | XHCI_ERDP_BUSY);

    return true;
}

/* Wait for an event of a particular kind, discarding the others. Port status
 * changes in particular arrive unbidden and mean nothing here. */
static bool await_event(uint32_t type, xhci_trb_t *into)
{
    for (int spin = 0; spin < WAIT_SPINS; spin++) {
        xhci_trb_t event;

        if (!next_event(&event)) {
            continue;
        }
        if (XHCI_TRB_TYPE_OF(event.control) != type) {
            continue;
        }
        *into = event;
        return true;
    }

    return false;
}

/*
 * Put a command on the ring and wait for its completion event.
 *
 * The cycle bit is written last, because it is what makes the entry visible: the
 * controller may be looking at this slot already, and an entry whose cycle bit
 * is set before its contents are is an entry it may act on half-written.
 */
static bool command(uint64_t parameter, uint32_t control, xhci_trb_t *completion)
{
    volatile xhci_trb_t *slot = &command_ring[command_index];

    slot->parameter = parameter;
    slot->status = 0;
    slot->control = control | command_cycle;

    command_index++;
    if (command_index == RING_ENTRIES - 1) {
        /* The last entry is the link back to the start, and it carries the cycle
         * bit too. */
        command_ring[RING_ENTRIES - 1].control =
            XHCI_TRB_TYPE(XHCI_TRB_LINK) | XHCI_TRB_TOGGLE_CYCLE | command_cycle;
        command_index = 0;
        command_cycle ^= 1;
    }

    /* Doorbell zero, target zero: the command ring. The barrier is what stops the
     * entry above from being written after the bell that announces it. */
    dma_barrier();
    doorbell[0] = 0;

    if (!await_event(XHCI_TRB_COMMAND_COMPLETE, completion)) {
        report("command %u never completed", XHCI_TRB_TYPE_OF(control));
        return false;
    }
    if (XHCI_COMPLETION_CODE(completion->status) != XHCI_COMPLETION_SUCCESS) {
        report("command %u failed, code %u", XHCI_TRB_TYPE_OF(control),
               XHCI_COMPLETION_CODE(completion->status));
        return false;
    }

    return true;
}

/* Port speeds, as the port status register reports them. */
#define SPEED_FULL 1
#define SPEED_LOW 2
#define SPEED_HIGH 3
#define SPEED_SUPER 4

static uint32_t ep0_packet_size(uint32_t speed)
{
    switch (speed) {
    case SPEED_SUPER: return 512;
    case SPEED_HIGH: return 64;
    case SPEED_LOW:
    case SPEED_FULL:
    default: return 8;
    }
}

/* Contexts are 32 or 64 bytes as the controller prefers, so they are reached by
 * index rather than by a fixed offset. */
static volatile uint32_t *context_at(volatile void *contexts, unsigned int index)
{
    size_t stride = large_contexts ? 64 : 32;

    return (volatile uint32_t *)((volatile uint8_t *)contexts + index * stride);
}

/*
 * Bring a port to the point where the device on it will answer.
 *
 * A USB 3 port resets itself when something is plugged in and is enabled by the
 * time anyone looks. A USB 2 port has to be asked, and says it is done by
 * enabling itself.
 */
static bool reset_port(uint32_t port)
{
    uint32_t status = read32(op, XHCI_PORTSC(port));

    if ((status & XHCI_PORTSC_ENABLED) == 0) {
        write32(op, XHCI_PORTSC(port), XHCI_PORTSC_KEEP(status) | XHCI_PORTSC_RESET);

        for (int spin = 0; spin < WAIT_SPINS; spin++) {
            status = read32(op, XHCI_PORTSC(port));
            if ((status & XHCI_PORTSC_ENABLED) != 0) {
                break;
            }
        }
        if ((status & XHCI_PORTSC_ENABLED) == 0) {
            return false;
        }
        /* Clear the change bits, and only those: a one written anywhere else
         * here would undo the reset that just succeeded. */
        write32(op, XHCI_PORTSC(port),
                XHCI_PORTSC_KEEP(status) | (status & XHCI_PORTSC_CHANGES));
    }

    /*
     * A device is not obliged to answer immediately after a reset. The
     * specification gives it 10 milliseconds to recover, and talking to it
     * sooner is a transaction error rather than a device that is merely slow --
     * which is exactly what Address Device reports.
     */
    delay_ms(20);

    return true;
}

/*
 * A control transfer, which is three entries on the endpoint's ring: what is
 * being asked, where the answer goes, and an acknowledgement.
 *
 * Only reads from the device are handled, because that is all this needs -- the
 * descriptors that say what the device is, and later what its endpoints are.
 */
/* The completion code a transfer ended with, or zero when no event arrived at
 * all. The two failures mean quite different things -- a code says the bus was
 * used and something came back, silence says the controller never looked -- so
 * they are reported rather than collapsed into a boolean. */
/* How many bytes the last transfer actually moved. */
static uint32_t last_transferred;

static int control_in(device_t *device, usb_setup_t setup, volatile void *buffer, uintptr_t buffer_paddr)
{
    volatile xhci_trb_t *ring = device->ep0_ring;
    uint32_t index = device->ep0_index;
    xhci_trb_t event;
    uint64_t immediate;

    (void)buffer;
    memcpy(&immediate, &setup, sizeof(immediate));

    ring[index].parameter = immediate;
    ring[index].status = sizeof(usb_setup_t);
    ring[index].control = XHCI_TRB_TYPE(XHCI_TRB_SETUP_STAGE) | XHCI_TRB_IMMEDIATE |
                          (setup.length != 0 ? XHCI_TRB_TRT_IN : XHCI_TRB_TRT_NO_DATA) |
                          device->ep0_cycle;
    index++;

    if (setup.length != 0) {
        ring[index].parameter = buffer_paddr;
        ring[index].status = setup.length;
        ring[index].control = XHCI_TRB_TYPE(XHCI_TRB_DATA_STAGE) | XHCI_TRB_DIR_IN |
                              device->ep0_cycle;
        index++;
    }

    ring[index].parameter = 0;
    ring[index].status = 0;
    /* The status stage goes the other way from the data, and is the one that
     * raises the event this waits on. */
    ring[index].control = XHCI_TRB_TYPE(XHCI_TRB_STATUS_STAGE) | XHCI_TRB_IOC |
                          (setup.length != 0 ? 0 : XHCI_TRB_DIR_IN) |
                          device->ep0_cycle;
    index++;

    device->ep0_index = index;

    /* Doorbell for this slot, target 1: the control endpoint. */
    dma_barrier();
    doorbell[device->slot] = 1;

    last_transferred = 0;
    if (!await_event(XHCI_TRB_TRANSFER_EVENT, &event)) {
        return 0;
    }
    /* Whatever arrived is in the buffer now, and the compiler has no reason to
     * believe it changed since it was cleared. */
    dma_barrier();
    /* The event reports what was left over, not what arrived. */
    if (XHCI_EVENT_RESIDUAL(event.status) <= setup.length) {
        last_transferred = setup.length - XHCI_EVENT_RESIDUAL(event.status);
    }

    return (int)XHCI_COMPLETION_CODE(event.status);
}

/*
 * The completion codes worth telling apart by name, since each points somewhere
 * different. Anything else is reported as a number.
 */
static const char *completion_name(int code)
{
    switch (code) {
    case 2: return "code 2 data buffer";
    case 4: return "code 4 transaction";
    case 5: return "code 5 bad TRB";
    case 6: return "code 6 stall";
    case 17: return "code 17 parameter";
    case 19: return "code 19 context state";
    default: {
        static char other[16];

        snprintf(other, sizeof(other), "code %d", code);
        return other;
    }
    }
}

/* A device that sends less than was asked for is not in error; it simply had
 * less to say. */
static bool transfer_ok(int code)
{
    return code == XHCI_COMPLETION_SUCCESS || code == XHCI_COMPLETION_SHORT_PACKET;
}

/*
 * A control transfer with nothing to carry: the setup packet and an
 * acknowledgement. Used to tell a device which configuration to adopt, which is
 * what makes its endpoints exist.
 */
static int control_out(device_t *device, usb_setup_t setup)
{
    volatile xhci_trb_t *ring = device->ep0_ring;
    uint32_t index = device->ep0_index;
    xhci_trb_t event;
    uint64_t immediate;

    memcpy(&immediate, &setup, sizeof(immediate));

    ring[index].parameter = immediate;
    ring[index].status = sizeof(usb_setup_t);
    ring[index].control = XHCI_TRB_TYPE(XHCI_TRB_SETUP_STAGE) | XHCI_TRB_IMMEDIATE |
                          XHCI_TRB_TRT_NO_DATA | device->ep0_cycle;
    index++;

    ring[index].parameter = 0;
    ring[index].status = 0;
    ring[index].control = XHCI_TRB_TYPE(XHCI_TRB_STATUS_STAGE) | XHCI_TRB_IOC |
                          XHCI_TRB_DIR_IN | device->ep0_cycle;
    index++;

    device->ep0_index = index;
    dma_barrier();
    doorbell[device->slot] = 1;

    if (!await_event(XHCI_TRB_TRANSFER_EVENT, &event)) {
        return 0;
    }

    return (int)XHCI_COMPLETION_CODE(event.status);
}

/*
 * Find an interrupt endpoint that reports to the host.
 *
 * A configuration descriptor is followed by the interface and endpoint
 * descriptors that belong to it, one after another, each saying its own length.
 * Walking them is the only way to learn what a device offers; nothing else
 * describes it.
 */
static bool find_interrupt_endpoint(const volatile uint8_t *config, size_t length,
                                    uint8_t *address, uint16_t *packet, uint8_t *interval,
                                    bool *keyboard)
{
    size_t at = 0;
    bool in_keyboard = false;

    *keyboard = false;

    while (at + 2 <= length) {
        uint8_t size = config[at];
        uint8_t type = config[at + 1];

        if (size < 2 || at + size > length) {
            return false;
        }
        if (type == USB_DESCRIPTOR_INTERFACE && size >= sizeof(usb_interface_descriptor_t)) {
            const volatile usb_interface_descriptor_t *interface =
                (const volatile usb_interface_descriptor_t *)&config[at];

            /* Class 3 is HID; boot protocol 1 is a keyboard, which is the one
             * arrangement whose reports mean the same thing on every device. */
            in_keyboard = interface->interface_class == 0x03 &&
                          interface->interface_protocol == 0x01;
        }
        if (type == USB_DESCRIPTOR_ENDPOINT && size >= sizeof(usb_endpoint_descriptor_t)) {
            const volatile usb_endpoint_descriptor_t *endpoint =
                (const volatile usb_endpoint_descriptor_t *)&config[at];

            if ((endpoint->attributes & USB_ENDPOINT_TYPE_MASK) == USB_ENDPOINT_TYPE_INTERRUPT &&
                (endpoint->address & USB_ENDPOINT_IN) != 0) {
                /* Keep looking past an interrupt endpoint that is not a
                 * keyboard's: a device may offer several, and only one of them
                 * says anything this can read. */
                if (!in_keyboard) {
                    at += size;
                    continue;
                }
                *address = endpoint->address;
                *packet = endpoint->max_packet_size & 0x7ff;
                *interval = endpoint->interval;
                *keyboard = true;
                return true;
            }
        }
        at += size;
    }

    return false;
}

/*
 * How often the controller should poll the endpoint, as the endpoint context
 * wants it: an exponent of 125 microsecond units.
 *
 * A full or low speed endpoint states its interval in whole milliseconds, so the
 * exponent is of that many frames' worth of microframes. The range is what the
 * specification allows; a device asking for something outside it gets the
 * nearest that is permitted.
 */
static uint32_t interval_exponent(uint32_t speed, uint8_t interval)
{
    uint32_t microframes = (speed == SPEED_HIGH || speed == SPEED_SUPER)
                           ? (interval > 0 ? (1u << (interval - 1)) : 1u)
                           : (uint32_t)interval * 8u;
    uint32_t exponent = 0;

    if (microframes == 0) {
        microframes = 1;
    }
    while ((1u << (exponent + 1)) <= microframes) {
        exponent++;
    }
    if (speed != SPEED_HIGH && speed != SPEED_SUPER) {
        if (exponent < 3) {
            exponent = 3;
        }
        if (exponent > 10) {
            exponent = 10;
        }
    }

    return exponent;
}

/*
 * Tell the controller about the endpoint the device reports on, and arm it.
 *
 * The input context describes what to add: the slot again, with its count of
 * contexts raised to cover the new endpoint, and the endpoint itself. Configure
 * Endpoint then makes it real, after which a transfer request block left on its
 * ring is filled in whenever the device has something to say.
 */
static bool configure_interrupt_endpoint(device_t *device, volatile void *input, uintptr_t input_paddr,
                                         uint8_t address, uint16_t packet, uint8_t interval)
{
    volatile uint32_t *control_context = context_at(input, 0);
    volatile uint32_t *slot_context = context_at(input, 1 + XHCI_SLOT_CONTEXT);
    volatile uint32_t *endpoint_context;
    uint32_t number = address & 0x0f;
    uint32_t dci = number * 2 + 1;              /* an IN endpoint's index */
    xhci_trb_t completion;

    device->ring = dma_page(RING_ENTRIES * sizeof(xhci_trb_t), &device->ring_paddr);
    device->report = dma_page(4096, &device->report_paddr);
    if (device->ring == NULL || device->report == NULL) {
        note_failure("no ep memory");
        return false;
    }
    device->ring_cycle = XHCI_TRB_CYCLE;
    device->ring_index = 0;
    link_ring(device->ring, device->ring_paddr);

    control_context[XHCI_INPUT_DROP_FLAGS] = 0;
    control_context[XHCI_INPUT_ADD_FLAGS] = XHCI_INPUT_ADD_SLOT | (1u << dci);

    /* The slot has to say how many contexts follow it, or the controller will
     * not look at the one just added. */
    slot_context[0] = (slot_context[0] & ~(0x1fu << 27)) | (dci << 27);

    endpoint_context = context_at(input, 1 + dci);
    endpoint_context[0] = interval_exponent(device->speed, interval) << 16;
    endpoint_context[1] = (3u << 1) | (XHCI_EP_TYPE_INTERRUPT_IN << 3) |
                          ((uint32_t)packet << 16);
    endpoint_context[2] = (uint32_t)(device->ring_paddr | device->ring_cycle);
    endpoint_context[3] = (uint32_t)((uint64_t)device->ring_paddr >> 32);
    /* Average and maximum payload per service interval, which for an interrupt
     * endpoint is one packet. */
    endpoint_context[4] = packet | ((uint32_t)packet << 16);

    memset(&completion, 0, sizeof(completion));
    if (!command(input_paddr,
                 XHCI_TRB_TYPE(XHCI_TRB_CONFIGURE_ENDPOINT) | ((uint32_t)device->slot << 24),
                 &completion)) {
        note_failure("configure ep");
        return false;
    }

    device->dci = (uint8_t)dci;
    device->report_size = packet > 64 ? 64 : packet;

    return true;
}

/*
 * Leave a request on the endpoint's ring for the device to fill.
 *
 * An interrupt endpoint is polled by the controller, but only while there is
 * somewhere to put what it finds. One request is outstanding at a time, replaced
 * as soon as it completes.
 */
static void arm_endpoint(device_t *device)
{
    volatile xhci_trb_t *slot = &device->ring[device->ring_index];

    slot->parameter = device->report_paddr;
    slot->status = device->report_size;
    slot->control = XHCI_TRB_TYPE(XHCI_TRB_NORMAL) | XHCI_TRB_IOC | XHCI_TRB_ISP |
                    device->ring_cycle;

    device->ring_index++;
    if (device->ring_index == RING_ENTRIES - 1) {
        device->ring[RING_ENTRIES - 1].control =
            XHCI_TRB_TYPE(XHCI_TRB_LINK) | XHCI_TRB_TOGGLE_CYCLE | device->ring_cycle;
        device->ring_index = 0;
        device->ring_cycle ^= 1;
    }

    dma_barrier();
    doorbell[device->slot] = device->dci;
    device->armed = true;
}

/*
 * Give the device an address and ask what it is.
 *
 * The input context describes what the controller should set up: the slot, and
 * the control endpoint every device has. Address Device then does the talking to
 * the device itself.
 */
static bool enumerate_port(uint32_t port, device_t *device)
{
    uint32_t speed;
    uintptr_t input_paddr;
    uintptr_t output_paddr;
    uintptr_t ep0_paddr;
    uintptr_t buffer_paddr;
    volatile void *input;
    volatile void *output;
    volatile uint32_t *control_context;
    volatile uint32_t *slot_context;
    volatile uint32_t *ep0_context;
    xhci_trb_t completion;
    volatile usb_device_descriptor_t *descriptor;
    usb_setup_t setup;
    int code;

    if (!reset_port(port)) {
        note_failure("no reset");
        return false;
    }

    /* Only now. On a USB 2 port the speed field means nothing until the reset
     * has established what the device actually is. Recorded here rather than on
     * success, so that a failure can still say what it was talking to. */
    speed = XHCI_PORTSC_SPEED(read32(op, XHCI_PORTSC(port)));
    device->speed = (uint8_t)speed;

    if (!command(0, XHCI_TRB_TYPE(XHCI_TRB_ENABLE_SLOT), &completion)) {
        return false;
    }
    device->slot = XHCI_EVENT_SLOT(completion.control);

    input = dma_page(4096, &input_paddr);
    output = dma_page(4096, &output_paddr);
    device->ep0_ring = dma_page(RING_ENTRIES * sizeof(xhci_trb_t), &ep0_paddr);
    descriptor = dma_page(4096, &buffer_paddr);
    if (input == NULL || output == NULL || device->ep0_ring == NULL || descriptor == NULL) {
        note_failure("no memory");
        return false;
    }
    device->ep0_cycle = XHCI_TRB_CYCLE;
    device->ep0_index = 0;
    link_ring(device->ep0_ring, ep0_paddr);

    /* Add the slot and the control endpoint; drop nothing. Both flags live in
     * the input control context, which is the one before the slot's. */
    control_context = context_at(input, 0);
    control_context[XHCI_INPUT_DROP_FLAGS] = 0;
    control_context[XHCI_INPUT_ADD_FLAGS] = XHCI_INPUT_ADD_SLOT | XHCI_INPUT_ADD_EP0;

    slot_context = context_at(input, 1 + XHCI_SLOT_CONTEXT);
    /* One context entry follows the slot, the control endpoint, and the device
     * hangs off this root port at this speed. */
    slot_context[0] = (1u << 27) | (speed << 20);
    slot_context[1] = port << 16;

    ep0_context = context_at(input, 1 + XHCI_EP0_CONTEXT);
    /*
     * Three errors before giving up, a control endpoint, and the largest packet
     * the speed allows on endpoint zero.
     *
     * The specification fixes this for every speed but full, where it may be 8,
     * 16, 32 or 64 and only the device knows. Eight is the one size every device
     * must accept, so that is what is used until it has been asked.
     */
    ep0_context[1] = (3u << 1) | (XHCI_EP_TYPE_CONTROL << 3) |
                     (ep0_packet_size(speed) << 16);
    ep0_context[2] = (uint32_t)(ep0_paddr | device->ep0_cycle);
    ep0_context[3] = (uint32_t)((uint64_t)ep0_paddr >> 32);
    ep0_context[4] = 8;

    dcbaa[device->slot] = output_paddr;

    /*
     * Address the device in two steps.
     *
     * The first sets up the slot without speaking to the device, which leaves it
     * answering at address zero. That is the only state in which it can be asked
     * how large its control endpoint's packets are -- at full speed the answer
     * may be 8, 16, 32 or 64, and getting it wrong is a transaction error.
     */
    memset(&completion, 0, sizeof(completion));
    if (!command(input_paddr,
                 XHCI_TRB_TYPE(XHCI_TRB_ADDRESS_DEVICE) | XHCI_TRB_BSR |
                 ((uint32_t)device->slot << 24),
                 &completion)) {
        note_failure("slot setup");
        return false;
    }

    setup.request_type = USB_DIR_IN;
    setup.request = USB_REQUEST_GET_DESCRIPTOR;
    setup.value = USB_DESCRIPTOR_DEVICE << 8;
    setup.index = 0;
    /* Only as far as the packet size field, which is the eighth byte. A device
     * whose packets are smaller than the whole descriptor would otherwise be
     * asked for more than it can send in one go before its size is known. */
    setup.length = 8;

    /*
     * One attempt only.
     *
     * A transfer that fails leaves the endpoint halted, and a halted endpoint
     * does not look at its ring again until it has been reset and told where to
     * start. Asking a second time without that produces no event at all, which
     * is a worse report than the error that caused it.
     */
    code = control_in(device, setup, descriptor, buffer_paddr);
    if (!transfer_ok(code)) {
        /* Code zero is not a completion code; it means no event ever arrived,
         * which points at the ring or the doorbell rather than at the bus. */
        note_failure(code == 0 ? "no event" : completion_name(code));
        return false;
    }

    if (descriptor->max_packet_size0 != 0) {
        ep0_context[1] = (ep0_context[1] & 0x0000ffffu) |
                         ((uint32_t)descriptor->max_packet_size0 << 16);
    }

    /* Now for real: the same command without the block, which sends SET_ADDRESS
     * and moves the device out of the state every unaddressed device shares. */
    memset(&completion, 0, sizeof(completion));
    if (!command(input_paddr,
                 XHCI_TRB_TYPE(XHCI_TRB_ADDRESS_DEVICE) | ((uint32_t)device->slot << 24),
                 &completion)) {
        note_failure("address");
        return false;
    }

    /*
     * The command took the endpoint's dequeue pointer from the input context,
     * which still names the start of the ring, so the controller is back at the
     * beginning of it. Our own position has to go back with it.
     *
     * Left as it was, the controller re-runs the requests already there while we
     * write further along, and every event after that belongs to a transfer
     * other than the one being waited for. The symptom is a buffer whose
     * contents do not match what the transfer said it did.
     */
    device->ep0_index = 0;
    device->ep0_cycle = XHCI_TRB_CYCLE;

    setup.length = sizeof(usb_device_descriptor_t);
    code = control_in(device, setup, descriptor, buffer_paddr);
    if (!transfer_ok(code)) {
        note_failure("full desc");
        return false;
    }

    device->vendor = descriptor->vendor;
    device->product = descriptor->product;
    device->device_class = descriptor->device_class;
    device->received = (uint8_t)last_transferred;
    device->packet_size = descriptor->max_packet_size0;

    /*
     * Read the configuration twice: once for its header, which says how long the
     * whole thing is, and once for all of it. The endpoints are described in the
     * part that only the second read brings back.
     */
    {
        volatile usb_config_descriptor_t *config = (volatile usb_config_descriptor_t *)descriptor;
        uint16_t total;
        uint8_t address;
        uint16_t packet;
        uint8_t interval;

        setup.value = USB_DESCRIPTOR_CONFIGURATION << 8;
        setup.length = sizeof(usb_config_descriptor_t);
        if (!transfer_ok(control_in(device, setup, config, buffer_paddr))) {
            note_failure("config hdr");
            return false;
        }

        total = config->total_length;
        if (total > 1024) {
            total = 1024;
        }
        setup.length = total;
        if (!transfer_ok(control_in(device, setup, config, buffer_paddr))) {
            note_failure("config");
            return false;
        }

        if (!find_interrupt_endpoint((const volatile uint8_t *)config, total,
                                     &address, &packet, &interval, &device->keyboard)) {
            const volatile uint8_t *raw = (const volatile uint8_t *)config;

            /*
             * Either the device has no interrupt endpoint -- a hub or a storage
             * device has none -- or the descriptor did not arrive. The start of
             * what was read says which: a configuration descriptor begins with
             * its own length, 9, and its type, 2.
             */
            snprintf(failure, sizeof(failure), "noep l%u %02x%02x%02x%02x%02x%02x",
                     total, raw[0], raw[1], raw[2], raw[3], raw[4], raw[5]);
            return true;
        }

        /* Adopt the configuration, which is what makes the endpoint exist. */
        setup.request_type = 0;
        setup.request = USB_REQUEST_SET_CONFIGURATION;
        setup.value = config->value;
        setup.index = 0;
        setup.length = 0;
        if (!transfer_ok(control_out(device, setup))) {
            note_failure("set config");
            return false;
        }

        if (!configure_interrupt_endpoint(device, input, input_paddr,
                                          address, packet, interval)) {
            return false;
        }
        arm_endpoint(device);
    }

    return true;
}

/*
 * Bring up every port with something on it and say what answered.
 *
 * Ports come up unpowered on some controllers and have to be asked. What is
 * behind them is not known until each has been through a reset, been given a
 * slot and been asked for its device descriptor, which is what this does.
 */
static void enumerate_devices(char *summary, size_t size, size_t *used)
{
    int attached = 0;

    for (uint32_t port = 1; port <= max_ports && device_count < MAX_DEVICES; port++) {
        uint32_t status = read32(op, XHCI_PORTSC(port));
        device_t *device = &devices[device_count];

        if ((status & XHCI_PORTSC_POWER) == 0) {
            write32(op, XHCI_PORTSC(port), XHCI_PORTSC_KEEP(status) | XHCI_PORTSC_POWER);
            status = read32(op, XHCI_PORTSC(port));
        }
        if ((status & XHCI_PORTSC_CONNECTED) == 0) {
            continue;
        }

        memset(device, 0, sizeof(*device));
        device->port = (uint8_t)port;
        failure[0] = '\0';
        attached++;

        if (!enumerate_port(port, device)) {
            /*
             * One port failing says nothing about the others, and on a machine
             * with devices soldered to it the one that matters may not be the
             * first. The reason is kept and the search goes on.
             */
            if (*used < size - 20) {
                *used += (size_t)snprintf(summary + *used, size - *used,
                                          " p%u/s%u!%s", port, device->speed, failure);
            }
            continue;
        }
        device_count++;

        if (*used < size - 36) {
            *used += (size_t)snprintf(summary + *used, size - *used,
                                      " p%u=%04x:%04x%s%s%s", port,
                                      device->vendor, device->product,
                                      device->dci != 0 ? "*" : "",
                                      failure[0] != '\0' ? "/" : "",
                                      failure[0] != '\0' ? failure : "");
        }
    }

    if (attached == 0 && *used < size - 8) {
        *used += (size_t)snprintf(summary + *used, size - *used, " -");
    }
}

/*
 * Bring one controller up, from reset to a running host, and say whether it
 * worked. The window is the mapping its registers are reachable through; which
 * machine it belongs to is not something this has to know.
 */
static bool start_controller(volatile uint8_t *window)
{
    uint32_t caplength = *(volatile uint8_t *)window;
    uint32_t params;

    /* Registers with nothing behind them read as ones, and a capability length
     * is a small non-zero number, so this is what rules a window out. */
    if (caplength == 0 || caplength == 0xff) {
        return false;
    }

    base = window;
    op = base + caplength;
    runtime = base + (read32(base, XHCI_RTSOFF) & ~0x1fu);
    doorbell = (volatile uint32_t *)(base + (read32(base, XHCI_DBOFF) & ~0x3u));

    params = read32(base, XHCI_HCSPARAMS1);
    max_slots = XHCI_HCSPARAMS1_MAX_SLOTS(params);
    max_ports = XHCI_HCSPARAMS1_MAX_PORTS(params);
    large_contexts = (read32(base, XHCI_HCCPARAMS1) & XHCI_HCCPARAMS1_CSZ) != 0;

    /* Each controller gets rings of its own, so the bookkeeping starts over. */
    command_index = 0;
    command_cycle = XHCI_TRB_CYCLE;
    event_index = 0;
    event_cycle = XHCI_TRB_CYCLE;

    if (!reset_controller()) {
        return false;
    }

    /* How many device slots the controller should make room for. */
    write32(op, XHCI_CONFIG, max_slots);

    if (!setup_rings()) {
        return false;
    }

    write32(op, XHCI_USBCMD, read32(op, XHCI_USBCMD) | XHCI_USBCMD_RUN);
    if (!wait_until_clear(op, XHCI_USBSTS, XHCI_USBSTS_HALTED)) {
        note_failure("no start");
        return false;
    }

    return true;
}

bool xhci_init(void)
{
    /* Every window the assembly maps. Which of them lead anywhere depends on the
     * machine, and asking is cheaper than knowing. */
    volatile void *const windows[] = { xhci, xhci_alt, xhci_qemu };
    pci_address_t address;
    char summary[200];
    size_t used = 0;

    if (initialised) {
        return running;
    }
    initialised = true;

    controller_count = pci_count_class(PCI_CLASS_XHCI);
    if (!pci_find_class(PCI_CLASS_XHCI, 0, &address)) {
        report("no controller found");
        return false;
    }

    /*
     * Every controller's address. Only the ones the assembly maps can be used,
     * so when a device is missing this is what says where else to look.
     */
    for (unsigned int i = 0; i < controller_count; i++) {
        pci_address_t other;
        size_t at = strlen(controller_bars);

        if (!pci_find_class(PCI_CLASS_XHCI, i, &other) ||
            at >= sizeof(controller_bars) - 16) {
            break;
        }
        snprintf(controller_bars + at, sizeof(controller_bars) - at, "%s%llx",
                 i == 0 ? "" : ",", (unsigned long long)pci_bar64(other, 0));
        /* Each controller fetches its own rings, which it may not do until bus
         * mastering is on. Nothing below would work without it and nothing would
         * say why. */
        pci_enable_bus_master(other);
    }

    for (size_t i = 0; i < ARRAY_SIZE(windows); i++) {
        if (windows[i] == NULL) {
            continue;
        }
        if (!start_controller((volatile uint8_t *)windows[i])) {
            continue;
        }
        running = true;

        if (used < sizeof(summary) - 8) {
            used += (size_t)snprintf(summary + used, sizeof(summary) - used,
                                     " h%u:", (unsigned)i);
        }
        enumerate_devices(summary, sizeof(summary), &used);
    }

    if (!running) {
        report("c%u[%s] no mapped controller answered", controller_count, controller_bars);
        return false;
    }

    report("c%u[%s]%s", controller_count, controller_bars, summary);

    return true;
}

/*
 * Take what the devices have said and show it.
 *
 * A transfer event names the slot and the endpoint it belongs to, so one pass
 * over the event ring serves every device. The report is shown as raw bytes:
 * the Steam Deck's controller is not a keyboard and no specification says what
 * its bytes mean, so the only way to learn is to press something and watch which
 * of them change.
 */
void xhci_poll(void)
{
    xhci_trb_t event;

    if (!running) {
        return;
    }

    while (next_event(&event)) {
        uint32_t slot = XHCI_EVENT_SLOT(event.control);
        device_t *device = NULL;

        if (XHCI_TRB_TYPE_OF(event.control) != XHCI_TRB_TRANSFER_EVENT) {
            continue;
        }
        for (int i = 0; i < device_count; i++) {
            if (devices[i].slot == slot && devices[i].dci != 0) {
                device = &devices[i];
                break;
            }
        }
        if (device == NULL) {
            continue;
        }

        {
            uint32_t got = device->report_size - XHCI_EVENT_RESIDUAL(event.status);

            if (got > device->report_size) {
                got = 0;
            }
            if (device->keyboard) {
                hid_keyboard_report(device->report, got, device->previous);
            }
        }

        /* One request at a time, replaced as soon as it is answered. */
        arm_endpoint(device);
    }
}

/* Runs after the console can print, which is the only way to see any of this on
 * a machine with no serial port. */
static void CONSTRUCTOR(MUSLCSYS_WITH_VSYSCALL_PRIORITY + 2) xhci_startup(void)
{
    (void)xhci_init();
}
