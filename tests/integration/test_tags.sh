#!/usr/bin/env bash
# T17: Setting and retrieving tags.
# T18: Finding objects by tag.

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

init_store
echo 'a' > "$STORE/.t1"
echo 'b' > "$STORE/.t2"
echo 'c' > "$STORE/.t3"
"$VFS" object import "$STORE/.t1" >/dev/null
"$VFS" object import "$STORE/.t2" >/dev/null
"$VFS" object import "$STORE/.t3" >/dev/null
A=$(object_id_matching '\.t1$')
B=$(object_id_matching '\.t2$')
C=$(object_id_matching '\.t3$')

# T17
"$VFS" tag add "$A" alpha >/dev/null
"$VFS" tag add "$A" beta  >/dev/null
"$VFS" tag add "$B" alpha >/dev/null

tagsA=$("$VFS" tag list "$A")
assert_contains "$tagsA" 'alpha'
assert_contains "$tagsA" 'beta'

# Remove one
"$VFS" tag remove "$A" alpha >/dev/null
tagsA2=$("$VFS" tag list "$A")
assert_not_contains "$tagsA2" 'alpha' 'tag removal worked'
assert_contains     "$tagsA2" 'beta'  'other tag survives'

# T18
"$VFS" tag add "$A" gamma >/dev/null
"$VFS" tag add "$B" gamma >/dev/null
hits=$("$VFS" find --tag gamma)
assert_contains "$hits" "$A" 'find by tag returns A'
assert_contains "$hits" "$B" 'find by tag returns B'
assert_not_contains "$hits" "$C" 'find by tag excludes C'

# Non-existent tag returns nothing
miss=$("$VFS" find --tag nope)
assert_not_contains "$miss" "$A"
assert_not_contains "$miss" "$B"
