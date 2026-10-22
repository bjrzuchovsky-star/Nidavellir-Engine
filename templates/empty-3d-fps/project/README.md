# Empty 3D First-person

A first-person 3D claim: an FPS character controller (Player component, View: First person, capsule collider), its Camera at eye height (a child of the player), a sun, a sky and a ground plane.

## Play

The mouse looks (Play captures the cursor; Esc frees it, click the view to look again), WASD move where you look, Space jumps, E interacts. A gamepad's right stick looks too. Q / Z and Page Up / Page Down still turn and tilt; set the Player's Look to Keys to leave the cursor alone.

## Next steps

- Move the Camera child to change the eye point; its FOV is the view's FOV.
- Add level geometry with Create > Primitive or glTF models.
- Write your rules in scripts/player.lua.
- No optional plugins are on. Turn them on from the Trading Post (or the Outfitter's Wagon) when you need them.
- Everything in this claim was made by the engine (CC0). Replace it freely.
