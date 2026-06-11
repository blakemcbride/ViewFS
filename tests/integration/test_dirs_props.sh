#!/usr/bin/env bash
# Views, directories, directory property filters (independent vs flow),
# object properties, find, and computed membership via `dir ls`.
source "$(dirname "$0")/lib.sh"
init_store

"$VFS" view create proj "demo" >/dev/null
assert_contains "$("$VFS" view list)" "proj" "view appears in list"

# Build a tree:  /blake [author=blake, flow]  >  /blake/2024 [year=2024]
"$VFS" dir mkdir proj /blake >/dev/null
"$VFS" prop set author=blake proj:/blake --flow >/dev/null
"$VFS" dir mkdir proj /blake/2024 >/dev/null
"$VFS" prop set year=2024 proj:/blake/2024 >/dev/null

eff=$("$VFS" prop list proj:/blake/2024 --effective)
assert_contains "$eff" "author" "flowed author visible in effective set"
assert_contains "$eff" "year"   "own year visible in effective set"
assert_contains "$eff" "[from /blake]" "effective list names the source dir"

# Objects: a(author=blake,year=2024), b(author=blake), c(author=jane,year=2024)
for f in a b c; do echo "$f" > "$STORE/$f.txt"; done
"$VFS" object import "$STORE/a.txt" --into proj:/blake/2024 >/dev/null  # author(flow)+year
"$VFS" object import "$STORE/b.txt" --into proj:/blake      >/dev/null  # author
cid=$("$VFS" object import "$STORE/c.txt" | awk '{print $1}')
"$VFS" prop set author=jane "$cid" >/dev/null
"$VFS" prop set year=2024 "$cid" >/dev/null

# /blake (author=blake) -> a.txt, b.txt ; NOT c.txt
ls_blake=$("$VFS" dir ls proj /blake)
assert_contains     "$ls_blake" "a.txt" "/blake has a"
assert_contains     "$ls_blake" "b.txt" "/blake has b"
assert_not_contains "$ls_blake" "c.txt" "/blake excludes c (author=jane)"
assert_contains     "$ls_blake" "2024/" "/blake lists child dir"

# /blake/2024 (year=2024 AND flowed author=blake) -> a.txt only
ls_2024=$("$VFS" dir ls proj /blake/2024)
assert_contains     "$ls_2024" "a.txt" "/blake/2024 has a"
assert_not_contains "$ls_2024" "b.txt" "/blake/2024 excludes b (no year)"
assert_not_contains "$ls_2024" "c.txt" "/blake/2024 excludes c (flowed author=blake)"

# find --prop AND
both=$("$VFS" find --prop author=blake --prop year=2024)
assert_contains     "$both" "a.txt" "find AND -> a"
assert_not_contains "$both" "b.txt" "find AND excludes b"
assert_not_contains "$both" "c.txt" "find AND excludes c"

# Multi-value AND in a directory filter: /both needs author in {blake,jane}.
"$VFS" object import "$STORE/a.txt" >/dev/null  # noise, propertyless
bid=$("$VFS" object id proj /blake/b.txt)
"$VFS" prop set author=jane "$bid" >/dev/null   # b now has blake AND jane
"$VFS" dir mkdir proj /both >/dev/null
"$VFS" prop set author=blake proj:/both >/dev/null
"$VFS" prop set author=jane proj:/both >/dev/null
ls_both=$("$VFS" dir ls proj /both)
assert_contains     "$ls_both" "b.txt" "/both -> b (has both authors)"
assert_not_contains "$ls_both" "c.txt" "/both excludes c (only jane)"

# prop unset KEY=VALUE removes just that one value
"$VFS" prop unset author=jane "$bid" >/dev/null
assert_not_contains "$("$VFS" dir ls proj /both)" "b.txt" "after unset, b leaves /both"

# rmdir refuses a dir whose membership is non-empty; allows an empty filter.
expect_failure "$VFS" dir rmdir proj /blake
"$VFS" dir mkdir proj /empty >/dev/null
"$VFS" prop set key=nomatch proj:/empty >/dev/null
"$VFS" dir rmdir proj /empty >/dev/null

assert_contains "$("$VFS" check)" "Store is consistent." "store consistent"
echo "PASS: test_dirs_props"
