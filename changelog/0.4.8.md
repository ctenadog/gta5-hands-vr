# 0.4.8 - shots follow the pistol
User (0.4.7): hands and pistol look right, but bullets fly upward.
Cause (suspected): shots used the OpenXR right "aim" pose; on Pico Neo 3 via PICO Connect + SteamVR it is tilted against the grip pose that now drives the hand.
Fix: shot direction = grip -Y (same barrel axis as the wrist rotation), origin = right hand bone (SKEL_R_Hand).
GTA5VR.ini [vr]: aim_source=grip (aim = 0.4.7 behaviour), aim_pitch=0 (degrees, + = up, e.g. -10 if still high).
Log: "aim: shot via grip, pitch shot X | grip X | aim pose X | head X deg" once per second while firing.
Untested in game.
