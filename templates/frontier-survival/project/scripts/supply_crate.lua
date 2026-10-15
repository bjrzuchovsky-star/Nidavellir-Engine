-- Supply crate: the first rider to bump into it gets a starter kit
-- (Frontier Survival's `frontier` table), then the crate is gone.
props = {
  wood = 12,
  stone = 6,
}

function on_trigger_enter(other)
  if not other:is_player() then
    return
  end
  local slot = other:slot()
  frontier.give(slot, "wood", props.wood)
  frontier.give(slot, "stone", props.stone)
  frontier.give(slot, "jerky", 2)
  other:toast("Supply crate: " .. props.wood .. " logs, " .. props.stone .. " stone, jerky. Ride safe.", 3)
  log("P" .. slot .. " opened the supply crate (" .. frontier.count(slot, "wood") .. " logs now)")
  self:destroy()
end
