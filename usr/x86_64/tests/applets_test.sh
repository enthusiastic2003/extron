#!/bin/sh
# Exercise the BusyBox applets on the data disk. Output format matches the C
# tests ("[name] what PASS|FAIL") so the runner's FAIL scan covers it.
D=/mnt/applets-$$
rm -rf "$D"; mkdir -p "$D" && cd "$D" || { echo "[applets] workdir FAIL"; exit 1; }
n=0
ck() { # ck "what" command...
    what=$1; shift
    if "$@" >/dev/null 2>&1; then echo "[applets] $what PASS"; else echo "[applets] $what FAIL"; fi
}
eq() { # eq "what" expected actual
    if [ "$2" = "$3" ]; then echo "[applets] $1 PASS"; else echo "[applets] $1 FAIL (got '$3')"; fi
}

printf 'one\ntwo\nthree\nfour\nfive\n' > f.txt
eq "head -n 2"        "one two"       "$(head -n 2 f.txt | tr '\n' ' ' | sed 's/ $//')"
eq "tail -n 2"        "four five"     "$(tail -n 2 f.txt | tr '\n' ' ' | sed 's/ $//')"
eq "grep match"       "three"         "$(grep thr f.txt)"
eq "grep -v count"    "4"             "$(grep -vc one f.txt)"
eq "egrep alternation" "two three"    "$(egrep 'two|three' f.txt | tr '\n' ' ' | sed 's/ $//')"
eq "sed substitute"   "ONE"           "$(echo one | sed 's/one/ONE/')"
eq "sed -i in place"  "uno"           "$(echo one > s.txt; sed -i 's/one/uno/' s.txt; cat s.txt)"
eq "awk field sum"    "6"             "$(printf '1 2\n3 0\n' | awk '{s+=$1+$2} END{print s}')"
eq "cut -d -f"        "b"             "$(echo a:b:c | cut -d: -f2)"
eq "tr upcase"        "ABC"           "$(echo abc | tr a-z A-Z)"
eq "sort"             "a b c"         "$(printf 'c\na\nb\n' | sort | tr '\n' ' ' | sed 's/ $//')"
eq "sort -n"          "2 10"          "$(printf '10\n2\n' | sort -n | tr '\n' ' ' | sed 's/ $//')"
eq "uniq -c"          "2 x"           "$(printf 'x\nx\n' | uniq -c | sed 's/^ *//')"
eq "wc -l"            "5"             "$(wc -l < f.txt)"
echo hi | tee t1.txt >/dev/null; eq "tee writes file" "hi" "$(cat t1.txt)"
printf 'a\nb\n' > d1; printf 'a\nc\n' > d2
ck "diff detects change" sh -c '! diff d1 d2'
ck "diff identical"      diff d1 d1
ck "cmp identical"       cmp d1 d1
diff -u d1 d2 > p.diff; cp d1 d3; patch d3 < p.diff >/dev/null 2>&1
ck "patch applies diff"  cmp d3 d2

mkdir -p tree/a/b tree/c; echo x > tree/a/b/f1; echo y > tree/c/f2; ln -s ../c tree/a/lnk
eq "find -type f"     "2"             "$(find tree -type f | wc -l)"
eq "find -name"       "tree/c/f2"     "$(find tree -name f2)"
eq "find -exec"       "x"             "$(find tree -name f1 -exec cat {} \;)"
eq "xargs"            "x y"           "$(find tree -type f | sort | xargs cat | tr '\n' ' ' | sed 's/ $//')"
ck "du runs"          du -k tree
dd if=/dev/zero of=z.bin bs=1k count=64 2>/dev/null
eq "dd size"          "65536"         "$(wc -c < z.bin)"
eq "od"               "0000000 141 142"  "$(printf 'ab' | od -b | head -n 1 | sed 's/  */ /g;s/ $//')"
ck "hexdump runs"     hexdump -C f.txt
eq "strings"          "hello world"   "$(printf '\001\002hello world\003' | strings)"
echo abc > h.txt
eq "sha1sum"          "03cfd743661f07975fa2f1220c5194cbaff48451" "$(sha1sum h.txt | cut -d' ' -f1)"
eq "sha256sum"        "edeaaff3f1774ad2888673770c6d64097e391bc362d7d6fb34982ddf0efd18cb" "$(sha256sum h.txt | cut -d' ' -f1)"
eq "md5sum"           "0bee89b07a248e27c83fc3d5951213c1" "$(md5sum h.txt | cut -d' ' -f1)"

