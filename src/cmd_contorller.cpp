#include <iostream>
#include <cstdlib>
#include "controller/VolumeController.h"

#pragma comment(lib, "Ole32.lib")

int WINAPI WinMain(HINSTANCE hInstatnce, HINSTANCE hPrevInstance, PSTR szCmdLine, int iCmdShow) {
    try {
        int volume = std::atoi(szCmdLine);
        ChangeVolume((float)volume/100);
        return 0;
    }
    catch(...){
        std::cout << "δ֪�쳣" << std::endl;
    }
}