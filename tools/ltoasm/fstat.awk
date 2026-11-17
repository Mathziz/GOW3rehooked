# Per-function stats from clang -S output (COFF/AT&T). Usage: awk -v mod=NAME -f fstat.awk file.s
function flush() {
    if (fn != "") printf "%s\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n", mod, fn, n, calls, spill, vec, jcc, div, rep, lines
    fn = ""
}
/^[^ \t.#][^ \t]*:/ {
    if (!(substr($0, 1, 3) == "\"?" && 0)) { flush(); fn = $0; sub(/:.*/, "", fn); gsub(/"/, "", fn); n = calls = spill = vec = jcc = div = rep = lines = 0; next }
}
/^\.Lfunc_end/ { flush(); next }
fn != "" && /^\t[a-z]/ {
    n++
    op = $1
    if (op ~ /^call/) calls++
    else if (op ~ /^j[a-z]+$/ && op != "jmp") jcc++
    if (op ~ /^v/ || op ~ /ps$|pd$|ss$|sd$/) vec++
    if (op ~ /^i?div/ || op ~ /^v?div/) div++
    if (op ~ /^rep/) rep++
    if ($0 ~ /\(%rsp\)|\(%rbp\)/ && op ~ /^(v?mov|push)/) spill++
    lines++
}
END { flush() }
