# Scripted first person (C#)

`scripts/character_movement.cs` drives the Wanderer itself: walk, jump, gravity, mouse look and a ground check. It does not use the Rider 3D plugin, and this claim does not list that plugin.

Open the folder as a claim and press Play. WASD moves, Space jumps, and the mouse looks. Esc frees the cursor. Props (move speed, gravity, jump height, look limits) are on the Wanderer in the Inspector.

The body origin sits on the ground. Its capsule is offset up so the feet stay at `Position.Y`. MainCamera is a child; the script pitches it with `SetLocalRotation`.

If you also put a Player component on Wanderer and switch Rider 3D on, both will try to move the body. Play warns you. Take the Player component off, or switch the plugin off, and let the script drive it.

C# needs the .NET 8 SDK while you edit. An exported game that uses these scripts needs the .NET 8 runtime installed on the player's machine (the SDK is only for building). See docs/scripting.md.
