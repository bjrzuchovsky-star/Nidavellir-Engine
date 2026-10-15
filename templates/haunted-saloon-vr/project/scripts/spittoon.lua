-- Spittoon: counts what gets thrown in (grabbables entering the trigger).
function on_trigger_enter(other)
  local n = self:get("dings", 0) + 1
  self:set("dings", n)
  toast("Ding! " .. other:name() .. " in the spittoon (" .. n .. ")", 2)
end
