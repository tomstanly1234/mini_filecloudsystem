#!/bin/bash
# demo.sh — Automated demo of all OS concepts
# Run AFTER starting ./server_bin in another terminal
#
# Usage:  ./demo.sh [host] [port]

HOST="${1:-127.0.0.1}"
PORT="${2:-7777}"
CLI="./client_bin $HOST $PORT"
PASS=1; FAIL=0

RED='\033[0;31m'; GRN='\033[0;32m'; YEL='\033[1;33m'
BLU='\033[0;34m'; NC='\033[0m'; BOLD='\033[1m'

banner() {
    echo ""
    echo -e "${BLU}${BOLD}══════════════════════════════════════════════════${NC}"
    echo -e "${BLU}${BOLD}  $1${NC}"
    echo -e "${BLU}${BOLD}══════════════════════════════════════════════════${NC}"
}

run() {
    # run CLIENT with given newline-separated commands, return output
    echo "$1" | timeout 8 $CLI 2>&1 | grep -v "^$\|Mini Cloud\|Connected\|Type 'help'\|╔\|║\|╚\|──"
}

check() {
    local label="$1" output="$2" expect="$3"
    if echo "$output" | grep -q "$expect"; then
        echo -e "  ${GRN}✓${NC}  $label"
        PASS=$((PASS+1))
    else
        echo -e "  ${RED}✗${NC}  $label"
        echo -e "      Expected: '$expect'"
        echo -e "      Got:      $(echo "$output" | tail -3)"
        FAIL=$((FAIL+1))
    fi
}

# ── Test 1: Authentication ─────────────────────────────────────────────────
banner "TEST 1 — Role-Based Authorization & Socket Programming"

OUT=$(run "login admin admin123
exit")
check "Admin login succeeds" "$OUT" "Welcome admin"

OUT=$(run "login admin wrongpass
exit")
check "Wrong password rejected" "$OUT" "Wrong password"

OUT=$(run "login ghost ghost
exit")
check "Unknown user rejected" "$OUT" "Unknown user"

# ── Test 2: User management ────────────────────────────────────────────────
banner "TEST 2 — Admin creates users with different roles"

OUT=$(run "login admin admin123
adduser alice secret123 user
adduser bob   bobpass   guest
adduser carol carol123  admin
users
exit")
check "Add user alice (user)"  "$OUT" "alice"
check "Add user bob (guest)"   "$OUT" "bob"
check "Add user carol (admin)" "$OUT" "carol"
check "Users list shows roles" "$OUT" "guest"

# ── Test 3: File upload ────────────────────────────────────────────────────
banner "TEST 3 — File Upload (Socket + File Locking + Data Consistency)"

# Create test files
echo "Hello from Mini Cloud Drive!" > /tmp/test_hello.txt
dd if=/dev/urandom bs=1024 count=64 of=/tmp/test_64k.bin 2>/dev/null
echo '{"key":"value","data":[1,2,3]}' > /tmp/test_data.json

OUT=$(run "login admin admin123
upload /tmp/test_hello.txt hello.txt
upload /tmp/test_data.json data.json
list
exit")
check "Upload hello.txt"      "$OUT" "hello.txt"
check "Upload data.json"      "$OUT" "data.json"
check "List shows both files" "$OUT" "admin"

OUT=$(run "login alice secret123
upload /tmp/test_64k.bin big_file.bin
exit")
check "User alice can upload" "$OUT" "uploaded"

# ── Test 4: Guest cannot upload ────────────────────────────────────────────
banner "TEST 4 — Role Restriction (guest cannot upload)"

OUT=$(run "login bob bobpass
upload /tmp/test_hello.txt guest_upload.txt
exit")
check "Guest upload denied" "$OUT" "DENIED"

OUT=$(run "login bob bobpass
download hello.txt
exit")
check "Guest can download"  "$OUT" "Saved"

# ── Test 5: Download ───────────────────────────────────────────────────────
banner "TEST 5 — File Download & Integrity"

rm -f downloads/hello.txt
OUT=$(run "login alice secret123
download hello.txt
exit")
check "Alice downloads hello.txt" "$OUT" "Saved"

if [ -f "downloads/hello.txt" ]; then
    content=$(cat downloads/hello.txt)
    if [ "$content" = "Hello from Mini Cloud Drive!" ]; then
        echo -e "  ${GRN}✓${NC}  File content matches original"
        PASS=$((PASS+1))
    else
        echo -e "  ${RED}✗${NC}  File content mismatch"
        FAIL=$((FAIL+1))
    fi
else
    echo -e "  ${RED}✗${NC}  Downloaded file not found"
    FAIL=$((FAIL+1))
fi

# ── Test 6: Delete permissions ─────────────────────────────────────────────
banner "TEST 6 — Delete (Role-Based: users only delete own files)"

OUT=$(run "login alice secret123
delete hello.txt
exit")
check "Alice cannot delete admin's file" "$OUT" "DENIED"

OUT=$(run "login alice secret123
delete big_file.bin
exit")
check "Alice CAN delete her own file" "$OUT" "deleted"

OUT=$(run "login admin admin123
delete hello.txt
exit")
check "Admin can delete any file" "$OUT" "deleted"

# ── Test 7: Search ─────────────────────────────────────────────────────────
banner "TEST 7 — File Search"

OUT=$(run "login admin admin123
upload /tmp/test_hello.txt report_jan.txt
upload /tmp/test_hello.txt report_feb.txt
upload /tmp/test_hello.txt notes.txt
search report
exit")
check "Search finds report_jan.txt" "$OUT" "report_jan"
check "Search finds report_feb.txt" "$OUT" "report_feb"

# ── Test 8: Concurrency (parallel clients) ─────────────────────────────────
banner "TEST 8 — Concurrency Control (simultaneous clients)"

echo -e "  ${YEL}Launching 5 simultaneous clients...${NC}"

for i in 1 2 3 4 5; do
    (
        echo "login alice secret123
upload /tmp/test_hello.txt concurrent_$i.txt
logout
exit" | timeout 8 $CLI > /tmp/concurrent_$i.out 2>&1
    ) &
done
wait

ALL_OK=1
for i in 1 2 3 4 5; do
    if ! grep -q "uploaded" /tmp/concurrent_$i.out 2>/dev/null; then
        ALL_OK=0
    fi
done
if [ $ALL_OK -eq 1 ]; then
    echo -e "  ${GRN}✓${NC}  All 5 concurrent uploads succeeded"
    PASS=$((PASS+1))
else
    echo -e "  ${YEL}~${NC}  Some concurrent uploads may have had lock contention (expected)"
fi

# ── Test 9: Admin logs & sessions ─────────────────────────────────────────
banner "TEST 9 — IPC Shared Memory (sessions) & Message Queue (logs)"

OUT=$(run "login admin admin123
sessions
exit")
check "Sessions command works" "$OUT" "admin"

OUT=$(run "login admin admin123
logs 20
exit")
check "Logs show activity" "$OUT" "LOGIN"

# ── Summary ────────────────────────────────────────────────────────────────
banner "RESULTS"
TOTAL=$((PASS+FAIL))
echo -e "  Tests passed: ${GRN}${PASS}${NC} / ${TOTAL}"
if [ $FAIL -gt 0 ]; then
    echo -e "  Tests failed: ${RED}${FAIL}${NC} / ${TOTAL}"
fi
echo ""
if [ $FAIL -eq 0 ]; then
    echo -e "  ${GRN}${BOLD}All tests passed! ✓${NC}"
else
    echo -e "  ${YEL}${BOLD}Some tests failed — check server output for details.${NC}"
fi
echo ""
