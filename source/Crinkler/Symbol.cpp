#include "Symbol.h"

#ifdef WIN32
#include <windows.h>
#include <dbghelp.h>
#endif

using namespace std;

Symbol::Symbol(const char* name, int value, unsigned int flags, Hunk* hunk, const char* miscString) :
	name(name), value(value), flags(flags), hunk(hunk), fromLibrary(false), hunk_offset(0)
{
	if(miscString)
		this->miscString = miscString;
}

std::string Symbol::GetUndecoratedName() const {
	string str = name;
	char buff[1024];
	UnDecorateSymbolName(str.c_str(), buff, sizeof(buff), UNDNAME_COMPLETE | UNDNAME_32_BIT_DECODE);
	return string(buff);
}
