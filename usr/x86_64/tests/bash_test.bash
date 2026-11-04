#!/bin/bash
# GNU bash on Extron: language features, expansions, redirections, processes,
# traps and job control. Output: "[bash] what PASS|FAIL" (the runner scans for FAIL).
D=/mnt/bash-$$
rm -rf "$D"; mkdir -p "$D" && cd "$D" || { echo "[bash] workdir FAIL"; exit 1; }
eq() { if [ "$2" = "$3" ]; then echo "[bash] $1 PASS"; else echo "[bash] $1 FAIL (got '$3', want '$2')"; fi; }

eq "version is 5.3"           "5"            "${BASH_VERSINFO[0]}"
# --- variables, arrays, expansion
a=(one two three); eq "indexed array"           "three 3" "${a[2]} ${#a[@]}"
declare -A m=([x]=1 [y]=2); eq "associative array"       "1 2" "${m[x]} ${m[y]}"
eq "assoc keys sorted"        "x y"          "$(printf '%s\n' "${!m[@]}" | sort | tr '\n' ' ' | sed 's/ $//')"
s=hello; eq "param substring"           "ell"  "${s:1:3}"
eq "param replace"            "heLLo"        "${s//l/L}"
eq "param default"            "dflt"         "${undefined_var:-dflt}"
eq "param uppercase"          "HELLO"        "${s^^}"
eq "param strip prefix"       "llo"          "${s#he}"
eq "brace expansion"          "a1 a2 a3"     "$(echo a{1..3})"
eq "arith expansion"          "42"           "$((6*7))"
(( n = 5, n *= 2 )); eq "arith command"         "10" "$n"
eq "printf -v"                "007"          "$(printf -v v '%03d' 7; echo $v)"
eq "command substitution"     "x y"          "$(echo x y)"
eq "tilde expansion"          "$HOME"        "$(echo ~)"
shopt -s extglob; touch a.b b.c; eq "extglob"     "b.c" "$(eval 'echo @(b).c')"
# --- conditionals and loops
[[ abc == a* ]] && r=yes; eq "[[ glob ]]"       "yes" "$r"
[[ abc =~ ^a(b)c$ ]]; eq "[[ regex ]] capture"  "b" "${BASH_REMATCH[1]}"
for i in 1 2 3; do t="$t$i"; done; eq "for loop"       "123" "$t"
i=0; while ((i<3)); do ((i++)); done; eq "while loop"   "3" "$i"
case "foo.txt" in *.txt) r=text;; *) r=other;; esac; eq "case"  "text" "$r"
for ((j=0;j<3;j++)); do k="$k$j"; done; eq "c-style for"   "012" "$k"
# --- functions
f() { local x=$1; echo $((x+1)); }; eq "function + local" "6" "$(f 5)"
g() { return 3; }; g; eq "return status"        "3" "$?"
fib() { local n=$1; ((n<2)) && { echo $n; return; }; echo $(( $(fib $((n-1))) + $(fib $((n-2))) )); }
eq "recursion (fib 10)"       "55"           "$(fib 10)"
# --- redirections and pipelines
echo data > f; eq "redirect >"                  "data" "$(cat f)"
echo more >> f; eq "redirect >>"                "2"    "$(wc -l < f)"
eq "here-string"              "hi"           "$(cat <<< hi)"
eq "here-doc"                 "l1"           "$(cat <<EOT
l1
EOT
)"
eq "process substitution"     "a b"          "$(cat <(echo a) <(echo b) | tr '\n' ' ' | sed 's/ $//')"
exec 3> fd3; echo viafd >&3; exec 3>&-; eq "custom fd"  "viafd" "$(cat fd3)"
eq "stderr redirect"          "err"          "$(ls /nonexistent_dir 2>&1 >/dev/null | grep -o 'No such' | head -1 | sed 's/.*/err/')"
set -o pipefail; false | true; eq "pipefail"    "1" "$?"; set +o pipefail
eq "PIPESTATUS"               "0 1 0"        "$(true | false | true; echo ${PIPESTATUS[*]})"
# --- read / builtins
eq "read from here-string"    "a|b"          "$(read -r x y <<< 'a b'; echo "$x|$y")"
eq "mapfile"                  "3"            "$(printf 'a\nb\nc\n' | { mapfile -t L; echo ${#L[@]}; })"
eq "getopts"                  "-a -b val"    "$(set -- -a -b val; while getopts ab: o; do printf -- '-%s ' $o; [ -n "$OPTARG" ] && printf '%s' "$OPTARG"; done | sed 's/ $//')"
eq "type -t builtin"          "builtin"      "$(type -t cd)"
eq "command -v"               "/bin/ls"      "$(PATH=/bin command -v ls)"
eq "printf %q"                "a\\ b"        "$(printf '%q' 'a b')"
eq "let / declare -i"         "7"            "$(declare -i z=3+4; echo $z)"
eq "readonly blocks assign"   "1"            "$(readonly RO=1; (RO=2) 2>/dev/null; echo $RO)"
eq "set -u unset error"       "127"           "$(bash -c 'set -u; echo $nope' >/dev/null 2>&1; echo $?)"
eq "set -e aborts"            "1"            "$(bash -c 'set -e; false; echo no' >/dev/null; echo $?)"
eq "alias in -i shell"        "ok"           "$(bash -ic $'alias q=echo\nq ok' 2>/dev/null)"
eq "history builtin exists"   "builtin"      "$(type -t history)"
# --- subshells, exit status, traps
eq "subshell isolation"       "outer"        "$(v=outer; (v=inner); echo $v)"
eq "exit status of subshell"  "4"            "$( (exit 4); echo $? )"
trap 'tr_hit=1' EXIT; trap - EXIT
eq "trap on USR1"             "got"          "$(bash -c 'trap "echo got; exit" USR1; kill -USR1 $$; sleep 1' 2>/dev/null)"
eq "trap EXIT"                "bye"          "$(bash -c 'trap "echo bye" EXIT' )"
eq "trap ERR"                 "err"          "$(bash -c 'trap "echo err" ERR; false' )"
# --- processes and job control
sleep 0.3 & p=$!; wait $p; eq "wait on background pid"  "0" "$?"
(exit 7) & wait $!; eq "wait returns status"             "7" "$?"
sleep 5 & q=$!; kill $q; wait $q 2>/dev/null; eq "kill + wait (143)" "143" "$?"
eq "\$! and jobs"             "1"            "$( sleep 3 & jobs | wc -l; kill %1 2>/dev/null )"
eq "coproc"                   "ping"         "$(coproc C { read x; echo $x; }; echo ping >&${C[1]}; read y <&${C[0]}; echo $y)"
eq "command substitution exit" "3"           "$(echo $(exit 3; ) >/dev/null; bash -c 'x=$(exit 3); echo $?')"
eq "exec replaces shell"      "replaced"     "$(bash -c 'exec echo replaced')"
eq "env passes to child"      "val"          "$(FOO=val bash -c 'echo $FOO')"
eq "export"                   "exp"          "$(export E=exp; bash -c 'echo $E')"
eq "script from file"         "from-file"    "$(echo 'echo from-file' > s.sh; bash s.sh)"
eq "source"                   "sourced"      "$(echo 'echo sourced' > src.sh; . ./src.sh)"
eq "shebang script"           "she"          "$(printf '#!/bin/bash\necho she\n' > sb; chmod +x sb; ./sb)"
eq "read -t timeout"          "1"            "$(read -t 0.2 x < <(sleep 2); echo $(( $? > 128 )))"
eq "\$RANDOM varies"          "1"            "$([ "$RANDOM" != "$RANDOM" ] && echo 1)"
eq "\$SECONDS / date"         "1"            "$(printf '%(%Y)T' -1 | grep -c '^20')"
eq "wait -n"                  "0"            "$(sleep 0.1 & sleep 0.5 & wait -n; echo $?)"
eq "ulimit -n"                "32"           "$(ulimit -n)"
eq "umask"                    "0022"         "$(umask)"
eq "pwd / cd"                 "/tmp"         "$(cd /tmp && pwd)"
eq "pushd/popd"               "$D"           "$(cd "$D"; pushd /tmp >/dev/null; popd >/dev/null; pwd)"
eq "hash / type -P"           "yes"          "$(type -P cat >/dev/null && echo yes)"
eq "test -nt/-ef"             "yes"          "$(touch -d @1 old; touch new; [ new -nt old ] && echo yes)"
eq "glob sorted"              "a.b b.c f"    "$(echo * | sed 's/ fd3.*//')"
eq "nullglob"                 "0"            "$(shopt -s nullglob; set -- *.zzz; echo $#)"
eq "wildcards with spaces"    "1"            "$(touch 'sp ace'; set -- sp*; echo $#)"
cd /; rm -rf "$D"
