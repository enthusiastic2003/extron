/*
 * Minimal PCI configuration-space access (mechanism #1, ports 0xCF8/0xCFC)
 * and a bus walk. Enough to locate a device by vendor/device id; no resource
 * assignment — the UEFI firmware has already programmed the BARs.
 */
#include "pci.h"
#include "io.h"

static uint32_t cfg_addr(struct pci_loc l, uint8_t off) {
    return 0x80000000u | ((uint32_t)l.bus << 16) | ((uint32_t)l.dev << 11)
         | ((uint32_t)l.fn << 8) | (off & 0xFC);
}

uint32_t pci_read32(struct pci_loc l, uint8_t off) {
    outl(0xCF8, cfg_addr(l, off));
    return inl(0xCFC);
}

void pci_write32(struct pci_loc l, uint8_t off, uint32_t v) {
    outl(0xCF8, cfg_addr(l, off));
    outl(0xCFC, v);
}

uint16_t pci_read16(struct pci_loc l, uint8_t off) {
    return (uint16_t)(pci_read32(l, off) >> ((off & 2) * 8));
}

void pci_write16(struct pci_loc l, uint8_t off, uint16_t v) {
    uint32_t w = pci_read32(l, off);
    int sh = (off & 2) * 8;
    w = (w & ~(0xFFFFu << sh)) | ((uint32_t)v << sh);
    pci_write32(l, off, w);
}

static bool scan_bus(uint8_t bus, int depth, uint16_t vendor,
                     uint16_t dev_lo, uint16_t dev_hi, struct pci_loc *out) {
    if (depth > 8) return false;
    for (uint8_t d = 0; d < 32; d++) {
        struct pci_loc l0 = { bus, d, 0 };
        if ((pci_read32(l0, 0) & 0xFFFF) == 0xFFFF) continue;     /* empty slot */
        bool multi = (pci_read32(l0, 0x0C) >> 16) & 0x80;
        for (uint8_t f = 0; f < (multi ? 8 : 1); f++) {
            struct pci_loc l = { bus, d, f };
            uint32_t id = pci_read32(l, 0);
            if ((id & 0xFFFF) == 0xFFFF) continue;
            uint16_t ven = id & 0xFFFF, dev = id >> 16;
            if (ven == vendor && dev >= dev_lo && dev <= dev_hi) {
                *out = l;
                return true;
            }
            uint32_t cls = pci_read32(l, 0x08) >> 16;             /* class:subclass */
            if (cls == 0x0604) {                                  /* PCI-PCI bridge */
                uint8_t sec = (pci_read32(l, 0x18) >> 8) & 0xFF;
                if (sec && scan_bus(sec, depth + 1, vendor, dev_lo, dev_hi, out))
                    return true;
            }
        }
    }
    return false;
}

bool pci_find(uint16_t vendor, uint16_t dev_lo, uint16_t dev_hi,
              struct pci_loc *out) {
    return scan_bus(0, 0, vendor, dev_lo, dev_hi, out);
}
