import sys

syscall_c = "kernel/proc/syscall.c"
with open(syscall_c, "r") as f:
    content = f.read()

idx = content.find("if (result == 0) {")
# after that line, insert ptmx check
insert_code = """
        if (devfs_is_ptmx(f->object.node)) {
            struct vfs_node *master = pty_allocate_master();
            if (!master) {
                file_release(f);
                return (uint64_t)-ENOMEM;
            }
            vfs_node_release(f->object.node); // release the static ptmx node
            f->object.node = master;
        }
"""
idx2 = content.find("f->object.node = node;", idx)
if idx2 != -1:
    content = content[:idx2 + len("f->object.node = node;")] + insert_code + content[idx2 + len("f->object.node = node;"):]
    
with open(syscall_c, "w") as f:
    f.write(content)
