# 0.6.2 - left hand no longer pulled onto the gun

User report (screenshot, pistol): the left hand snaps onto the weapon (game's two-handed grip IK).
In VR every hand follows its own controller, so the mod now sets CPED_RESET_FLAG_CancelLeftHandGripIk (324) every
frame on foot (SET_PED_RESET_FLAG). In vehicles nothing changes.
