/*
 * Register and structure layout from the xHCI specification, revision 1.2.
 *
 * Three register banks, all within the one region the base address register
 * points at. The capability registers say where the other two begin, because
 * their sizes depend on how many slots and interrupters the controller has.
 */

#pragma once

#include <stdint.h>

/* Capability registers, at offset zero. */
#define XHCI_CAPLENGTH 0x00      /* byte: where the operational registers start */
#define XHCI_HCIVERSION 0x02     /* half */
#define XHCI_HCSPARAMS1 0x04
#define XHCI_HCSPARAMS2 0x08
#define XHCI_HCCPARAMS1 0x10
#define XHCI_DBOFF 0x14          /* doorbells, relative to the base */
#define XHCI_RTSOFF 0x18         /* runtime registers, relative to the base */

#define XHCI_HCSPARAMS1_MAX_SLOTS(v) ((v) & 0xff)
#define XHCI_HCSPARAMS1_MAX_PORTS(v) (((v) >> 24) & 0xff)

/* Scratchpad buffers the controller wants for itself, split across two fields
 * for historical reasons. */
#define XHCI_HCSPARAMS2_MAX_SCRATCHPAD(v) \
    ((((v) >> 21) & 0x1f) << 5 | (((v) >> 27) & 0x1f))

#define XHCI_HCCPARAMS1_AC64 0x00000001u  /* addresses may be 64 bit */
#define XHCI_HCCPARAMS1_CSZ 0x00000004u   /* contexts are 64 bytes, not 32 */

/* Operational registers, at CAPLENGTH. */
#define XHCI_USBCMD 0x00
#define XHCI_USBSTS 0x04
#define XHCI_PAGESIZE 0x08
#define XHCI_CRCR 0x18           /* command ring control, 64 bit */
#define XHCI_DCBAAP 0x30         /* device context base address array, 64 bit */
#define XHCI_CONFIG 0x38
#define XHCI_PORTSC(port) (0x400 + 0x10 * ((port) - 1))

#define XHCI_USBCMD_RUN 0x00000001u
#define XHCI_USBCMD_RESET 0x00000002u

#define XHCI_USBSTS_HALTED 0x00000001u
#define XHCI_USBSTS_NOT_READY 0x00000800u

#define XHCI_CRCR_RING_CYCLE 0x00000001u

#define XHCI_PORTSC_CONNECTED 0x00000001u
/* Reads as whether the port is enabled. Writing a one DISABLES it. */
#define XHCI_PORTSC_ENABLED 0x00000002u
/* Writing a one starts a reset. */
#define XHCI_PORTSC_RESET 0x00000010u
#define XHCI_PORTSC_POWER 0x00000200u
#define XHCI_PORTSC_SPEED(v) (((v) >> 10) & 0xf)

/* The change bits, cleared by writing a one to each. */
#define XHCI_PORTSC_CHANGES 0x00fe0000u

/*
 * What is left of a port status register once everything that acts on being
 * written has been taken out.
 *
 * This register cannot be read, modified and written back the way most can.
 * Several of its bits mean one thing when read and do another when written: a
 * one in the enabled bit reports an enabled port but disables it, and a one in
 * the reset bit starts a reset. Writing back what was read therefore undoes the
 * very thing that was just established, and the port goes quiet in a way that
 * only shows up at the first transfer.
 */
#define XHCI_PORTSC_KEEP(v) \
    ((v) & ~(XHCI_PORTSC_ENABLED | XHCI_PORTSC_RESET | XHCI_PORTSC_CHANGES))

/* Runtime registers, at RTSOFF. Interrupter zero begins at offset 0x20. */
#define XHCI_IMAN 0x20
#define XHCI_IMOD 0x24
#define XHCI_ERSTSZ 0x28
#define XHCI_ERSTBA 0x30         /* 64 bit */
#define XHCI_ERDP 0x38           /* 64 bit */

#define XHCI_ERDP_BUSY 0x00000008u

/* A transfer request block: the unit every ring is made of. */
typedef struct {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
} __attribute__((packed)) xhci_trb_t;

#define XHCI_TRB_CYCLE 0x00000001u
#define XHCI_TRB_TOGGLE_CYCLE 0x00000002u
#define XHCI_TRB_TYPE(t) ((uint32_t)(t) << 10)
#define XHCI_TRB_TYPE_OF(control) (((control) >> 10) & 0x3f)

#define XHCI_TRB_NORMAL 1
#define XHCI_TRB_SETUP_STAGE 2
#define XHCI_TRB_DATA_STAGE 3
#define XHCI_TRB_STATUS_STAGE 4
#define XHCI_TRB_LINK 6
#define XHCI_TRB_ENABLE_SLOT 9
#define XHCI_TRB_ADDRESS_DEVICE 11
#define XHCI_TRB_CONFIGURE_ENDPOINT 12
#define XHCI_TRB_NOOP_COMMAND 23
#define XHCI_TRB_TRANSFER_EVENT 32
#define XHCI_TRB_COMMAND_COMPLETE 33
#define XHCI_TRB_PORT_STATUS_CHANGE 34

