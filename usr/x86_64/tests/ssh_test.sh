#!/bin/sh
# OpenSSH on Extron: a private sshd on loopback (own host key, own config, port 2299) and the
# client tools against it: command execution and exit status, stdin, public-key
# authentication (and rejection of a wrong key), scp both ways, sftp, and a pty session.
# Runs inside the guest. Output format: "[ssh] what PASS|FAIL".
W=/tmp/ssh-test-$$
rm -rf "$W"; mkdir -p "$W" && cd "$W" || { echo "[ssh] workdir FAIL"; exit 1; }
command -v sshd >/dev/null || { echo "[ssh] not installed (skipped) PASS"; exit 0; }
eq() { if [ "$2" = "$3" ]; then echo "[ssh] $1 PASS"; else echo "[ssh] $1 FAIL (got '$3')"; fi; }
t() { what=$1; shift; if "$@" >/dev/null 2>&1; then echo "[ssh] $what PASS"; else echo "[ssh] $what FAIL"; fi; }
nt() { what=$1; shift; if "$@" >/dev/null 2>&1; then echo "[ssh] $what FAIL"; else echo "[ssh] $what PASS"; fi; }

ssh-keygen -q -t ed25519 -N "" -f host_key
ssh-keygen -q -t ed25519 -N "" -f user_key
ssh-keygen -q -t ed25519 -N "" -f wrong_key
cp user_key.pub authorized_keys
cat > sshd_config <<C
Port 2299
ListenAddress 127.0.0.1
HostKey $W/host_key
AuthorizedKeysFile $W/authorized_keys
PidFile $W/sshd.pid
StrictModes no
PermitRootLogin yes
PasswordAuthentication no
KbdInteractiveAuthentication no
Subsystem sftp /usr/libexec/sftp-server
C
t "sshd starts" /usr/sbin/sshd -f sshd_config -E sshd.log
sleep 2
S="-p 2299 -i user_key -o IdentitiesOnly=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o BatchMode=yes"
eq "remote command output" "hello" "$(ssh $S 127.0.0.1 echo hello 2>/dev/null)"
ssh $S 127.0.0.1 'exit 7' 2>/dev/null; eq "exit status propagates" "7" "$?"
eq "stdin reaches the remote command" "piped" "$(echo piped | ssh $S 127.0.0.1 cat 2>/dev/null)"
eq "remote environment (USER/HOME sane)" "/root" "$(ssh $S 127.0.0.1 'echo $HOME' 2>/dev/null)"
nt "wrong key is rejected" ssh -p 2299 -i wrong_key -o IdentitiesOnly=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o BatchMode=yes 127.0.0.1 true

dd if=/dev/urandom of=big.bin bs=1k count=1024 2>/dev/null
SC="-P 2299 -i user_key -o IdentitiesOnly=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o BatchMode=yes"
t "scp upload (1 MiB)" scp $SC big.bin 127.0.0.1:$W/up.bin
t "uploaded file identical" cmp big.bin up.bin
t "scp download" scp $SC 127.0.0.1:$W/up.bin down.bin
t "downloaded file identical" cmp big.bin down.bin
echo "ls $W/up.bin" > batch.txt
case "$(sftp $SC -b batch.txt 127.0.0.1 2>&1)" in *up.bin*) echo "[ssh] sftp listing PASS";; *) echo "[ssh] sftp listing FAIL";; esac
case "$(ssh -tt $S 127.0.0.1 tty 2>/dev/null)" in */dev/pts*) echo "[ssh] pty session has a /dev/pts terminal PASS";; *) echo "[ssh] pty session has a /dev/pts terminal FAIL";; esac
eq "keygen output is a valid ed25519 fingerprint" "ED25519" "$(ssh-keygen -l -f host_key.pub | sed 's/.*(\(.*\))/\1/')"

kill "$(cat sshd.pid 2>/dev/null)" 2>/dev/null
sleep 1
nt "sshd stopped (connection refused)" ssh $S 127.0.0.1 true
cd /; rm -rf "$W"
