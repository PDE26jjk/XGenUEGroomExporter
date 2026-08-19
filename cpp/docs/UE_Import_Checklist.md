# UE Import Verification Checklist (developer)

The `.abc` files produced by xgcore must be imported into Unreal Engine by a
developer. The agent does not run UE; this checklist exists so the verification
is quick and unambiguous.

## Sample files

Generated from the reference raw data (`raw_data.bin`, 399,874 curves /
1,999,370 CVs per frame):

- Static: `cpp/build/verification/hair_static.abc` (group `hair1`, group id 3)
- Animated: `cpp/build/verification/hair_anim.abc` (3 frames at 24 fps,
  UV-sorted, group `hair1`, group id 3)

Regenerate with:

```bat
xgcore_test abc hair_static.abc raw_data.bin --group hair1 --group-id 3
xgcore_test abc hair_anim.abc raw_data.bin raw_data.bin raw_data.bin --animate --sort --start 1 --fps 24 --group hair1 --group-id 3
```

Note: the animated sample repeats the same frame three times, so its file size
is nearly identical to the static one (Ogawa shares identical arrays). The
time sampling is still correct (3 samples at 1/24, 2/24, 3/24). A multi-frame
file with genuinely different data grows per frame; see
`cpp/build/verification/anim3.abc` (generated from synthetic frames) for an
example.

## Steps

1. Open or create a UE 5.x project (the reference machine has UE 5.4).
2. Copy `hair_static.abc` into the project's `Content/` folder (e.g. via the
   Content Browser drag-and-drop).
3. Wait for the import and check the Output Log for errors/warnings.
4. Open the imported Groom asset and confirm:
   - Curve count is 399,874
   - Total vertices ~1,999,370
   - Guide/interpolation settings are not garbled (no malformed strands)
5. Repeat for `hair_anim.abc`, then scrub the animation timeline and confirm
   all 3 samples load and the groom does not disappear or glitch.
6. If the group id / group name attributes matter for your project, verify
   they are visible in the Groom's imported attributes
   (`groom_group_name`, `groom_group_id`).

## Expected result

- Static and animated grooms import without errors.
- The curve and vertex counts match the numbers above.
- The animated groom keeps a stable vertex count across all frames.

## Known limitations to check

- UE 5.3+ has a known bug reading guide-weight attributes (`groom_guide`
   etc.); this project currently writes `groom_guide`/`groom_group_id`/
   `groom_group_name`, and `groom_root_uv` is added by a later integration
   step.
- If UE reports an inconsistent vertex count, capture the exact log text and
   report it - xgcore detects this condition and returns an error rather than
   writing a broken file.
