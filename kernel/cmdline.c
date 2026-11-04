#include <kernel/cmdline.h>
#include <kernel/klibc/string.h>
#include <boot/multiboot2.h>

static char cmdline[512];

void cmdline_init(uint64_t mb2_addr) {
    cmdline[0] = 0;
    if (!mb2_addr)
        return;
    struct multiboot_tag *tag = (struct multiboot_tag *)(mb2_addr + 8);
    while (tag->type != MULTIBOOT_TAG_TYPE_END) {
        if (tag->type == MULTIBOOT_TAG_TYPE_CMDLINE) {
            const char *s = (const char *)tag + 8;
            unsigned n = tag->size > 8 ? tag->size - 8 : 0;
            if (n > sizeof(cmdline) - 1)
                n = sizeof(cmdline) - 1;
            memcpy(cmdline, s, n);
            cmdline[n] = 0;
            return;
        }
        tag = (struct multiboot_tag *)((uint64_t)tag + ((tag->size + 7) & ~7u));
    }
}

const char *cmdline_raw(void) { return cmdline; }

/* Walk the space-separated words; `fn` gets the start and length of each. */
static bool find_word(const char *key, unsigned klen, bool want_value,
                      const char **value, unsigned *vlen) {
    const char *p = cmdline;
    while (*p) {
        while (*p == ' ') p++;
        const char *w = p;
        while (*p && *p != ' ') p++;
        unsigned len = (unsigned)(p - w);
        if (!len)
            break;
        if (want_value) {
            if (len > klen && w[klen] == '=' && !memcmp(w, key, klen)) {
                *value = w + klen + 1;
                *vlen = len - klen - 1;
                return true;
            }
        } else if (len == klen && !memcmp(w, key, klen)) {
            return true;
        }
    }
    return false;
}

bool cmdline_get(const char *key, char *out, unsigned size) {
    const char *v; unsigned vlen;
    if (!size || !find_word(key, (unsigned)strlen(key), true, &v, &vlen))
        return false;
    if (vlen > size - 1)
        vlen = size - 1;
    memcpy(out, v, vlen);
    out[vlen] = 0;
    return true;
}

bool cmdline_has(const char *word) {
    return find_word(word, (unsigned)strlen(word), false, 0, 0);
}
