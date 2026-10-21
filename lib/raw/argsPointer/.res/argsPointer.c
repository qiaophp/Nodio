	#include <stdlib.h>  
	__declspec(dllexport) void getArgsPointer(void *(__cdecl *callback) (int id,void ** ppStruct),int id,void * pStruct)
    {
    	void ** pp = (void **)&pStruct; 
    	callback(id,pp); 
    }
