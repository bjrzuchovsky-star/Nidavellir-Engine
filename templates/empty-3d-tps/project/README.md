# Empty 3D Third-person

A third-person 3D claim: a character controller (Player component, capsule mesh and collider) with the orbit camera behind it (Cam distance 5 m, Cam height 2 m), a sun, a sky and a ground plane.

## Play

The mouse orbits the camera round the player (Play captures the cursor; Esc frees it, click the view to look again) and the scroll wheel zooms. WASD move relative to the camera and the player turns to face where it walks. Space jumps, E interacts. Q / Z still orbit; set the Player's Look to Keys to leave the cursor alone.

## Next steps

- Swap the capsule for a skinned glTF and add an Animator (idle / walk / run).
- Add level geometry with Create > Primitive or glTF models.
- Write your rules in scripts/player.lua.
- No optional plugins are on. Turn them on from the Trading Post (or the Outfitter's Wagon) when you need them.
- Everything in this claim was made by the engine (CC0). Replace it freely.
