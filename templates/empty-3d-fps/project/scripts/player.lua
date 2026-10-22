-- player.lua: the starter script on the player.
-- Hooks: on_start(), on_tick(dt), on_trigger_enter(other),
-- on_trigger_exit(other), on_interact(rider), on_reload().
-- 3D adds pos3 / set_pos3 / move3 / yaw / set_yaw on entities.
-- Movement and the camera come from the Player component (see the
-- Inspector); this script is where your game rules go.

props = { greeting = "Ready to ride." }

function on_start()
  log(self:name() .. ": " .. props.greeting)
end

function on_tick(dt)
  -- Runs 60 times a second.
end
