#include "tjsCommHead.h"
#include "Platform.h"

#include <filesystem>
#include <codecvt>
#include <algorithm>

#include <SDL3/SDL_iostream.h>
#include <sys/utime.h>
#include <fcntl.h>
#include <dirent.h>

#include "TVPSystem.h"
#include "TVPEvent.h"
#include "TVPMsg.h"
#include "TVPStorage.h"
#include "Random.h"
#include "RenderManager.h"
#include "TVPApplication.h"

//---------------------------------------------------------------------------
// tTVPFileMedia
//---------------------------------------------------------------------------
class tTVPFileMedia : public iTVPStorageMedia
{
	tjs_uint RefCount;

public:
	tTVPFileMedia() { RefCount = 1; }
	~tTVPFileMedia() { ; }

	void TJS_INTF_METHOD AddRef() { RefCount++; }
	void TJS_INTF_METHOD Release()
	{
		if (RefCount == 1)
			delete this;
		else
			RefCount--;
	}

	void TJS_INTF_METHOD GetName(ttstr& name) { name = TJS_W("file"); }

	void TJS_INTF_METHOD NormalizeDomainName(ttstr& name);
	void TJS_INTF_METHOD NormalizePathName(ttstr& name);
	bool TJS_INTF_METHOD CheckExistentStorage(const ttstr& name);
	tTJSBinaryStream* TJS_INTF_METHOD Open(const ttstr& name, tjs_uint32 flags);
	void TJS_INTF_METHOD GetListAt(const ttstr& name, iTVPStorageLister* lister);
	void TJS_INTF_METHOD GetLocallyAccessibleName(ttstr& name);

public:
	void TJS_INTF_METHOD GetLocalName(ttstr& name);
};
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPFileMedia::NormalizeDomainName(ttstr& name)
{
	// normalize domain name
	// make all characters small
	tjs_char* p = name.Independ();
	while (*p)
	{
		if (*p >= TJS_W('A') && *p <= TJS_W('Z'))
			*p += TJS_W('a') - TJS_W('A');
		p++;
	}
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPFileMedia::NormalizePathName(ttstr& name)
{
	// normalize path name
	// make all characters small
	tjs_char* p = name.Independ();
	while (*p)
	{
		if (*p >= TJS_W('A') && *p <= TJS_W('Z'))
			*p += TJS_W('a') - TJS_W('A');
		p++;
	}
}
//---------------------------------------------------------------------------
bool TJS_INTF_METHOD tTVPFileMedia::CheckExistentStorage(const ttstr& name)
{
	if (name.IsEmpty()) return false;

	ttstr _name(name);
	GetLocalName(_name);

	return TVPCheckExistentLocalFile(_name);
}
//---------------------------------------------------------------------------
tTJSBinaryStream* TJS_INTF_METHOD tTVPFileMedia::Open(const ttstr& name, tjs_uint32 flags)
{
	// open storage named "name".
	// currently only local/network(by OS) storage systems are supported.
	if (name.IsEmpty())
		TVPThrowExceptionMessage(TVPCannotOpenStorage, TJS_W("\"\""));

	ttstr origname = name;
	ttstr _name(name);
	GetLocalName(_name);

	return new tTVPLocalFileStream(origname, _name, flags);
}

//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPFileMedia::GetListAt(const ttstr& _name, iTVPStorageLister* lister)
{
	ttstr name(_name);
	GetLocalName(name);
	TVPGetLocalFileListAt(name, [lister](const ttstr& name, tTVPLocalFileInfo* s) {
		if (s->Mode & (S_IFREG)) {
			lister->Add(name);
		}
		});
}

static int _utf8_strcasecmp(const char* a, const char* b) {
	for (; *a && *b; ++a, ++b) {
		int ca = *a, cb = *b;
		if ('A' <= ca && ca <= 'Z') ca += 'a' - 'A';
		if ('A' <= cb && cb <= 'Z') cb += 'a' - 'A';
		int ret = ca - cb;
		if (ret) return ret;
	}
	return *a - *b;
}

//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPFileMedia::GetLocallyAccessibleName(ttstr& name)
{
	ttstr newname;

	const tjs_char* ptr = name.c_str();

#ifdef WIN32
	if (TJS_strncmp(ptr, TJS_W("./"), 2))
	{
		// differs from "./",
		// this may be a UNC file name.
		// UNC first two chars must be "\\\\" ?
		// AFAIK 32-bit version of Windows assumes that '/' can be used as a path
		// delimiter. Can UNC "\\\\" be replaced by "//" though ?

		newname = ttstr(TJS_W("\\\\")) + ptr;
	}
	else
	{
		ptr += 2;  // skip "./"
		if (!*ptr) {
			newname = TJS_W("");
		}
		else {
			tjs_char dch = *ptr;
			if (*ptr < TJS_W('a') || *ptr > TJS_W('z')) {
				newname = TJS_W("");
			}
			else {
				ptr++;
				if (*ptr != TJS_W('/')) {
					newname = TJS_W("");
				}
				else {
					newname = ttstr(dch) + TJS_W(":") + ptr;
				}
			}
		}
	}

	// change path delimiter to '\\'
	tjs_char* pp = newname.Independ();
	while (*pp)
	{
		if (*pp == TJS_W('/')) *pp = TJS_W('\\');
		pp++;
	}
#else // posix
	if (!TJS_strncmp(ptr, TJS_W("./"), 2)) {
		ptr += 2;  // skip "./"
		newname.Clear();
	}
#if CC_TARGET_PLATFORM == CC_PLATFORM_IOS
	{
		std::string prefix = "/";
		prefix += tTJSNarrowStringHolder(ptr).Buf;
		static const std::vector<ttstr>& prefixPath = _getPrefixPath();
		static const std::vector<std::string>& homeDir = _getHomeDir();
		for (int i = 0; i < prefixPath.size(); ++i) {
			const std::string& dir = homeDir[i];
			if (prefix.length() < dir.length()) continue;
			std::string actualPrefix = prefix.substr(0, dir.length());
			if (!_utf8_strcasecmp(actualPrefix.c_str(), dir.c_str())) {
				newname = prefixPath[i];
				ptr += prefixPath[i].length();
				while (*ptr && *ptr == TJS_W('/')) ++ptr;
				break;
			}
		}
	}
#endif
	while (*ptr) {
		const tjs_char* ptr_end = ptr;
		while (*ptr_end && *ptr_end != TJS_W('/')) ++ptr_end;
		if (ptr_end == ptr) break;
		const tjs_char* ptr_cur = ptr;
		tTJSNarrowStringHolder walker(ttstr(ptr, ptr_end - ptr).c_str());
		while (*ptr_end && *ptr_end == TJS_W('/')) ++ptr_end;
		ptr = ptr_end;

		DIR* dirp;
		struct dirent* direntp;
		newname += "/";
		if ((dirp = opendir(tTJSNarrowStringHolder(newname.c_str())))) {
			bool found = false;
			while ((direntp = readdir(dirp)) != NULL) {
				if (!_utf8_strcasecmp(walker, direntp->d_name)) {
					newname += direntp->d_name;
					found = true;
					break;
				}
			}
			closedir(dirp);
			if (!found) {
				newname += ptr_cur;
				break;
			}
		}
		else {
			newname += ptr_cur;
			break;
		}
	}

#endif
	name = newname;
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPFileMedia::GetLocalName(ttstr& name)
{
	ttstr tmp = name;
	GetLocallyAccessibleName(tmp);
	if (tmp.IsEmpty()) TVPThrowExceptionMessage(TVPCannotGetLocalName, name);
	name = tmp;
}
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
iTVPStorageMedia* TVPCreateFileMedia()
{
	return new tTVPFileMedia;
}
//---------------------------------------------------------------------------

#include <windows.h>
#include <Psapi.h>

tjs_int TVPGetSystemFreeMemory()
{
	MEMORYSTATUS info;
	GlobalMemoryStatus(&info);
	return info.dwAvailPhys / (1024 * 1024);
}

tjs_int TVPGetSelfUsedMemory()
{
	PROCESS_MEMORY_COUNTERS info;
	GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof(info));
	return info.WorkingSetSize / (1024 * 1024);
}

void TVPGetMemoryInfo(TVPMemoryInfo& m)
{
	MEMORYSTATUS status;
	status.dwLength = sizeof(status);
	GlobalMemoryStatus(&status);

	m.MemTotal = status.dwTotalPhys / 1024;
	m.MemFree = status.dwAvailPhys / 1024;
	m.SwapTotal = status.dwTotalPageFile / 1024;
	m.SwapFree = status.dwAvailPageFile / 1024;
	m.VirtualTotal = status.dwTotalVirtual / 1024;
	m.VirtualUsed = (status.dwTotalVirtual - status.dwAvailVirtual) / 1024;
}

// int gettimeofday(struct timeval * val, struct timezone *)
// {
// 	if (val)
// 	{
// 		LARGE_INTEGER liTime, liFreq;
// 		QueryPerformanceFrequency(&liFreq);
// 		QueryPerformanceCounter(&liTime);
// 		val->tv_sec = (long)(liTime.QuadPart / liFreq.QuadPart);
// 		val->tv_usec = (long)(liTime.QuadPart * 1000000.0 / liFreq.QuadPart - val->tv_sec * 1000000.0);
// 	}
// 	return 0;
// }

void* dlopen(const char* filename, int flag) {
	return (void*)LoadLibraryA(filename);
}

void* dlsym(void* handle, const char* funcname) {
	return (void*)GetProcAddress((HMODULE)handle, funcname);
}

extern "C" int usleep(unsigned long us) {
	Sleep(us / 1000);
	return 0;
}

//extern "C" __declspec(dllimport) int __cdecl __wgetmainargs(int * _Argc, wchar_t *** _Argv, wchar_t *** _Env, int _DoWildCard, void * _StartInfo);
std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
std::string TVPGetDefaultFileDir() {
	wchar_t buf[MAX_PATH];
	_wgetcwd(buf, sizeof(buf) / sizeof(buf[0]));
	wchar_t* p = buf;
	while (*p) {
		if (*p == '\\') *p = '/';
		++p;
	}
	return converter.to_bytes(buf);
}

void TVPCheckAndSendDumps(const std::string& dumpdir, const std::string& packageName, const std::string& versionStr);
bool TVPCheckStartupArg() {
    return false;
}

std::vector<std::string> TVPGetDriverPath() {
	std::vector<std::string> ret;
	char drv[4] = { 'C', ':', '/', 0 };
	for (char c = 'C'; c <= 'Z'; ++c) {
		drv[0] = c;
		switch (GetDriveTypeA(drv)) {
		case DRIVE_REMOVABLE:
		case DRIVE_FIXED:
		case DRIVE_REMOTE:
			ret.emplace_back(drv);
			break;
		}
	}
	return ret;
}

std::vector<std::string> TVPGetAppStoragePath() {
	std::vector<std::string> ret;
	ret.emplace_back(TVPGetDefaultFileDir());
	return ret;
}

bool TVPCheckStartupPath(const std::string& path) { return true; }

std::string TVPGetPackageVersionString() {
	return "win32";
}

void TVPControlAdDialog(int adType, int arg1, int arg2) {}
void TVPForceSwapBuffer() {}


//---------------------------------------------------------------------------
// TVPCreateFolders
//---------------------------------------------------------------------------
static bool _TVPCreateFolders(const ttstr& folder)
{
    // create directories along with "folder"
    if(folder.IsEmpty())
        return true;

    if(TVPCheckExistentLocalFolder(folder))
        return true; // already created

    const tjs_char *p = folder.c_str();
    tjs_int i = folder.GetLen() - 1;

    if(p[i] == TJS_W(':'))
        return true;

    while(i >= 0 && (p[i] == TJS_W('/') || p[i] == TJS_W('\\')))
        i--;

    if(i >= 0 && p[i] == TJS_W(':'))
        return true;

    for(; i >= 0; i--) {
        if(p[i] == TJS_W(':') || p[i] == TJS_W('/') || p[i] == TJS_W('\\'))
            break;
    }

    ttstr parent(p, i + 1);
    if(!TVPCreateFolders(parent))
        return false;

    return !std::filesystem::create_directory(folder.AsStdString().c_str());
}
//---------------------------------------------------------------------------

bool TVPCreateFolders(const ttstr& folder)
{
	if (folder.IsEmpty()) return true;

	const tjs_char* p = folder.c_str();
	tjs_int i = folder.GetLen() - 1;

	if (p[i] == TJS_W(':')) return true;

	if (p[i] == TJS_W('/') || p[i] == TJS_W('\\')) i--;

	return _TVPCreateFolders(ttstr(p, i + 1));
}

std::string TVPGetCurrentLanguage() {
	LANGID lid = GetUserDefaultUILanguage();
	const LCID locale_id = MAKELCID(lid, SORT_DEFAULT);
	char code[10] = { 0 };
	char country[10] = { 0 };
	GetLocaleInfoA(locale_id, LOCALE_SISO639LANGNAME, code, sizeof(code));
	GetLocaleInfoA(locale_id, LOCALE_SISO3166CTRYNAME, country, sizeof(country));
	std::string ret = code;
	if (country[0]) {
		for (int i = 0; i < sizeof(country) && country[i]; ++i) {
			char c = country[i];
			if (c <= 'Z' && c >= 'A') {
				country[i] += 'a' - 'A';
			}
		}
		ret += "_";
		ret += country;
	}
	return ret;
}

void TVPReleaseFontLibrary();
void TVPExitApplication(int code) {
	// clear some static data for memory leak detect
	TVPDeliverCompactEvent(TVP_COMPACT_LEVEL_MAX);
	if (!TVPIsSoftwareRenderManager())
		iTVPTexture2D::RecycleProcess();

	exit(code);
}

bool TVPDeleteFile(const std::string& filename)
{
    return unlink(filename.c_str()) == 0;
}

bool TVPRenameFile(const std::string& from, const std::string& to)
{
	tjs_int ret = rename(from.c_str(), to.c_str());
	return !ret;
}

void CopyFileAttributes(const wchar_t* src, const wchar_t* dst)
{
    DWORD attr = GetFileAttributesW(src);
    if (attr != INVALID_FILE_ATTRIBUTES)
    {
        SetFileAttributesW(dst, attr);
    }

    HANDLE hSrc = CreateFileW(src, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    HANDLE hDst = CreateFileW(dst, GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);

    if (hSrc != INVALID_HANDLE_VALUE && hDst != INVALID_HANDLE_VALUE)
    {
        FILETIME createTime, accessTime, writeTime;
        if (GetFileTime(hSrc, &createTime, &accessTime, &writeTime))
        {
            SetFileTime(hDst, &createTime, &accessTime, &writeTime);
        }
        CloseHandle(hSrc);
        CloseHandle(hDst);
    }
}
void CopyFolderAttributes(const wchar_t* src, const wchar_t* dst)
{
    DWORD attr = GetFileAttributesW(src);
    if (attr != INVALID_FILE_ATTRIBUTES)
    {
        SetFileAttributesW(dst, attr);
    }
}
bool TVPCopyFolder(const std::string& from, const std::string& to)
{
    if (!TVPCheckExistentLocalFolder(to) && !TVPCreateFolders(to))
    {
        return false;
    }

    bool success = true;
    TVPListDir(from,
               [&](const std::string& _name, int mask)
               {
                   if (_name == "." || _name == "..")
                       return;
                   if (!success)
                       return;
                   if (mask & S_IFREG)
                   {
                       success = TVPCopyFile(from + "/" + _name, to + "/" + _name);
                   }
                   else if (mask & S_IFDIR)
                   {
                       success = TVPCopyFolder(from + "/" + _name, to + "/" + _name);
                   }
               });
    return success;
}
bool TVPCopyFile(const std::string& from, const std::string& to)
{
    FILE* fFrom = fopen(from.c_str(), "rb");
    if (!fFrom)
    {
        return TVPCopyFolder(from, to);
    }
    FILE* fTo = fopen(to.c_str(), "wb");
    if (!fTo)
    {
        fclose(fFrom);
        return false;
    }
    const int bufSize = 1 * 1024 * 1024;
    std::vector<char> buffer;
    buffer.resize(bufSize);
    size_t index = 0;
    while ((index = fread(&buffer.front(), 1, bufSize, fFrom)))
    {
        fwrite(&buffer.front(), 1, index, fTo);
    }
    fclose(fFrom);
    fclose(fTo);
    return true;
}

void TVPProcessInputEvents() {}
void TVPShowIME(int x, int y, int w, int h) {}
void TVPHideIME() {}

void TVPRelinquishCPU() { Sleep(0); }

tjs_uint32 TVPGetRoughTickCount32()
{
    tjs_uint32 uptime = 0;
    struct timespec on;
    if(clock_gettime(CLOCK_MONOTONIC, &on) == 0)
        uptime = on.tv_sec * 1000 + on.tv_nsec / 1000000;
    return uptime;
}

void TVPPrintLog(const char* str) {
	printf("%s", str);
}

bool TVP_stat(const tjs_char* name, tTVP_stat& s) {
	tTJSNarrowStringHolder holder(name);
	return TVP_stat(holder, s);
}

bool TVP_stat(const char* name, tTVP_stat& s) {
    struct stat t;
    bool ret = !stat(name, &t);
    s.tvp_mode = t.st_mode;
    s.tvp_size = t.st_size;
    s.tvp_atime = t.st_atime;
    s.tvp_mtime = t.st_mtime;
    s.tvp_ctime = t.st_ctime;
    return ret;
}

bool TVP_utime(const char* name, time_t modtime) {
        _utimbuf utb;
        utb.modtime = modtime;
        utb.actime = modtime;
        ttstr filename(name);
        return _wutime((const wchar_t*)filename.c_str(), &utb) == 0;
}

void TVPSendToOtherApp(const std::string& filename) {

}

void TVPListDir(const std::string& folder, std::function<void(const std::string&, int)> cb) {
    DIR* dirp;
    struct dirent* direntp;
    tTVP_stat stat_buf;
    if ((dirp = opendir(folder.c_str())))
    {
        while ((direntp = readdir(dirp)) != NULL)
        {
            std::string fullpath = folder + "/" + direntp->d_name;
            if (!TVP_stat(fullpath.c_str(), stat_buf))
                continue;
            cb(direntp->d_name, stat_buf.tvp_mode);
        }
        closedir(dirp);
    }
}
void TVPGetLocalFileListAt(const ttstr& name, const std::function<void(const ttstr&, tTVPLocalFileInfo*)>& cb) {
    DIR* dirp;
    struct dirent* direntp;
    tTVP_stat stat_buf;
    std::string folder(name.AsNarrowStdString());
    if ((dirp = opendir(folder.c_str())))
    {
        while ((direntp = readdir(dirp)) != NULL)
        {
            std::string fullpath = folder + "/" + direntp->d_name;
            if (!TVP_stat(fullpath.c_str(), stat_buf))
                continue;
            ttstr file(direntp->d_name);
            if (file.length() <= 2) {
                if (file == TJS_W(".") || file == TJS_W(".."))
                    continue;
            }
            tjs_char* p = file.Independ();
            while (*p)
            {
                // make all characters small
                if (*p >= TJS_W('A') && *p <= TJS_W('Z'))
                    *p += TJS_W('a') - TJS_W('A');
                p++;
            }
            tTVPLocalFileInfo info;
            info.NativeName = direntp->d_name;
            info.Mode = stat_buf.tvp_mode;
            info.Size = stat_buf.tvp_size;
            info.AccessTime = stat_buf.tvp_atime;
            info.ModifyTime = stat_buf.tvp_mtime;
            info.CreationTime = stat_buf.tvp_ctime;
            cb(file, &info);
        }
        closedir(dirp);
    }
}

tTVPMemoryStream* GetResourceStream(const ttstr& filename)
{
    tTJSBinaryStream* tmp =
        TVPCreateBinaryStreamForRead(ExePath() + ttstr("/") + filename, 0);
    tTVPMemoryStream* ret = new tTVPMemoryStream(nullptr, tmp->GetSize());
    tmp->ReadBuffer(ret->GetInternalBuffer(), tmp->GetSize());
    delete tmp;
    return ret;
}

void TVPPreNormalizeStorageName(ttstr& name)
{
	// if the name is an OS's native expression, change it according with the
	// TVP storage system naming rule.
	tjs_int namelen = name.GetLen();
	if (namelen == 0) return;
	if (namelen >= 2)
	{
		if ((name[0] >= TJS_W('a') && name[0] <= TJS_W('z') ||
			name[0] >= TJS_W('A') && name[0] <= TJS_W('Z')) &&
			name[1] == TJS_W(':'))
		{
			// Windows drive:path expression
			ttstr newname(TJS_W("file://./"));
			newname += name[0];
			newname += (name.c_str() + 2);
			name = newname;
			return;
		}
	}

	if (namelen >= 3)
	{
		if (name[0] == TJS_W('\\') && name[1] == TJS_W('\\') ||
			name[0] == TJS_W('/') && name[1] == TJS_W('/'))
		{
			// unc expression
			name = ttstr(TJS_W("file:")) + name;
			return;
		}
	}
}
