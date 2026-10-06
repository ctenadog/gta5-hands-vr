# 0.4.1
- F8 no longer fires twice for one press: key edge detection + 600 ms debounce; a second F8 within 3 s of turning VR on is ignored and logged ("F8 ignored"). 0.4.0 log showed "F8: VR on" then "F8: VR off" 7 s later, which cancelled the helper while it was connecting.
- GTA5VR.log prints "waiting for the helper to connect to SteamVR" every 5 s while connecting.
- Helper waits up to 60 s for the headset (was 30 s), logs progress every 5 s, logs "headset found", and no longer reports a false FATAL "headset not connected" when VR is switched off or the game closes while connecting.
