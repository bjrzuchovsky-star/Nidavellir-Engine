-- player.lua: the starter script on the Player.
-- Hooks: on_start(), on_tick(dt), on_trigger_enter(other),
-- on_trigger_exit(other), on_interact(rider), on_reload().
-- Movement comes from the Player Controller component (see the Inspector);
-- this script is where your game rules go.

props = { greeting = "Ready to ride.", fall_limit = 2000 }

local start_x, start_y = 0, 0

function on_start()
  start_x, start_y = self:pos()
  log(self:name() .. ": " .. props.greeting)
end

function on_tick(dt)
  -- Runs 60 times a second. Example: fell off the map? Back to the start.
  local x, y = self:pos()
  if y > start_y + props.fall_limit then self:set_pos(start_x, start_y) end
end
