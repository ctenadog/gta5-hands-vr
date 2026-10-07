## 0.2.9 (2026-10-06) - experimental wrist rotation scout (F12)
- No native rotates a ped bone. New src/hands.cpp: first F12 searches the player skeleton in memory without hard-coded 3889 offsets:
  4 bones (L/R hand, head, pelvis) via GET_PED_BONE_INDEX + GET_WORLD_POSITION_OF_ENTITY_BONE, entity matrix at ped+0x60 (checked
  against GET_ENTITY_COORDS), then pointers from the ped object (3 levels, VirtualQuery-checked reads) are scanned for a 64-byte-stride
  matrix array whose translations match those bones (object or world space). Path logged.
- Second F12: writes hand bone rotation = controller rotation x calibration (taken at switch-on, so bone axes need not be known);
  before each write re-resolves the path and checks the hand translation; any mismatch turns it off. Logs whether the game
  overwrote last frame's write (if yes, a post-animation hook is needed - next step).
- SHV getScriptHandleBaseAddress resolved (optional export). Natives added: GET_PED_BONE_INDEX, GET_WORLD_POSITION_OF_ENTITY_BONE.
- Also in 0.2.8: SteamAppId/SteamGameId hidden from SteamVR (Desktop Game Theatre "Waiting..." fix attempt).
