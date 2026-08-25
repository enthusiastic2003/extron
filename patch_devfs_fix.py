import sys

devfs_c = "kernel/fs/devfs.c"
with open(devfs_c, "r") as f:
    content = f.read()

# Fix syntax error
content = content.replace('}{ "ptmx",    &ptmx_node,    {0} },', '{ "ptmx",    &ptmx_node,    {0} },')

# Ensure kheap.h is included
if "<kernel/mm/kheap.h>" not in content:
    content = "#include <kernel/mm/kheap.h>\n" + content

with open(devfs_c, "w") as f:
    f.write(content)
