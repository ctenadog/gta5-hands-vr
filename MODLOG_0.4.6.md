# 0.4.6 - hands and controls

Hands
- Hands sit farther from the body: GTA5VR.ini arm_scale=120 (% of the real controller distance) and arm_forward=12 (cm forward along the view); reach limit 0.95 m (was 0.7).
- Wrist rotation rewritten: absolute (no calibration), turns on automatically with VR (hand_rotation=1, F12 = off/on).
  The hand frame is measured from the skeleton (wrist -> middle knuckle, pinky -> index knuckle) and matched to the
  controller grip pose (+Y / -Z); the hand AND its 15 finger bones + weapon grip point rotate together around the wrist.
  0.3.0-0.4.5 rotated only the hand matrix with an F12 calibration -> fingers left behind, arms/sleeves stretched.
- hand_pitch (degrees, default 0) tilts the hands if they still point up/down.
- Log: "hands: rotation applied N times in 5 s ... angle to controller L x R y deg", finger bone indices.

Controls
- B: get in / out of a vehicle as a task (TASK_ENTER_VEHICLE nearest car within 7 m / TASK_LEAVE_VEHICLE).
  0.4.5 pushed INPUT_ENTER via SET_CONTROL_VALUE_NEXT_FRAME, which the game ignored -> could not leave the car.
- Y: next weapon you own (cycles), right stick click: holster (fists).
- Log: every button/trigger change "input: <action> DOWN/up" (first 300) - shows which Pico Neo 3 buttons reach the mod.

Untested in game.
