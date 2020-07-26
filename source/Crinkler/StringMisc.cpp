#include "StringMisc.h"
#include <algorithm>
#include <cstring>

#ifndef WIN32
#define sprintf_s snprintf
#endif

using namespace std;

string ToUpper(const string& s) {
	string d(s);
	transform(d.begin(), d.end(), d.begin(), (int(*)(int))toupper);
	return d;
}

string ToLower(const string& s) {
	string d(s);
	transform(d.begin(), d.end(), d.begin(), (int(*)(int))tolower);
	return d;
}

std::string StripPath(const std::string& s) {
	int idx = (int)s.size()-1;
	while(idx >= 0 && s[idx] != ':' && s[idx] != '/' && s[idx] != '\\') idx--;
	return s.substr(idx+1, s.size() - (idx+1));
}

string ToHtml(char c) {
	char buff[16];
	if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') 
		|| (c == '_'))
		sprintf_s(buff, sizeof(buff), "%c", c);
	else
		sprintf_s(buff, sizeof(buff), "%%%2X", c);
	return string((char*)buff);
}

std::string EscapeHtml(const std::string& s) {
	string d;
	for(char c : s) {
		d += ToHtml(c);
	}
	return d;
}

bool EndsWith(const char* str, const char* ending) {
	int slen = (int)strlen(str);
	int elen = (int)strlen(ending);
	if(slen < elen)
		return false;
	return memcmp(str+slen-elen, ending, elen) == 0;
}

bool StartsWith(const char* str, const char* start) {
	while(true) {
		if(*start == '\0')
			return true;
		if(*str != *start)
			return false;
		str++;
		start++;
	}
}

std::vector<std::string> IntoLines(const char *data, int length) {
	std::vector<std::string> lines;
	int start = 0;
	for (int i = 0; i <= length; i++) {
		if (i == length || (data[i] < ' ' && data[i] != '\t')) {
			if (i > start) {
				lines.emplace_back(&data[start], i - start);
			}
			start = i + 1;
		}
	}
	return lines;
}
