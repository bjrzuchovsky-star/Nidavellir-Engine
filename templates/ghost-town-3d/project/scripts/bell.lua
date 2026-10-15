-- Church bell: rings when a rider walks under it (trigger collider).
function on_trigger_enter(other)
  toast(other:name() .. " rang the bell", 2)
  self:set("rings", self:get("rings", 0) + 1)
  -- ts.quests (optional): counts toward Old Hank's "Ring the Bell".
  if quests then quests.progress("bell", "ring") end
end
