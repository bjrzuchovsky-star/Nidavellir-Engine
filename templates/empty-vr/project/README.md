# Empty VR

A 3D claim with VR on: an XR rig (VR Rig component) with skinned hands (assets/hand.gltf), teleport locomotion and 45 degree snap turn, a sun, a sky and a ground plane.

## Play

In a headset: push the right stick forward to aim the teleport arc and release to go; flick it left / right to snap turn; squeeze to grab; the menu button opens the comfort menu.

Desktop fallback: with no headset (or ts_game --desktop) the rig rides as a regular third-person rider (WASD, Space, Q / Z), and ts_game --sim drives the rig from the desktop simulator.

## Next steps

- Add Grabbable to props to pick them up and throw them.
- Switch locomotion to Teleport + smooth or tune snap turn on the VR Rig.
- Write your rules in scripts/player.lua.
- No optional plugins are on. Turn them on from the Trading Post (or the Outfitter's Wagon) when you need them.
- Everything in this claim was made by the engine (CC0). Replace it freely.
