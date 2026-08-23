#ifndef TAR_H
#define TAR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct tar_file {
    const char *name;
    size_t size;
    void *data;
};

// Initializes the tar filesystem by locating the multiboot module.
// Note: Pass the physical mb2_addr directly. The function will apply the HHDM offset.
void tar_init(uint64_t mb2_addr);

// Lists all files in the loaded tarball.
void tar_list(void);

// Looks up a file in the loaded tarball.
// Returns true if found and populates 'out', false otherwise.
bool tar_open(const char *name, struct tar_file *out);
typedef void (*tar_visit_fn)(const struct tar_file *file, void *ctx);
void tar_foreach(tar_visit_fn visit, void *ctx);

#endif // TAR_H
