# 0.4.4 - camera and head

* Camera is ATTACHED to the character (ATTACH_CAM_TO_ENTITY, offset in the character's space) instead of being moved
  to GET_ENTITY_COORDS from the script every frame. The script runs before the game moves the character, so the camera
  lagged one frame behind: shaking while walking, falling behind in cars.
* In a vehicle "straight ahead" turns with the vehicle (was world-locked: a turning car left you looking out of the side).
  Snap turn and F9 still work. On foot it stays world-locked as before.
* Eyes 10 cm in front of the body axis, near clip 5 cm (no own head/body cut into the view).
* Head hidden while VR is on, back when VR is off:
  - hat / glasses / earpiece props (0,1,2) are taken off and put back exactly as they were;
  - head/face/hair bones (within 25 cm of SKEL_Head, not below the neck, not near a hand, found automatically on VR
    start via the same skeleton search as F12) are collapsed to scale 0.001 by the background writer. Translation
    untouched. The game rebuilds them every frame, so when writing stops (VR off, pause menu, loading) the head is back.
  - GTA5VR.ini [vr] hide_head=1 (0 = do not hide).
* Head bone is checked against the game every frame; on mismatch (character switch, model change) hiding stops and
  the skeleton is searched again (max 3 times).
Untested in game.
