#pragma once
#include <d3d11.h>
namespace blit {
bool init(ID3D11Device* dev);
// testPattern: draw a colour test image instead of the game (checks that the headset receives frames)
// GTA backbuffer -> VR eye texture (any format), centre-cropped to eyeAspect (width/height), optional sRGB->linear
bool draw(ID3D11DeviceContext* ctx, ID3D11Texture2D* bb, ID3D11Texture2D* eyeTex, DXGI_FORMAT rtvFormat, bool linearize, float eyeAspect, bool testPattern, bool copySource = true);
void reset();
}
