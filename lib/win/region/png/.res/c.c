#include <windows.h>  
__declspec(dllexport) HRGN PngDataScan0ToRegion (UINT *pDataScan0,INT imgWidth,INT imgHeight)
{ 
    HRGN hRgn = CreateRectRgn(0, 0, 0, 0);
    for (UINT h = 0; h < imgHeight; ++ h)
    {
        UINT right = 0;
        while(right < imgWidth)
        {
            UINT left = right;
            while (left ++ < imgWidth && !(*pDataScan0 ++ & 0xFF000000) );

            right = left; 
            while (right ++ < imgWidth && (*pDataScan0 ++ & 0xFF000000) ); 

            HRGN hRgnBlock = CreateRectRgn(left, h, right - 1, h + 1);
            CombineRgn(hRgn, hRgn, hRgnBlock, RGN_OR);
			DeleteObject(hRgnBlock); 
        }
    }

    return hRgn;
}
