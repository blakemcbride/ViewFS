#!/usr/bin/env bash
# T16: Setting and retrieving attributes.
# T19: Finding objects by attribute.

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

init_store
echo 'a' > "$STORE/.s1"
echo 'b' > "$STORE/.s2"
"$VFS" object import "$STORE/.s1" >/dev/null
"$VFS" object import "$STORE/.s2" >/dev/null
A=$(object_id_matching '\.s1$')
B=$(object_id_matching '\.s2$')

# T16
"$VFS" attr set "$A" language C       >/dev/null
"$VFS" attr set "$A" project compiler >/dev/null

out=$("$VFS" attr get "$A")
assert_contains "$out" 'language' 'language key listed'
assert_contains "$out" 'C'        'language value listed'
assert_contains "$out" 'project'  'project key listed'
assert_contains "$out" 'compiler' 'project value listed'

"$VFS" attr remove "$A" language >/dev/null
out2=$("$VFS" attr get "$A")
assert_not_contains "$out2" 'language' 'attr removed'
assert_contains     "$out2" 'project'  'other attr survives'

# T19 — find by attribute
"$VFS" attr set "$B" project compiler >/dev/null
hits=$("$VFS" find --attr project=compiler)
assert_contains "$hits" "$A" 'find by attr returns A'
assert_contains "$hits" "$B" 'find by attr returns B'

# Mismatching value does not match
miss=$("$VFS" find --attr project=nope)
assert_not_contains "$miss" "$A" 'mismatching value excludes A'
