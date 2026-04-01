#include <iostream>
#include <vector>
#include <windows.h>
#include "controller/BaseController.h"
#include "controller/VolumeController.h"
#include "controller/MediaController.h"
#include "controller/BluetoothController.h"
#include "controller/SleepController.h"
#include "utils/Logger.h"

#pragma comment(lib, "Ole32.lib")

bool g_DebugMode = true;

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR szCmdLine, int iCmdShow) {
    try {
        std::string cmd(szCmdLine);

        if (cmd.find("--debug") != std::string::npos) {
            g_DebugMode = true;
            FILE* log = fopen("D:\\debug.txt", "w");
            if (log) fclose(log);
        }

        LOG("Raw input: %s\n", cmd.c_str());
        LOG("Input length: %d\n", (int)cmd.length());

        std::vector<BaseController*> controllers = {
            new VolumeController(),
            new BluetoothController(),
            new SleepController(),
            new MediaController()
        };

        for (auto* controller : controllers) {
            if (controller->Match(cmd)) {
                controller->Execute(cmd);
                break;
            }
        }

        for (auto* controller : controllers) {
            delete controller;
        }

        return 0;
    } catch(...) {
        return 1;
    }
}