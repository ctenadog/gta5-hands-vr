#pragma once
// 0.6.3 auto-update: checks GitHub once per game start, downloads a newer release in the background and installs it
// right after the game exits (the running .asi can't be replaced while GTA5.exe has it loaded).
namespace update {
void start();                 // background thread, call once at boot
// 1 = a newer version was downloaded and will be installed when the game closes (ver = that version)
int takeEvent(char* ver, int n);
}
