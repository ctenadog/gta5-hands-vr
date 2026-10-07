# 0.6.4 - pause menu and phone from the controllers

- Left controller menu button = pause menu (Esc). In the pause menu: left stick = navigate, A or right trigger = select,
  B = back, right stick left/right = previous/next tab, menu button = close.
- Left grip = take out the phone. While the phone is out: left stick = navigate, A = select, B or left grip = back
  (on the home screen this puts the phone away). Walking / shooting / jumping are paused while the phone is out.
- Done with simulated keyboard keys (SendInput scancodes: Esc, arrows, Enter, Backspace, Q, E), only while the GTA window
  has focus; every key is logged as "menu: key ...". New inputs "menu" (/input/menu/click) and "phone"
  (/input/squeeze/value) in sheets/inputs.json; shared-memory version 3 (old helper refuses with a clear log line).
- Untested in game: if the Pico menu button is taken by SteamVR, the pause menu still opens with Esc on the keyboard.
