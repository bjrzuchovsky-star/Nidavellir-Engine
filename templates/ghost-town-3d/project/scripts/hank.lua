-- Old Hank: a townsfolk on the Drifter rig. Waves when a rider comes
-- close. anim.play(entity, state_or_clip [, fade [, hold]]),
-- anim.crossfade(entity, state, seconds), anim.set_param(entity, name, v),
-- anim.trigger / anim.param / anim.state / anim.stop.
function on_trigger_enter(other)
  anim.play(self, "wave", 0.2)
  toast("Old Hank tips his hat to " .. other:name(), 2)
end
