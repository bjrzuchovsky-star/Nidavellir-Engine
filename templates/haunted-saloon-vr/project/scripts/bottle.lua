-- Whiskey bottle: reacts to hands. on_grab(hand) / on_release(hand, speed).
function on_grab(hand)
  self:set("grabs", self:get("grabs", 0) + 1)
  vr.haptic(hand, 0.3, 0.05)
end

function on_release(hand, speed)
  if speed > 3 then
    toast("Yeehaw! " .. string.format("%.1f", speed) .. " m/s", 1.5)
  end
end
