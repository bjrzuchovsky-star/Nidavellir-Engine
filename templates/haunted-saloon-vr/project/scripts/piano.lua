-- Player piano: laser + trigger (or the desktop action key) plays a tune.
function on_vr_trigger(hand)
  self:set("tunes", self:get("tunes", 0) + 1)
  toast("The piano plinks out Camptown Races", 2)
end

function on_interact(rider)
  on_vr_trigger("right")
end
