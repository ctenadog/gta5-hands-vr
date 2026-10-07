# 0.6.1 - wrist skin no longer stretched

User report (Trevor, screenshot): hand rotation itself is right, but one side of the wrist / hand mesh is pulled down.
Cause: only the hand + 15 finger bones were turned. The forearm roll (twist) bone RB_*_ForeArmRoll kept the animation
pose, so wrist skin weighted to it stretched; helper bones of the hand that are not in the fixed finger list (they differ
per model) stayed behind too.

- Forearm roll bone gets half of the hand's twist about the forearm axis (how the game rig drives it).
- Every other bone of the current model that lies in the hand (past the wrist, within 16 cm) now turns with the hand.
  Recomputed per character (Michael / Franklin / Trevor).
- Log: "hands: extra hand bones L n R n, forearm roll bone ..." and "wrist twist L .. R .. deg" in the rotation line.
