# 0.4.7 - wrist direction fix

Tester log 0.4.6 (2026-10-07 03:38): hand rotation applied ~60/s, but "angle to controller" 150-175 deg and the
screenshot shows the pistol pointing down at the player, left palm up.

Cause: OpenXR grip pose axes. With a pistol grip (-Z up along the fist, +X right) +Y = Z x X points BACKWARD,
so wrist -> middle knuckle is grip -Y. 0.4.6 mapped it to +Y -> each hand rotated 180 deg about the knuckle row.

Fix: hand forward = grip -Y. New GTA5VR.ini [vr] keys (auto-written):
- hand_flip=1 (0 = old 0.4.6 direction)
- hand_yaw=0, hand_roll=0 (degrees, extra offsets next to hand_pitch)

Controls confirmed working by the log: B enter/leave vehicle, Y next weapon, A jump, X reload, triggers, snap turn, sprint.
Untested in game.
