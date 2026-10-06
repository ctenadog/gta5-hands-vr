# 0.4.2
- Fix: after publishing a frame transport the game compared the helper's answer time with a timestamp taken before publishing (unsigned underflow), so every transport was declared "did not answer" instantly and VR switched off. Now waits up to 20 s from the next frame.
- Fix: transport order is now NT handle -> legacy shared handle -> CPU copy (0.4.1 skipped the legacy handle).
- Fix: after an automatic VR off, every later F8 was cancelled at once (stale failure flag). A new F8 now starts cleanly.
- Helper answers transport offers on its own thread, so a blocked xrWaitFrame no longer delays the answer; logs "game offers frame transport".
