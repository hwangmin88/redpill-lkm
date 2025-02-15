#ifndef REDPILLLKM_RUNTIME_CONFIG_H
#define REDPILLLKM_RUNTIME_CONFIG_H

#include "uart_defs.h" //UART config values
#include "../shim/pci_shim.h" //pci_shim_device_type
#include <linux/types.h> //bool

//These below are currently known runtime limitations
#define MAX_NET_IFACES 8
#define MAC_ADDR_LEN 12
#define MAX_BLACKLISTED_CMDLINE_TOKENS 10

#ifdef CONFIG_SYNO_BOOT_SATA_DOM
#define NATIVE_SATA_DOM_SUPPORTED //whether SCSI sd.c driver supports native SATA DOM
#endif

//UART-related constants were moved to uart_defs.h, to allow subcomponents to importa a smaller subset than this header

//Defines below are experimentally determined to be sufficient but can often be changed
#define MAX_VPCI_BUSES 8 //adjust if needed, max 256
#define MAX_VPCI_DEVS 16 //adjust if needed, max 256*32=8192
#define MODEL_MAX_LENGTH 10
#define SN_MAX_LENGTH 13

#define VID_PID_EMPTY 0x0000
#define VID_PID_MAX   0xFFFF

typedef unsigned short device_id;
typedef char syno_hw[MODEL_MAX_LENGTH + 1];
typedef char mac_address[MAC_ADDR_LEN + 1];
typedef char serial_no[SN_MAX_LENGTH + 1];
typedef char cmdline_token[];

enum boot_media_type {
    BOOT_MEDIA_USB,
    BOOT_MEDIA_SATA
};

struct boot_media {
    enum boot_media_type type; //                                     Default: BOOT_MEDIA_USB <valid>

    //USB only options
    bool mfg_mode; //emulate mfg mode (valid for USB boot only).      Default: false <valid>
    device_id vid; //Vendor ID of device containing the loader.       Default: empty <valid, use first>
    device_id pid; //Product ID of device containing the loader.      Default: empty <valid, use first>

    //SATA only options
    unsigned long dom_size_mib; //Max size of SATA DOM                Default: 1024 <valid, READ sata_boot_shim.c!!!>
};

struct vpci_device_stub {
    enum pci_shim_device_type type;
    u8 bus;
    u8 dev;
    u8 fn;
    bool multifunction:1;
};

struct hw_config {
    const char *name; //the longest so far is "RR36015xs+++" (12+1)

    struct vpci_device_stub pci_stubs[MAX_VPCI_DEVS];

    //All custom flags
    bool emulate_rtc:1;
    bool swap_serial:1; //Whether ttyS0 and ttyS1 are swapped (reverses CONFIG_SYNO_X86_SERIAL_PORT_SWAP)
    bool reinit_ttyS0:1; //Should the ttyS0 be forcefully re-initialized after module loads
    bool fix_disk_led_ctrl:1; //Disabled libata-scsi bespoke disk led control (which often crashes some v4 platforms)
};

struct runtime_config {
    syno_hw hw; //used to determine quirks.                                Default: empty <invalid>
    serial_no sn; //Used to validate it and warn the user.                 Default: empty <invalid>
    struct boot_media boot_media;
    bool port_thaw; //Currently unknown.                                   Default: true  <valid>
    unsigned short netif_num; //Number of eth interfaces.                  Default: 0     <invalid>
    mac_address *macs[MAX_NET_IFACES]; //MAC addresses of eth interfaces.  Default: []    <invalid>
    cmdline_token *cmdline_blacklist[MAX_BLACKLISTED_CMDLINE_TOKENS];//    Default: []
    const struct hw_config *hw_config;
};
extern struct runtime_config current_config;

/**
 * Takes a raw extracted config and "shakes it a little bit" by validating things & constructing dependent structures
 *
 * Warning: if this function returns false YOU MUST NOT trust the config structure. Other code WILL break as it assumes
 * the config is valid (e.g. doesn't have null ptrs which this function generates).
 * Also, after you call this function you should call free_runtime_config() to clear up memory reservations.
 */
int populate_runtime_config(struct runtime_config *config);

void free_runtime_config(struct runtime_config *config);

#endif //REDPILLLKM_RUNTIME_CONFIG_H
