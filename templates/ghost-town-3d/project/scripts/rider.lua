-- The rider: the Drifter's animator follows the controller (speed,
-- grounded, jump). Clip events arrive as on_anim_event(name).
function on_anim_event(name)
  if name == "footstep" then
    self:set("steps", self:get("steps", 0) + 1)
  end
end
