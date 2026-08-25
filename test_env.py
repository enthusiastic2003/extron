import pexpect
import sys

print("Starting QEMU...")
child = pexpect.spawn('make run', encoding='utf-8')
child.logfile = sys.stdout

child.expect('/ #', timeout=10)
child.sendline('export TERM=xterm')
child.expect('/ #', timeout=5)

child.sendline('./getenv_test.elf')
child.expect('/ #', timeout=5)

child.sendline('poweroff')