tar cf t.tar tree 2>/dev/null
mkdir x && (cd x && tar xf ../t.tar) 2>/dev/null
ck "tar roundtrip"    cmp x/tree/a/b/f1 tree/a/b/f1
ck "tar keeps symlink" sh -c '[ -L x/tree/a/lnk ]'
gzip -c f.txt > f.gz 2>/dev/null
eq "gzip/gunzip"      "$(cat f.txt)"  "$(gunzip -c f.gz 2>/dev/null)"
eq "zcat"             "$(cat f.txt)"  "$(zcat f.gz 2>/dev/null)"
tar czf t.tgz tree 2>/dev/null; mkdir y; (cd y && tar xzf ../t.tgz) 2>/dev/null
ck "tar.gz roundtrip" cmp y/tree/c/f2 tree/c/f2

cp f.txt cp.txt; ck "cp copies" cmp f.txt cp.txt
cp -r tree tree2 2>/dev/null; ck "cp -r" cmp tree2/a/b/f1 tree/a/b/f1
eq "basename"         "f.txt"         "$(basename /a/b/f.txt)"
eq "dirname"          "/a/b"          "$(dirname /a/b/f.txt)"
eq "expr"             "7"             "$(expr 3 + 4)"
eq "seq"              "1 2 3"         "$(seq 3 | tr '\n' ' ' | sed 's/ $//')"
eq "tac"              "c b a"         "$(printf 'a\nb\nc\n' | tac | tr '\n' ' ' | sed 's/ $//')"
eq "rev"              "cba"           "$(echo abc | rev)"
eq "nl"               "1 a"           "$(echo a | nl | sed 's/^ *//;s/\t/ /')"
eq "env sets var"     "bar"           "$(env FOO=bar sh -c 'echo $FOO')"
eq "install -m"       "755"           "$(install -m 755 f.txt inst.txt; stat -c %a inst.txt)"
eq "truncate"         "10"            "$(truncate -s 10 tr.bin; wc -c < tr.bin)"
eq "split"            "2"             "$(split -l 3 f.txt sp_; ls sp_* | wc -l)"
eq "realpath"         "$D/f.txt"      "$(realpath f.txt)"
eq "yes|head"         "y y"           "$(yes | head -n 2 | tr '\n' ' ' | sed 's/ $//')"
ck "mktemp"           sh -c 'f=$(mktemp -p '"$D"') && [ -f "$f" ]'
ck "timeout runs"     timeout 5 true
ck "date runs"        date
ck "sync"             sync
ck "kill -0 self"     kill -0 $$
eq "pipeline rc"      "ok"            "$(true | false; true | true && echo ok)"

# Archives made by the host's tar/zip (fixtures in /opt/tests/data): tar must detect the
# compression by itself, since most source releases are .tar.xz or .tar.bz2.
DD=/opt/tests/data
mkdir ar && cd ar
two() { cat sample/a.txt sample/sub/b.txt | tr '\n' ' ' | sed 's/ $//'; }
tar xf $DD/sample.tar.xz;  eq "tar xf autodetects .tar.xz"  "alpha beta" "$(two)"; rm -rf sample
tar xJf $DD/sample.tar.xz; eq "tar xJf"                     "alpha beta" "$(two)"; rm -rf sample
tar xf $DD/sample.tar.bz2; eq "tar xf autodetects .tar.bz2" "alpha beta" "$(two)"; rm -rf sample
tar xjf $DD/sample.tar.bz2; eq "tar xjf"                    "alpha beta" "$(two)"; rm -rf sample
eq "xzcat | tar t"  "sample/ sample/a.txt sample/sub/ sample/sub/b.txt" \
   "$(xzcat $DD/sample.tar.xz | tar tf - | sort | tr '\n' ' ' | sed 's/ $//')"
eq "bzcat | tar t"  "sample/ sample/a.txt sample/sub/ sample/sub/b.txt" \
   "$(bzcat $DD/sample.tar.bz2 | tar tf - | sort | tr '\n' ' ' | sed 's/ $//')"
eq "unzip -p"       "alpha"         "$(unzip -p $DD/sample.zip sample/a.txt | tr -d '\n')"
eq "tar reads old v7 format (no ustar magic)" "alpha" "$(tar xOf $DD/sample-v7.tar sample/a.txt)"
eq "bzip2 | bzcat round trip" "round-trip" "$(printf round-trip | bzip2 | bzcat)"
cd ..

cd /; rm -rf "$D"
ck "rm -rf cleanup"   sh -c "[ ! -e $D ]"
echo "[applets] done"
