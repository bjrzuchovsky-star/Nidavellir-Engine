-- Bounty board: pays out for every pirate ship knocked out of the sky
-- (Airship Works' `ships` table).
props = {
  bounty = 50,
}

local pirates = 0
local purse = 0

function on_start()
  pirates = #ships.list("pirates")
  toast("Sky pirates sighted: " .. pirates .. ". $" .. props.bounty .. " a hull.", 4)
end

function on_tick(dt)
  local now = #ships.list("pirates")
  if now < pirates then
    purse = purse + (pirates - now) * props.bounty
    local lost, splits = ships.stats()
    toast("Bounty paid! Purse $" .. purse .. " (" .. lost .. " parts shot off, " .. splits .. " wrecks)", 3)
    log("bounty purse now " .. purse)
  end
  pirates = now
end
