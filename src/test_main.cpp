#include <iostream>
#include <string>
#include <windows.h>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: test_main <command>" << std::endl;
        return 1;
    }

    std::string input(argv[1]);
    std::cout << "Input (char*): " << input << std::endl;
    std::cout << "Input length: " << input.length() << std::endl;

    // 测试UTF-8转换
    int len = MultiByteToWideChar(CP_UTF8, 0, input.c_str(), -1, NULL, 0);
    std::cout << "Wide char length: " << len << std::endl;

    std::wstring wcmd(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, input.c_str(), -1, &wcmd[0], len);

    std::wcout.imbue(std::locale(""));
    std::wcout << L"Wide string: " << wcmd << std::endl;

    // 测试匹配
    if (wcmd.find(L"音量增加") != std::wstring::npos) {
        std::cout << "Matched: 音量增加" << std::endl;
    }
    else if (wcmd.find(L"设置音量") != std::wstring::npos) {
        std::cout << "Matched: 设置音量" << std::endl;
    }
    else {
        std::cout << "No match found" << std::endl;
    }

    return 0;
}
