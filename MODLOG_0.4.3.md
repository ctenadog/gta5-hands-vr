# 0.4.3
- Fix: headset froze on one frame with CPU transport (3). The 2-stage readback queue could end in a state where
  neither stage was read nor written (log: "handed to helper 0 fps, busy 0"). Now any full stage is read back,
  a free stage is always reused, and a 1 s watchdog resets the queue ("CPU transport stalled ... restarting").
- Hand writer thread (F12) lowered to below-normal priority and yields regularly; it ran ~900k writes/s at
  above-normal priority and the freeze began right after F12.
