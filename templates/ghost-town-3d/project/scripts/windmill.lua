-- Windmill: turns its sails on the 60 Hz tick. 3D entities have the
-- usual handle methods (name, get / set, show / hide, toast...) plus
-- pos3 / set_pos3 / move3 / yaw / set_yaw / spin(deg, axis) / scale3 /
-- set_scale3.
props = {
  speed = 45,  -- degrees per second
}

function on_start()
  log(self:name() .. " catches the wind")
end

function on_tick(dt)
  self:spin(props.speed * dt, "z")
end
