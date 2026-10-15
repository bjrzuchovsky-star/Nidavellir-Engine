-- Gold nugget: the first rider through its trigger pockets it. The rider
-- hears about it on the HUD, keeps a running "gold" tally, and the
-- nugget leaves the world.
props = {
  amount = 10,
}

function on_trigger_enter(other)
  if not other:is_player() then
    return
  end
  local purse = other:get("gold", 0) + props.amount
  other:set("gold", purse)
  other:toast("Picked up " .. props.amount .. " gold (" .. purse .. " in the purse)", 2.5)
  log("P" .. other:slot() .. " picked up " .. props.amount .. " gold at " .. self:name())
  self:destroy()
end
