# 0.5.3 - comfort in the headset

User request: make the picture in the headset more comfortable.

- Steady on-foot camera (GTA5VR.ini head_bob=0, default): the camera no longer follows every step / sprint lean of
  the neck bone. The 0.5.1 log showed the neck forward offset jumping 0 -> 0.28 m while running = the view rocked.
  Sideways sway is ignored, forward lean limited to -4..+10 cm, height follows slowly (crouch, stairs still work).
  head_bob=1 = 0.5.2 behaviour. In vehicles nothing changed.
- Comfort vignette: image edges darken while walking with the stick or smooth-turning (rises in ~0.25 s, fades in ~0.7 s).
  GTA5VR.ini comfort_vignette=60 (% darkness, 0 = off), vignette_size=55 (% radius where darkening starts, larger = less).
  The minimap is never darkened.
- Not tested in game.
