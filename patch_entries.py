import sys

devfs_c = "kernel/fs/devfs.c"
with open(devfs_c, "r") as f:
    content = f.read()

idx = content.find('{ .name = "console", .node = &console_node },')
content = content[:idx] + '{ .name = "ptmx",    .node = &ptmx_node },\n    ' + content[idx:]

with open(devfs_c, "w") as f:
    f.write(content)
