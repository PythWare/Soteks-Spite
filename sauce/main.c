#include "gui.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR commandLine, int show)
{
    (void)previous;
    (void)commandLine;
    return RunGui(instance, show);
}
