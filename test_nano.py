import pexpect
import sys

print("Starting QEMU...")
child = pexpect.spawn('make run', encoding='utf-8')
child.logfile = sys.stdout

child.expect('/ #', timeout=10)
print("Shell ready.")

child.sendline('export TERM=xterm')
child.expect('/ #', timeout=5)

child.sendline('./nano')
try:
    # nano clears the screen and draws the title bar, usually containing "GNU nano"
    child.expect('GNU nano', timeout=5)
    print("\n\nSUCCESS! Nano rendered its UI!")
    child.sendcontrol('x') # Ctrl-X to exit
    child.expect('/ #', timeout=5)
    print("Exited cleanly.")
except pexpect.TIMEOUT:
    print("\n\nFAILED: Did not see nano UI.")

child.sendline('poweroff')
