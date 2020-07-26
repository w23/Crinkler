#include "CallTransform.h"
#include "Hunk.h"
#include "CoffObjectLoader.h"
#include "data.h"
#include "HunkList.h"
#include "Symbol.h"
#include "Log.h"
#include <cstring>

Hunk* CallTransform::GetDetransformer() {
	CoffObjectLoader loader;
	HunkList* hl = loader.Load(calltransObj, int(calltransObj_end - calltransObj), "call detransform");
	Hunk* h = hl->ToHunk("call detransformer");
	delete hl;
	return h;
}

bool CallTransform::DoTransform(Hunk* hunk, int splittingPoint, bool verbose) {
	unsigned char* data = (unsigned char*)hunk->GetPtr();
	int size = splittingPoint;

	int num = 0;
	for (int i = 0 ; i < size-4 ; i++) {
		if (data[i] == 0xe8) {
			int *offset = (int *)&data[i+1];
			if (*offset >= -32768 && *offset <= 32767) {
				*offset = (int)(short)(*offset + i+1);
				i += 4;
				num++;
			}
		}
	}
	if (num > 0) {
		*(int *)(hunk->GetPtr() + hunk->FindSymbol("_CallTrans")->value+2) = num;
		if(verbose)
			printf("\nCalls transformed: %d\n", num);
		return true;
	} else {
		int start = hunk->FindSymbol("_CallTrans")->value;
		int size = hunk->FindSymbol("_CallTransSize")->value;
		memset(hunk->GetPtr()+start, 0x90, size);
		if (verbose)
			Log::Warning("", "No calls - call transformation not applied");
		// Do not run call trans next time
		Disable();
		return false;
	}
}