/* Flags shared by transfer request blocks. */
#define XHCI_TRB_IOC 0x00000020u        /* raise an event when this completes */
#define XHCI_TRB_IMMEDIATE 0x00000040u  /* the parameter is data, not an address */
#define XHCI_TRB_ISP 0x00000004u        /* report a transfer that ends early */
#define XHCI_TRB_DIR_IN 0x00010000u     /* data stage reads from the device */

/*
 * Block Set Address Request.
 *
 * Address Device normally does two things: it tells the controller about the
 * slot, and it sends SET_ADDRESS to the device. With this set it does only the
 * first, which leaves the device reachable at address zero -- long enough to ask
 * it how large its control endpoint's packets are, which at full speed is
 * something only it knows.
 */
#define XHCI_TRB_BSR 0x00000200u

/* A setup stage says what shape of control transfer follows. */
#define XHCI_TRB_TRT_NO_DATA 0x00000000u
#define XHCI_TRB_TRT_IN 0x00030000u

/* The slot a command completion event refers to. */
#define XHCI_EVENT_SLOT(control) (((control) >> 24) & 0xff)

/*
 * Contexts are either 32 or 64 bytes, as the capability parameters say, and are
 * addressed by index rather than by offset for that reason.
 */
#define XHCI_SLOT_CONTEXT 0
#define XHCI_EP0_CONTEXT 1

/*
 * The input control context, which is the first context of an input context and
 * says which of the ones after it the controller should read.
 *
 * These are word offsets within that context, not context indices: the drop and
 * add flags are two adjacent words at its start. Each bit stands for one of the
 * contexts that follow, with bit 0 the slot and bit 1 the control endpoint.
 */
#define XHCI_INPUT_DROP_FLAGS 0
#define XHCI_INPUT_ADD_FLAGS 1

#define XHCI_INPUT_ADD_SLOT 0x1u
#define XHCI_INPUT_ADD_EP0 0x2u

/* Endpoint types, in the endpoint context. */
#define XHCI_EP_TYPE_CONTROL 4
#define XHCI_EP_TYPE_INTERRUPT_IN 7

/* Standard USB requests, as they appear in a setup packet. */
#define USB_REQUEST_GET_DESCRIPTOR 0x06
#define USB_REQUEST_SET_CONFIGURATION 0x09

#define USB_DESCRIPTOR_DEVICE 0x01
#define USB_DESCRIPTOR_CONFIGURATION 0x02

/* Direction, type and recipient, packed into the first byte of a setup packet. */
#define USB_DIR_IN 0x80

typedef struct {
    uint8_t request_type;
    uint8_t request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
} __attribute__((packed)) usb_setup_t;

typedef struct {
    uint8_t length;
    uint8_t type;
    uint16_t total_length;
    uint8_t interfaces;
    uint8_t value;
    uint8_t index;
    uint8_t attributes;
    uint8_t max_power;
} __attribute__((packed)) usb_config_descriptor_t;

#define USB_DESCRIPTOR_INTERFACE 0x04
#define USB_DESCRIPTOR_ENDPOINT 0x05

typedef struct {
    uint8_t length;
    uint8_t type;
    uint8_t number;
    uint8_t alternate;
    uint8_t endpoints;
    uint8_t interface_class;
    uint8_t interface_subclass;
    uint8_t interface_protocol;
    uint8_t index;
} __attribute__((packed)) usb_interface_descriptor_t;

typedef struct {
    uint8_t length;
    uint8_t type;
    uint8_t address;
    uint8_t attributes;
    uint16_t max_packet_size;
    uint8_t interval;
} __attribute__((packed)) usb_endpoint_descriptor_t;

/* In an endpoint's address, the direction; in its attributes, the kind. */
#define USB_ENDPOINT_IN 0x80
#define USB_ENDPOINT_TYPE_MASK 0x03
#define USB_ENDPOINT_TYPE_INTERRUPT 0x03

typedef struct {
    uint8_t length;
    uint8_t type;
    uint16_t usb_version;
    uint8_t device_class;
    uint8_t device_subclass;
    uint8_t device_protocol;
    uint8_t max_packet_size0;
    uint16_t vendor;
    uint16_t product;
    uint16_t device_version;
    uint8_t manufacturer_index;
    uint8_t product_index;
    uint8_t serial_index;
    uint8_t configurations;
} __attribute__((packed)) usb_device_descriptor_t;

#define XHCI_COMPLETION_CODE(status) (((status) >> 24) & 0xff)
/* What a transfer did NOT move. Subtracting it from what was asked for gives
 * what actually arrived, which is the only way to tell a transfer that moved
 * nothing from one that moved everything. */
#define XHCI_EVENT_RESIDUAL(status) ((status) & 0x00ffffffu)
#define XHCI_COMPLETION_SUCCESS 1
#define XHCI_COMPLETION_SHORT_PACKET 13

/* One entry of the event ring segment table. */
typedef struct {
    uint64_t address;
    uint32_t size;
    uint32_t reserved;
} __attribute__((packed)) xhci_erst_entry_t;
