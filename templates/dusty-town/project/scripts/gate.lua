-- Corral gate: ride up close and press the action button (E / Space /
-- pad A). The gate swings open (no more wall) and, when close_after is
-- above zero, shuts itself again that many seconds later.
props = {
  open_line = "The gate creaks open.",
  close_after = 0,
  locked = false,
}

function on_start()
  self:set("open", false)
end

local function shut()
  self:set("open", false)
  self:set_collider(true)
  self:show()
  log(self:name() .. " swung shut")
end

function on_interact(rider)
  if self:get("open", false) then
    return
  end
  if props.locked then
    rider:toast("Locked tight. Somebody's got the key.", 2)
    return
  end
  self:set("open", true)
  self:set_collider(false)
  self:hide()
  log(self:name() .. " opened by P" .. rider:slot())
  rider:toast(props.open_line, 2)
  if props.close_after > 0 then
    after(props.close_after, shut)
  end
end
