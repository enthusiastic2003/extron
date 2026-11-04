#!/bin/sh
# GNU coreutils (/usr/gnu/bin, one multi-call binary). Calls them by full path so
# BusyBox applets are never what gets tested. Output format: "[coreutils] what PASS|FAIL".
G=/usr/gnu/bin
[ -x $G/coreutils ] || { echo "[coreutils] not installed (skipped) PASS"; exit 0; }
D=/mnt/coreutils-$$
rm -rf "$D"; mkdir -p "$D" && cd "$D" || { echo "[coreutils] workdir FAIL"; exit 1; }
eq() { if [ "$2" = "$3" ]; then echo "[coreutils] $1 PASS"; else echo "[coreutils] $1 FAIL (got '$3')"; fi; }

eq "version banner"      "ls (GNU coreutils)"  "$($G/ls --version | head -1 | cut -d' ' -f1-3)"
eq "seq | sort -rn"      "5 4 3 2 1"           "$($G/seq 5 | $G/sort -rn | $G/paste -sd' ' -)"
eq "sort | head (EPIPE)" "100000"              "$($G/seq 100000 | $G/sort -rn | $G/head -1)"
eq "tr"                  "ABC"                 "$(echo abc | $G/tr a-z A-Z)"
eq "cut"                 "b"                   "$(echo a:b:c | $G/cut -d: -f2)"
eq "wc -l"               "3"                   "$(printf 'a\nb\nc\n' | $G/wc -l)"
eq "sha256sum"           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" \
                         "$(printf abc | $G/sha256sum | cut -d' ' -f1)"
eq "md5sum"              "900150983cd24fb0d6963f7d28e17f72" "$(printf abc | $G/md5sum | cut -d' ' -f1)"
eq "base64"              "YWJj"                "$(printf abc | $G/base64)"
eq "expr"                "12"                  "$($G/expr 3 \* 4)"
eq "factor"              "360: 2 2 2 3 3 5"    "$($G/factor 360)"
eq "numfmt"              "118M"                "$($G/numfmt --to=iec 123456789)"
eq "date -d @0"          "1970-01-01"          "$($G/date -u -d @0 +%F)"
eq "basename/dirname"    "b /a"                "$($G/basename /a/b.c .c) $($G/dirname /a/b.c)"

echo x > a; $G/cp a b; $G/mv b c; $G/ln -s c d; $G/ln a h
eq "cp/mv/ln -s"         "x"                   "$($G/cat d)"
eq "stat -c"             "regular file symbolic link" "$($G/stat -c %F c | tr '\n' ' ')$($G/stat -c %F d)"
eq "hard link count"     "2"                   "$($G/stat -c %h a)"
$G/mkdir -p t/u/v; $G/touch t/u/v/f; 
eq "ls -R"               "t/u/v:"              "$($G/ls -R t | grep ':$' | tail -1)"
eq "cp -a tree"          "yes"                 "$($G/cp -a t t2 && [ -f t2/u/v/f ] && echo yes)"
eq "cp -a /usr/gnu (stack growth)" "yes"       "$($G/cp -a /usr/gnu g && [ -x g/bin/coreutils ] && echo yes)"
eq "du -s runs"          "yes"                 "$($G/du -s g >/dev/null && echo yes)"
eq "rm -r"               "gone"                "$($G/rm -rf t t2 g; [ ! -e t ] && echo gone)"
$G/yes | $G/head -c 3000000 > big
eq "yes | head -c"       "3000000"             "$($G/stat -c %s big)"
$G/split -b 1000000 big pt_
eq "split + cat + cmp"   "same"                "$($G/cat pt_* | cmp - big && echo same)"
$G/truncate -s 10 big
eq "truncate"            "10"                  "$($G/stat -c %s big)"
eq "df lists /mnt"       "/mnt"                "$($G/df -P /mnt | tail -1 | $G/tr -s ' ' | cut -d' ' -f6)"
eq "nproc"               "1"                   "$($G/nproc)"
eq "timeout kills"       "124"                 "$($G/timeout 1 $G/sleep 5 2>/dev/null; echo $?)"
eq "mktemp"              "yes"                 "$(f=$($G/mktemp) && [ -f "$f" ] && rm -f "$f" && echo yes)"
eq "sleep 0.2"           "ok"                  "$($G/sleep 0.2 && echo ok)"
cd /; rm -rf "$D"
