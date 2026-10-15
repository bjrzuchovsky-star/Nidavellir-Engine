-- The saloon ghost: bobs over the piano. Point the laser at it
-- (on_point / on_unpoint) and pull the trigger (on_vr_trigger) to spook
-- it; vr.haptic buzzes the hand that did it.
props = {
  bob = 0.15,   -- metres
}

local t = 0
local base_y = nil

function on_start()
  local _, y, _ = self:pos3()
  base_y = y
end

function on_tick(dt)
  t = t + dt
  local x, _, z = self:pos3()
  self:set_pos3(x, base_y + math.sin(t * 2) * props.bob, z)
  self:spin(30 * dt)
end

function on_point(hand)
  self:set("pointed", self:get("pointed", 0) + 1)
end

function on_vr_trigger(hand)
  local n = self:get("spooks", 0) + 1
  self:set("spooks", n)
  toast("Boo! (" .. n .. ")", 2)
  vr.haptic(hand, 0.8, 0.15)
end
