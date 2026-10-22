-- player.lua: the starter script on the VR Rig.
-- Hooks: on_start(), on_tick(dt), on_grab(hand), on_release(hand, speed),
-- on_point / on_unpoint / on_vr_trigger on the things you point at.
-- The `vr` table (vr.active, vr.head, vr.hand, vr.teleport, vr.haptic)
-- reaches the headset; on the desktop the rig rides as a regular rider.

props = { greeting = "Ready to ride." }

function on_start()
  log(self:name() .. ": " .. props.greeting)
end

function on_tick(dt)
  -- Runs 60 times a second.
end
