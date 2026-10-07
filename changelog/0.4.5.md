# 0.4.5 - camera in the head, no head artifacts, sharper picture

Report on 0.4.4: picture in the headset, no crashes; camera shakes on foot and in cars, camera not inside the head,
hair/neck/face visible, artifacts from the head, picture blurry and stuttering.

* Camera anchor = the character's neck bone (SKEL_Neck_1, smoothed) + eye_up / eye_forward, in the character's own
  space. 0.4.4 used root + fixed 0.65 m: wrong height when seated, beside/behind the real head.
* In a vehicle the camera rotation is attached too (HARD_ATTACH_CAM_TO_ENTITY, relative to the character):
  0.4.4 set a world rotation from the car heading read one frame early -> shaking in turns.
* Head hiding: hidden bones are now collapsed to 0.0001 AND moved to the neck (except SKEL_Head itself, used for the
  safety check). 0.4.4 collapsed each bone in place -> stretched skin "artifacts". Radius 0.30 m (was 0.25), down to
  7 cm below the head bone (was 4).
* CPU transport image 1800 px high (was 1440): sharper. GTA5VR.ini cpu_size (0 = full headset size, 720..4096).
* New GTA5VR.ini [vr] keys: eye_up=12, eye_forward=12 (cm), head_smooth=25 (%, lower = calmer), car_hard_attach=1, cpu_size=1800.
* Log every 10 s: "camera: ... neck (...), camera offset (...)".
Untested in game.
