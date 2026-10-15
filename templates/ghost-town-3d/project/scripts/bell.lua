-- Church bell: rings when a rider walks under it (trigger collider).
function on_trigger_enter(other)
  toast(other:name() .. " rang the bell", 2)
  self:set("rings", self:get("rings", 0) + 1)
end
