#ifndef X86_PCI_H
#define X86_PCI_H
#include <stdint.h>
#include <stdbool.h>

struct pci_loc { uint8_t bus, dev, fn; };

uint32_t pci_read32(struct pci_loc l, uint8_t off);
void     pci_write32(struct pci_loc l, uint8_t off, uint32_t v);
uint16_t pci_read16(struct pci_loc l, uint8_t off);
void     pci_write16(struct pci_loc l, uint8_t off, uint16_t v);

/* Walk bus 0 and every bus behind a PCI-PCI bridge; find the first function
 * matching vendor and any device id in [dev_lo, dev_hi]. */
bool pci_find(uint16_t vendor, uint16_t dev_lo, uint16_t dev_hi,
              struct pci_loc *out);

#define PCI_COMMAND        0x04
#define PCI_CMD_IO         0x01
#define PCI_CMD_MEM        0x02
#define PCI_CMD_BUSMASTER  0x04
#define PCI_BAR0           0x10

#endif
