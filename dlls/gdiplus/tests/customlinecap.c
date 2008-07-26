/*
 * Unit test suite for customlinecap
 *
 * Copyright (C) 2008 Nikolay Sivov
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "windows.h"
#include "gdiplus.h"
#include "wine/test.h"

#define expect(expected, got) ok(got == expected, "Expected %.8x, got %.8x\n", expected, got)

static void test_constructor_destructor(void)
{
    GpCustomLineCap *custom;
    GpPath *path, *path2;
    GpStatus stat;

    stat = GdipCreatePath(FillModeAlternate, &path);
    expect(Ok, stat);
    stat = GdipAddPathRectangle(path, 5.0, 5.0, 10.0, 10.0);
    expect(Ok, stat);

    stat = GdipCreatePath(FillModeAlternate, &path2);
    expect(Ok, stat);
    stat = GdipAddPathRectangle(path2, 5.0, 5.0, 10.0, 10.0);
    expect(Ok, stat);

    /* NULL args */
    stat = GdipCreateCustomLineCap(NULL, NULL, LineCapFlat, 0.0, NULL);
    expect(InvalidParameter, stat);
    stat = GdipCreateCustomLineCap(path, NULL, LineCapFlat, 0.0, NULL);
    expect(InvalidParameter, stat);
    stat = GdipCreateCustomLineCap(NULL, path, LineCapFlat, 0.0, NULL);
    expect(InvalidParameter, stat);
    stat = GdipCreateCustomLineCap(NULL, NULL, LineCapFlat, 0.0, &custom);
    expect(InvalidParameter, stat);
    stat = GdipDeleteCustomLineCap(NULL);
    expect(InvalidParameter, stat);

    /* valid args */
    stat = GdipCreateCustomLineCap(NULL, path2, LineCapFlat, 0.0, &custom);
    expect(Ok, stat);
    stat = GdipDeleteCustomLineCap(custom);
    expect(Ok, stat);
    /* it's strange but native returns NotImplemented on stroke == NULL */
    stat = GdipCreateCustomLineCap(path, NULL, LineCapFlat, 10.0, &custom);
    todo_wine expect(NotImplemented, stat);

    GdipDeletePath(path2);
    GdipDeletePath(path);
}

START_TEST(customlinecap)
{
    struct GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;

    gdiplusStartupInput.GdiplusVersion              = 1;
    gdiplusStartupInput.DebugEventCallback          = NULL;
    gdiplusStartupInput.SuppressBackgroundThread    = 0;
    gdiplusStartupInput.SuppressExternalCodecs      = 0;

    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    test_constructor_destructor();

    GdiplusShutdown(gdiplusToken);
}
