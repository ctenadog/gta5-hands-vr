# 0.3.2
- Fix: 0.3.1 could not start VR at all ("shared eye texture failed 0x80070057, format 29"): the driver refuses a shared keyed-mutex texture in sRGB format.
- Shared texture now tries R8G8B8A8_TYPELESS / UNORM (copy-compatible with the eye image) and two bind modes, logging each attempt.
- If sharing is impossible, the mod falls back to own_device=0 automatically instead of switching VR off.
