#ifndef SOTEK_TRIANGLE_H
#define SOTEK_TRIANGLE_H

#include "surface.h"

#define WM_TRIANGLE_CHOICE (WM_APP + 20)

int TriangleRegister(HINSTANCE instance);
HWND MessageTriangleShow(HWND owner, const Theme *theme, ErrorLog *log, HINSTANCE instance);
HWND ChoiceTriangleShow(HWND owner, const Theme *theme, HINSTANCE instance, const wchar_t *lineOne, const wchar_t *lineTwo, const wchar_t *const *choices, int count);

#endif
