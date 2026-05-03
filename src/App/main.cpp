#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include "App.h"

static void InitDebugConsole()
{
    if (!AllocConsole())
        return;

    FILE* stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
    freopen_s(&stream, "CONIN$", "r", stdin);

    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    SetConsoleTitleW(L"Shader Lab Console");

    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
}

int WINAPI WinMain(_In_ HINSTANCE hInst, _In_opt_ HINSTANCE, _In_ LPSTR, _In_ int nCmdShow)
{
    InitDebugConsole();

    App app;
    if (!app.Init(hInst, nCmdShow))
    {
        FreeConsole();
        return -1;
    }

    int rc = app.Run();
    FreeConsole();
    return rc;
}
