/*-----------------------------------------------------------------------------
	Paper Plane xUI Everything Search Module (PPXETS_M, migemo edition)

	Based on PPXETS (message communication version) by TORO.
	This is an updated / extended version. 64bit only.

	- Target : Everything 1.5 or later (WM_COPYDATA IPC)
	- Option : migemo search (C/Migemo 1.6.1 or later, 64bit migemo.dll)
	- Build  : see MAKEFILE (nmake, MSVC x64)

	NOTE: This source file is ASCII only, so cp932 and UTF-8 are identical.
-----------------------------------------------------------------------------*/
#define STRICT
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stddef.h>
#include <string.h>
#include <wchar.h>
#include "torowin.h"
#include "ppcommon.h"
#include "everything_ipc.h"

#ifndef HWND_MESSAGE
#define HWND_MESSAGE ((HWND)-3)
#endif

/*----------------------------------------------------------------------------
	constants
----------------------------------------------------------------------------*/
#define REPLY_COPYDATA_ID	0x123456	/* dwData of Everything's reply */
#define REPLY_CLASSNAME		L"PPXETS_M_IPC"

#define DEF_TIMEOUT_PART	2000	/* ms, simple search (maxresults <= 100) */
#define DEF_TIMEOUT_FULL	10000	/* ms, detailed search */
#define DEF_RETRY			10000	/* ms, negative cache after a failure */
#define MIN_TIMEOUT			100		/* ms, lower clamp */
#define FIND_TIMEOUT		500		/* ms, version query to find the window */
#define CFG_REFRESH			3000	/* ms, interval of re-reading settings */
#define MIGEMO_RETRY		30000	/* ms, retry interval after a load failure */
#define MSGRESET_MIN		10000	/* ms, lower limit of the idle time that re-arms the notice */

#define DEF_MIGEMOMIN		2
#define MIGEMO_MAX_LEN		64
#define MIGEMO_MAX_REGEX	4000	/* WCHARs, over this -> normal search */

/* error kinds (a notice is shown only when the kind changes) */
enum {
	ERR_NONE = 0,
	ERR_NOEVERYTHING,
	ERR_NORESPONSE,
	ERR_REJECTED,
	ERR_ACCESS,
	ERR_INTERNAL
};

/*----------------------------------------------------------------------------
	migemo function types (x64 has a single calling convention)
----------------------------------------------------------------------------*/
typedef void * (WINAPI *MIGEMO_OPEN)(const char *dict);
typedef void (WINAPI *MIGEMO_CLOSE)(void *mo);
typedef unsigned char * (WINAPI *MIGEMO_QUERY)(void *mo, const unsigned char *word);
typedef void (WINAPI *MIGEMO_RELEASE)(void *mo, unsigned char *p);
typedef int (WINAPI *MIGEMO_IS_ENABLE)(void *mo);
typedef const char * (WINAPI *MIGEMO_VERSION)(void);
typedef void (WINAPI *MIGEMO_SET_ESCAPE)(void *mo, const unsigned char *chars);

/*----------------------------------------------------------------------------
	types / globals
----------------------------------------------------------------------------*/
typedef struct {
	DWORD full, part;			/* ETP_FULL / ETP_PART */
	DWORD tmoFull, tmoPart;		/* ms, ETS_TIMEOUT_FULL / ETS_TIMEOUT_PART */
	DWORD retry;				/* ms, 0 = no negative cache */
	int migemo;					/* 0 = off, 1 = Where is, 2 = one line editor, 3 = both */
	DWORD migemoMin;			/* ETS_MIGEMOMIN: minimum length of a keyword for migemo */
	WCHAR dict[VFPS];			/* ETS_MIGEMODICT (empty = default) */
	WCHAR dll[VFPS];			/* ETS_MIGEMODLL  (empty = default) */
} ETSCONFIG;

typedef struct {
	PPXAPPINFOW *ppxa;
	DWORD replyID;
	volatile LONG done;
} SEARCHCTX;

HINSTANCE hDllInstance;

/* csModule protects: Cfg, CfgLoaded, CfgTick, ClassRegistered,
   hEverything, RetryUntil, LastErr */
static CRITICAL_SECTION csModule;
static ETSCONFIG Cfg;
static BOOL CfgLoaded = FALSE;
static ULONGLONG CfgTick = 0;
static BOOL ClassRegistered = FALSE;
static HWND hEverything = NULL;			/* cached Everything window */
static ULONGLONG RetryUntil = 0;		/* negative cache expire (tick) */
static ULONGLONG LastSearchTick = 0;	/* tick of the previous search */
static int LastErr = ERR_NONE;			/* last notified error kind */

/* csMigemo protects all migemo state below */
static CRITICAL_SECTION csMigemo;
static HMODULE hMigemoDll = NULL;
static void *MigemoObj = NULL;
static UINT MigemoCP = CP_UTF8;			/* charset of the dictionary */
static MIGEMO_CLOSE pMigemoClose;
static MIGEMO_QUERY pMigemoQuery;
static MIGEMO_RELEASE pMigemoRelease;
static BOOL MigemoFailed = FALSE;		/* last load attempt failed */
static ULONGLONG MigemoFailTick = 0;
static BOOL MigemoWarned = FALSE;		/* failure already notified */
static WCHAR MigemoKeyDict[VFPS];		/* settings used for the load */
static WCHAR MigemoKeyDll[VFPS];

/*----------------------------------------------------------------------------
	DLL entry
----------------------------------------------------------------------------*/
BOOL DLLEntry(HINSTANCE hInst, DWORD reason, LPVOID reserved)
{
	UnUsedParam(reserved);
	if ( reason == DLL_PROCESS_ATTACH ){
		hDllInstance = hInst;
		InitializeCriticalSection(&csModule);
		InitializeCriticalSection(&csMigemo);
	}else if ( reason == DLL_PROCESS_DETACH ){
		DeleteCriticalSection(&csMigemo);
		DeleteCriticalSection(&csModule);
	}
	return TRUE;
}

/*----------------------------------------------------------------------------
	notice (PPXCMDID_REPORTSEARCH, same as the original PPXETS)
----------------------------------------------------------------------------*/
static void PostNotice(PPXAPPINFOW *ppxa, const WCHAR *msg)
{
	ppxa->Function(ppxa, PPXCMDID_REPORTSEARCH, (void *)msg);
}

/* Show a notice when the error kind changes. ERR_NONE re-arms the notice.
   Do not call with a lock held. */
static void Notify(PPXAPPINFOW *ppxa, int kind)
{
	const WCHAR *msg = NULL;

	EnterCriticalSection(&csModule);
	if ( kind != LastErr ){
		LastErr = kind;
		switch ( kind ){
			case ERR_NOEVERYTHING:
				msg = L"PPXETS_M: Everything is not running.";
				break;
			case ERR_NORESPONSE:
				msg = L"PPXETS_M: Everything is not responding.";
				break;
			case ERR_REJECTED:
				msg = L"PPXETS_M: Everything rejected the query.";
				break;
			case ERR_ACCESS:
				msg = L"PPXETS_M: Access to Everything was denied (privilege level differs?).";
				break;
			case ERR_INTERNAL:
				msg = L"PPXETS_M: internal error.";
				break;
			default:
				break;
		}
	}
	LeaveCriticalSection(&csModule);
	if ( msg != NULL ) PostNotice(ppxa, msg);
}

/*----------------------------------------------------------------------------
	settings (_User:NAME)
----------------------------------------------------------------------------*/
static BOOL GetCustString(PPXAPPINFOW *ppxa, const WCHAR *name, WCHAR *dest, size_t destlen)
{
	WCHAR buf[CMDLINESIZE];
	WCHAR *p, *q;
	size_t len;

	dest[0] = L'\0';
	wsprintfW(buf, L"%%*getcust(_User:%s)", name);
	ppxa->Function(ppxa, PPXCMDID_EXTRACT, buf);
	buf[CMDLINESIZE - 1] = L'\0';

	p = buf;
	while ( (*p == L' ') || (*p == L'\t') ) p++;
	q = p + wcslen(p);
	while ( (q > p) && ((q[-1] == L' ') || (q[-1] == L'\t') ||
			(q[-1] == L'\r') || (q[-1] == L'\n')) ) q--;
	*q = L'\0';
	len = (size_t)(q - p);
	if ( (len == 0) || (len >= destlen) ) return FALSE;
	memcpy(dest, p, (len + 1) * sizeof(WCHAR));
	return TRUE;
}

/* returns FALSE when the item is not defined (def is stored) */
static BOOL GetCustNumber(PPXAPPINFOW *ppxa, const WCHAR *name, DWORD def, DWORD *result)
{
	WCHAR buf[64];
	const WCHAR *p;
	DWORD n = 0;

	*result = def;
	if ( GetCustString(ppxa, name, buf, 64) == FALSE ) return FALSE;
	p = buf;
	if ( (*p < L'0') || (*p > L'9') ) return FALSE;
	for ( ; (*p >= L'0') && (*p <= L'9') ; p++ ){
		if ( n > 0x0ccccccc ) break;	/* overflow guard */
		n = n * 10 + (DWORD)(*p - L'0');
	}
	*result = n;
	return TRUE;
}

static void ReadConfig(PPXAPPINFOW *ppxa, ETSCONFIG *c)
{
	DWORD n;

	GetCustNumber(ppxa, L"ETP_FULL", 1, &c->full);
	GetCustNumber(ppxa, L"ETP_PART", 1, &c->part);

	GetCustNumber(ppxa, L"ETS_TIMEOUT_PART", DEF_TIMEOUT_PART, &c->tmoPart);
	if ( c->tmoPart < MIN_TIMEOUT ) c->tmoPart = MIN_TIMEOUT;
	GetCustNumber(ppxa, L"ETS_TIMEOUT_FULL", DEF_TIMEOUT_FULL, &c->tmoFull);
	if ( c->tmoFull < MIN_TIMEOUT ) c->tmoFull = MIN_TIMEOUT;

	GetCustNumber(ppxa, L"ETS_RETRY", DEF_RETRY, &c->retry);

	GetCustNumber(ppxa, L"ETS_MIGEMO", 0, &n);
	c->migemo = ((n >= 1) && (n <= 3)) ? (int)n : 0;	/* other values: disabled */

	GetCustNumber(ppxa, L"ETS_MIGEMOMIN", DEF_MIGEMOMIN, &c->migemoMin);
	if ( c->migemoMin < 1 ) c->migemoMin = 1;
	if ( c->migemoMin > MIGEMO_MAX_LEN ) c->migemoMin = MIGEMO_MAX_LEN;

	GetCustString(ppxa, L"ETS_MIGEMODICT", c->dict, VFPS);
	GetCustString(ppxa, L"ETS_MIGEMODLL", c->dll, VFPS);
}

/* Copy the current settings to 'out'. Re-read them every CFG_REFRESH ms.
   Do not call with a lock held. */
static void GetConfig(PPXAPPINFOW *ppxa, ETSCONFIG *out)
{
	BOOL reload;
	ETSCONFIG *nc;
	ULONGLONG now = GetTickCount64();

	EnterCriticalSection(&csModule);
	reload = !CfgLoaded || ((now - CfgTick) >= CFG_REFRESH);
	if ( !reload ){
		*out = Cfg;
		LeaveCriticalSection(&csModule);
		return;
	}
	if ( CfgLoaded ) CfgTick = now;		/* others keep using the old one */
	LeaveCriticalSection(&csModule);

	nc = (ETSCONFIG *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ETSCONFIG));
	if ( nc == NULL ){
		EnterCriticalSection(&csModule);
		*out = Cfg;
		LeaveCriticalSection(&csModule);
		return;
	}
	ReadConfig(ppxa, nc);
	EnterCriticalSection(&csModule);
	Cfg = *nc;
	CfgLoaded = TRUE;
	CfgTick = GetTickCount64();
	*out = Cfg;
	LeaveCriticalSection(&csModule);
	HeapFree(GetProcessHeap(), 0, nc);
}

/*----------------------------------------------------------------------------
	Everything window detection
----------------------------------------------------------------------------*/
typedef struct {
	HWND exact;		/* class name exactly matches (default instance) */
	HWND other;		/* class name has an instance suffix */
	BOOL denied;	/* a candidate existed but access was denied */
} FINDCTX;

/* Check one window. Returns TRUE to continue the enumeration. */
static BOOL CheckCandidate(HWND hwnd, FINDCTX *fc)
{
	WCHAR cls[128];
	size_t base = wcslen(EVERYTHING_IPC_WNDCLASSW);
	DWORD_PTR res = 0;
	int len;

	len = GetClassNameW(hwnd, cls, 128);
	if ( (len <= 0) || ((size_t)len < base) ) return TRUE;
	if ( wcsncmp(cls, EVERYTHING_IPC_WNDCLASSW, base) != 0 ) return TRUE;

	/* adopt only windows that answer the version query */
	SetLastError(0);
	if ( SendMessageTimeoutW(hwnd, EVERYTHING_WM_IPC,
			EVERYTHING_IPC_GET_MAJOR_VERSION, 0,
			SMTO_ABORTIFHUNG, FIND_TIMEOUT, &res) == 0 ){
		if ( GetLastError() == ERROR_ACCESS_DENIED ) fc->denied = TRUE;
		return TRUE;
	}
	if ( (size_t)len == base ){
		fc->exact = hwnd;
		return FALSE;	/* default instance is the best, stop */
	}
	if ( fc->other == NULL ) fc->other = hwnd;
	return TRUE;
}

static BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lParam)
{
	return CheckCandidate(hwnd, (FINDCTX *)lParam);
}

/* csModule is held by the caller. *denied is set when access was denied. */
static HWND FindEverything(BOOL *denied)
{
	FINDCTX fc;
	HWND h;

	*denied = FALSE;
	if ( (hEverything != NULL) && IsWindow(hEverything) ) return hEverything;
	hEverything = NULL;

	fc.exact = fc.other = NULL;
	fc.denied = FALSE;
	EnumWindows(EnumProc, (LPARAM)&fc);

	/* message-only windows are not enumerated by EnumWindows */
	if ( fc.exact == NULL ){
		h = NULL;
		while ( (h = FindWindowExW(HWND_MESSAGE, h, NULL, NULL)) != NULL ){
			if ( CheckCandidate(h, &fc) == FALSE ) break;
		}
	}
	hEverything = (fc.exact != NULL) ? fc.exact : fc.other;
	if ( hEverything == NULL ) *denied = fc.denied;
	return hEverything;
}

/*----------------------------------------------------------------------------
	result handling (bounds checked)
----------------------------------------------------------------------------*/
/* length of a NUL terminated WCHAR string at byte offset 'off'.
   returns -1 when out of range / not terminated inside the data */
static int SafeLen(const BYTE *base, DWORD size, DWORD off)
{
	const WCHAR *s;
	DWORD maxch, i;

	if ( (off >= size) || (off & 1) ) return -1;
	s = (const WCHAR *)(base + off);
	maxch = (size - off) / sizeof(WCHAR);
	for ( i = 0 ; i < maxch ; i++ ){
		if ( s[i] == L'\0' ) return (int)i;
	}
	return -1;
}

/* returns FALSE to stop */
static BOOL GetResults(SEARCHCTX *ctx, const BYTE *data, DWORD size)
{
	const EVERYTHING_IPC_LISTW *list = (const EVERYTHING_IPC_LISTW *)data;
	DWORD i, maxitems;
	WCHAR filepath[VFPS];

	if ( size < offsetof(EVERYTHING_IPC_LISTW, items) ) return TRUE;
	maxitems = (DWORD)((size - offsetof(EVERYTHING_IPC_LISTW, items)) /
			sizeof(EVERYTHING_IPC_ITEMW));
	if ( list->numitems < maxitems ) maxitems = list->numitems;

	for ( i = 0 ; i < maxitems ; i++ ){
		const EVERYTHING_IPC_ITEMW *item = &list->items[i];
		DWORD flags = item->flags;
		int namelen, pathlen;
		const WCHAR *name, *path;
		DWORD cmd;

		namelen = SafeLen(data, size, item->filename_offset);
		if ( namelen < 0 ) continue;
		name = (const WCHAR *)(data + item->filename_offset);

		if ( flags & EVERYTHING_IPC_DRIVE ){
			if ( namelen >= VFPS ) continue;
			memcpy(filepath, name, ((size_t)namelen + 1) * sizeof(WCHAR));
			cmd = PPXCMDID_REPORTSEARCH_DIRECTORY;
		}else{
			pathlen = SafeLen(data, size, item->path_offset);
			if ( pathlen < 0 ) continue;
			path = (const WCHAR *)(data + item->path_offset);
			if ( ((size_t)pathlen + 1 + (size_t)namelen) >= VFPS ) continue;
			memcpy(filepath, path, (size_t)pathlen * sizeof(WCHAR));
			filepath[pathlen] = L'\\';
			memcpy(filepath + pathlen + 1, name, ((size_t)namelen + 1) * sizeof(WCHAR));
			cmd = (flags & EVERYTHING_IPC_FOLDER) ?
					PPXCMDID_REPORTSEARCH_DIRECTORY : PPXCMDID_REPORTSEARCH_FILE;
		}
		if ( ctx->ppxa->Function(ctx->ppxa, cmd, filepath) != 1 ) return FALSE;
	}
	return TRUE;
}

static LRESULT CALLBACK ResultWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if ( msg == WM_COPYDATA ){
		SEARCHCTX *ctx = (SEARCHCTX *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
		const COPYDATASTRUCT *cds = (const COPYDATASTRUCT *)lParam;

		if ( (ctx != NULL) && (cds != NULL) && (cds->dwData == ctx->replyID) &&
			 (ctx->done == 0) ){
			/* mark done first: a re-entrant reply must not report twice */
			InterlockedExchange(&ctx->done, 1);
			if ( (cds->lpData != NULL) && (cds->cbData > 0) ){
				GetResults(ctx, (const BYTE *)cds->lpData, cds->cbData);
			}
			return TRUE;
		}
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* csModule is held by the caller */
static BOOL EnsureClass(void)
{
	WNDCLASSW wc;

	if ( ClassRegistered ) return TRUE;
	memset(&wc, 0, sizeof(wc));
	wc.hInstance = hDllInstance;
	wc.lpfnWndProc = ResultWindowProc;
	wc.lpszClassName = REPLY_CLASSNAME;
	if ( (RegisterClassW(&wc) == 0) &&
		 (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) ){
		return FALSE;
	}
	ClassRegistered = TRUE;
	return TRUE;
}

/*----------------------------------------------------------------------------
	migemo (all functions here: csMigemo is held by the caller)
----------------------------------------------------------------------------*/
/* "major.minor.patch" >= 1.6.1 ? unparsable -> accepted */
static BOOL VersionOk(const char *v)
{
	int part[3] = {0, 0, 0};
	int i;

	if ( v == NULL ) return TRUE;
	for ( i = 0 ; i < 3 ; i++ ){
		if ( (*v < '0') || (*v > '9') ){
			if ( i == 0 ) return TRUE;
			break;
		}
		while ( (*v >= '0') && (*v <= '9') ){
			part[i] = part[i] * 10 + (*v - '0');
			if ( part[i] > 100000 ) part[i] = 100000;
			v++;
		}
		if ( *v == '.' ) v++; else break;
	}
	if ( part[0] != 1 ) return part[0] > 1;
	if ( part[1] != 6 ) return part[1] > 6;
	return part[2] >= 1;
}

static BOOL FileExists(const WCHAR *path)
{
	DWORD a = GetFileAttributesW(path);
	return (a != INVALID_FILE_ATTRIBUTES) && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* wide path -> ANSI path usable by migemo_open. FALSE when impossible */
static BOOL PathToAnsi(const WCHAR *wpath, char *dest, int destsize)
{
	WCHAR shortpath[MAX_PATH];
	BOOL used = FALSE;

	if ( (wcslen(wpath) < MAX_PATH) &&
		 (WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wpath, -1,
			dest, destsize, NULL, &used) > 0) && !used ){
		return TRUE;
	}
	/* retry with the 8.3 name */
	if ( GetShortPathNameW(wpath, shortpath, MAX_PATH) == 0 ) return FALSE;
	used = FALSE;
	if ( (WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, shortpath, -1,
			dest, destsize, NULL, &used) > 0) && !used ){
		return TRUE;
	}
	return FALSE;
}

static BOOL FindBytes(const unsigned char *s, const unsigned char *pat, size_t patlen)
{
	size_t len = strlen((const char *)s);
	size_t i;

	if ( len < patlen ) return FALSE;
	for ( i = 0 ; i <= len - patlen ; i++ ){
		if ( memcmp(s + i, pat, patlen) == 0 ) return TRUE;
	}
	return FALSE;
}

static void UnloadMigemo(void)
{
	if ( MigemoObj != NULL ){
		if ( pMigemoClose != NULL ) pMigemoClose(MigemoObj);
		MigemoObj = NULL;
	}
	if ( hMigemoDll != NULL ){
		FreeLibrary(hMigemoDll);
		hMigemoDll = NULL;
	}
}

/* module directory (with trailing '\'). FALSE on failure */
static BOOL GetModuleDir(WCHAR *dir, size_t size)
{
	DWORD len = GetModuleFileNameW(hDllInstance, dir, (DWORD)size);
	WCHAR *p;

	if ( (len == 0) || (len >= size) ) return FALSE;
	p = wcsrchr(dir, L'\\');
	if ( p == NULL ) return FALSE;
	p[1] = L'\0';
	return TRUE;
}

static BOOL OpenMigemo(const ETSCONFIG *c)
{
	WCHAR dir[VFPS], dllpath[VFPS], dict[VFPS];
	char ansidict[MAX_PATH];
	MIGEMO_OPEN pOpen;
	MIGEMO_IS_ENABLE pIsEnable;
	MIGEMO_VERSION pVersion;
	MIGEMO_SET_ESCAPE pEscape;
	unsigned char *rx;

	if ( GetModuleDir(dir, VFPS - 64) == FALSE ) return FALSE;

	/* migemo.dll */
	if ( c->dll[0] != L'\0' ){
		wcscpy(dllpath, c->dll);
	}else{
		wcscpy(dllpath, dir);
		wcscat(dllpath, L"migemo.dll");
	}
	hMigemoDll = LoadLibraryExW(dllpath, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
	if ( hMigemoDll == NULL ) return FALSE;

	pOpen = (MIGEMO_OPEN)GetProcAddress(hMigemoDll, "migemo_open");
	pMigemoClose = (MIGEMO_CLOSE)GetProcAddress(hMigemoDll, "migemo_close");
	pMigemoQuery = (MIGEMO_QUERY)GetProcAddress(hMigemoDll, "migemo_query");
	pMigemoRelease = (MIGEMO_RELEASE)GetProcAddress(hMigemoDll, "migemo_release");
	pIsEnable = (MIGEMO_IS_ENABLE)GetProcAddress(hMigemoDll, "migemo_is_enable");
	pVersion = (MIGEMO_VERSION)GetProcAddress(hMigemoDll, "migemo_version");
	pEscape = (MIGEMO_SET_ESCAPE)GetProcAddress(hMigemoDll, "migemo_set_escape_chars");
	if ( (pOpen == NULL) || (pMigemoClose == NULL) || (pMigemoQuery == NULL) ||
		 (pMigemoRelease == NULL) || (pIsEnable == NULL) ){
		return FALSE;
	}
	/* a missing migemo_version means an old migemo */
	if ( (pVersion == NULL) || !VersionOk(pVersion()) ) return FALSE;

	/* dictionary: option > dict\utf-8\migemo-dict > dict\cp932\migemo-dict */
	if ( c->dict[0] != L'\0' ){
		wcscpy(dict, c->dict);
		if ( !FileExists(dict) ) return FALSE;
	}else{
		wcscpy(dict, dir);
		wcscat(dict, L"dict\\utf-8\\migemo-dict");
		if ( !FileExists(dict) ){
			wcscpy(dict, dir);
			wcscat(dict, L"dict\\cp932\\migemo-dict");
			if ( !FileExists(dict) ) return FALSE;
		}
	}
	if ( PathToAnsi(dict, ansidict, MAX_PATH) == FALSE ) return FALSE;

	MigemoObj = pOpen(ansidict);
	if ( (MigemoObj == NULL) || !pIsEnable(MigemoObj) ) return FALSE;

	if ( pEscape != NULL ){
		pEscape(MigemoObj, (const unsigned char *)"\\.*+^$/?{}()|[]");
	}

	/* confirm the charset of the dictionary using the query "a" */
	rx = pMigemoQuery(MigemoObj, (const unsigned char *)"a");
	if ( rx == NULL ) return FALSE;
	if ( FindBytes(rx, (const unsigned char *)"\xE3\x81\x82", 3) ){
		MigemoCP = CP_UTF8;
	}else if ( FindBytes(rx, (const unsigned char *)"\x82\xA0", 2) ){
		MigemoCP = 932;
	}else{
		pMigemoRelease(MigemoObj, rx);
		return FALSE;
	}
	pMigemoRelease(MigemoObj, rx);
	return TRUE;
}

/* Returns TRUE when migemo is ready. *newfail is set at the first failure
   (to notify only once). */
static BOOL PrepareMigemo(const ETSCONFIG *c, BOOL *newfail)
{
	ULONGLONG now = GetTickCount64();

	*newfail = FALSE;

	/* settings changed -> reload */
	if ( (wcscmp(MigemoKeyDict, c->dict) != 0) || (wcscmp(MigemoKeyDll, c->dll) != 0) ){
		UnloadMigemo();
		MigemoFailed = FALSE;
		MigemoWarned = FALSE;
		wcscpy(MigemoKeyDict, c->dict);
		wcscpy(MigemoKeyDll, c->dll);
	}
	if ( MigemoObj != NULL ) return TRUE;
	if ( MigemoFailed && ((now - MigemoFailTick) < MIGEMO_RETRY) ) return FALSE;

	if ( OpenMigemo(c) ){
		MigemoFailed = FALSE;
		MigemoWarned = FALSE;
		return TRUE;
	}
	UnloadMigemo();
	MigemoFailed = TRUE;
	MigemoFailTick = now;
	if ( !MigemoWarned ){
		MigemoWarned = TRUE;
		*newfail = TRUE;
	}
	return FALSE;
}

/* single word of 2-64 chars with at least one letter. Allowed: ASCII letters,
   digits and the symbols below. Characters that have a meaning in the
   Everything search syntax (* ? ! | " < > :), path separators (\ /),
   spaces and non-ASCII characters are not allowed (normal search is used). */
static BOOL MigemoApplicable(const WCHAR *kw, char *ascii, DWORD minlen)
{
	size_t len = wcslen(kw), i;
	BOOL hasalpha = FALSE;

	if ( (len < minlen) || (len > MIGEMO_MAX_LEN) ) return FALSE;
	for ( i = 0 ; i < len ; i++ ){
		WCHAR ch = kw[i];
		if ( ((ch >= L'a') && (ch <= L'z')) || ((ch >= L'A') && (ch <= L'Z')) ){
			hasalpha = TRUE;
		}else if ( ((ch >= L'0') && (ch <= L'9')) || (wcschr(L"-_.()[]+#&@,;'=~^$%", ch) != NULL) ){
			/* ok */
		}else{
			return FALSE;
		}
		ascii[i] = (char)ch;
	}
	ascii[len] = '\0';
	return hasalpha;
}

/* Make a regex from the keyword. Returns a HeapAlloc'ed string, or NULL
   (NULL means: use the normal search). Do not call with csModule held. */
static WCHAR *BuildRegex(PPXAPPINFOW *ppxa, const ETSCONFIG *c, const WCHAR *keyword, BOOL partial)
{
	char ascii[MIGEMO_MAX_LEN + 1];
	unsigned char *rx;
	WCHAR *wide = NULL;
	int wlen;
	BOOL newfail = FALSE;

	/* scope: 1 = Where is, 2 = one line editor, 3 = both */
	if ( !(c->migemo & (partial ? 2 : 1)) ) return NULL;
	if ( !MigemoApplicable(keyword, ascii, c->migemoMin) ) return NULL;

	/* migemo is not thread safe and may be slow: fall back when busy */
	if ( !TryEnterCriticalSection(&csMigemo) ) return NULL;
	if ( PrepareMigemo(c, &newfail) ){
		rx = pMigemoQuery(MigemoObj, (const unsigned char *)ascii);
		if ( rx != NULL ){
			wlen = MultiByteToWideChar(MigemoCP, 0, (const char *)rx, -1, NULL, 0);
			if ( (wlen > 1) && (wlen <= MIGEMO_MAX_REGEX + 1) ){
				wide = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (size_t)wlen * sizeof(WCHAR));
				if ( wide != NULL ){
					if ( MultiByteToWideChar(MigemoCP, 0, (const char *)rx, -1, wide, wlen) <= 0 ){
						HeapFree(GetProcessHeap(), 0, wide);
						wide = NULL;
					}
				}
			}
			pMigemoRelease(MigemoObj, rx);
		}
	}
	LeaveCriticalSection(&csMigemo);

	if ( newfail ){
		PostNotice(ppxa, L"PPXETS_M: migemo is unavailable. Normal search is used.");
	}
	return wide;
}

/*----------------------------------------------------------------------------
	search
----------------------------------------------------------------------------*/
static void DoSearch(PPXMSEARCHSTRUCT *pss, PPXAPPINFOW *ppxa)
{
	ETSCONFIG *c;
	DWORD maxresults, timeout, flags = 0;
	HWND hEv, hReply;
	SEARCHCTX ctx;
	const WCHAR *searchstr;
	WCHAR *regex = NULL;
	EVERYTHING_IPC_QUERYW *query;
	size_t len, size;
	COPYDATASTRUCT cds;
	DWORD_PTR sendresult = 0;
	DWORD lasterr = 0;
	ULONGLONG deadline, now;
	LRESULT sent;
	int err = ERR_NONE;
	BOOL partial, denied = FALSE, classok;
	MSG msg;

	/* the old modules would search twice: stay out of the way */
	if ( (GetModuleHandleW(L"PPXETS64.DLL") != NULL) ||
		 (GetModuleHandleW(L"PPXETP64.DLL") != NULL) ){
		return;		/* no notice: it cannot be displayed in this situation */
	}

	/* settings (copied: ETSCONFIG is large, keep it off the stack) */
	c = (ETSCONFIG *)HeapAlloc(GetProcessHeap(), 0, sizeof(ETSCONFIG));
	if ( c == NULL ) return;
	GetConfig(ppxa, c);

	partial = (pss->maxresults <= 100);
	maxresults = partial ? c->part : c->full;
	timeout = partial ? c->tmoPart : c->tmoFull;
	if ( maxresults == 0 ){		/* search disabled */
		HeapFree(GetProcessHeap(), 0, c);
		return;
	}
	if ( maxresults == 1 ) maxresults = pss->maxresults;

	/* negative cache / find Everything */
	EnterCriticalSection(&csModule);
	{
		/* a long pause means a new editing session: show the notice again
		   and probe Everything immediately */
		ULONGLONG nowtick = GetTickCount64();
		ULONGLONG msgReset = (c->retry > MSGRESET_MIN) ? c->retry : MSGRESET_MIN;
		if ( (LastSearchTick != 0) && ((nowtick - LastSearchTick) >= msgReset) ){
			LastErr = ERR_NONE;
			RetryUntil = 0;
		}
		LastSearchTick = nowtick;
	}
	if ( (RetryUntil != 0) && (GetTickCount64() < RetryUntil) ){
		LeaveCriticalSection(&csModule);
		HeapFree(GetProcessHeap(), 0, c);
		return;
	}
	hEv = FindEverything(&denied);
	if ( hEv == NULL ){
		if ( c->retry != 0 ) RetryUntil = GetTickCount64() + c->retry;
		LeaveCriticalSection(&csModule);
		Notify(ppxa, denied ? ERR_ACCESS : ERR_NOEVERYTHING);
		HeapFree(GetProcessHeap(), 0, c);
		return;
	}
	classok = EnsureClass();
	LeaveCriticalSection(&csModule);
	if ( !classok ){
		Notify(ppxa, ERR_INTERNAL);
		HeapFree(GetProcessHeap(), 0, c);
		return;
	}

	/* migemo (optional) */
	searchstr = pss->keyword;
	regex = BuildRegex(ppxa, c, pss->keyword, partial);
	if ( regex != NULL ){
		searchstr = regex;
		flags |= EVERYTHING_IPC_REGEX;
	}

	memset(&ctx, 0, sizeof(ctx));
	ctx.ppxa = ppxa;
	ctx.replyID = REPLY_COPYDATA_ID;

	hReply = CreateWindowExW(WS_EX_NOACTIVATE, REPLY_CLASSNAME, L"", 0,
			0, 0, 0, 0, HWND_MESSAGE, NULL, hDllInstance, NULL);
	if ( hReply == NULL ){
		Notify(ppxa, ERR_INTERNAL);
		goto cleanup_regex;
	}
	SetWindowLongPtrW(hReply, GWLP_USERDATA, (LONG_PTR)&ctx);

	len = wcslen(searchstr);
	size = offsetof(EVERYTHING_IPC_QUERYW, search_string) + (len + 1) * sizeof(WCHAR);
	query = (EVERYTHING_IPC_QUERYW *)HeapAlloc(GetProcessHeap(), 0, size);
	if ( query == NULL ){
		DestroyWindow(hReply);
		goto cleanup_regex;
	}
	query->reply_hwnd = (DWORD)(DWORD_PTR)hReply;	/* HWND fits in 32bit */
	query->reply_copydata_message = ctx.replyID;
	query->search_flags = flags;
	query->offset = 0;
	query->max_results = maxresults;
	memcpy(query->search_string, searchstr, (len + 1) * sizeof(WCHAR));

	cds.dwData = EVERYTHING_IPC_COPYDATAQUERY;
	cds.cbData = (DWORD)size;
	cds.lpData = query;

	deadline = GetTickCount64() + timeout;

	/* Everything usually replies (as a sent message) while we are in here */
	SetLastError(0);
	sent = SendMessageTimeoutW(hEv, WM_COPYDATA, (WPARAM)hReply, (LPARAM)&cds,
			SMTO_ABORTIFHUNG, timeout, &sendresult);
	lasterr = GetLastError();
	if ( (sent == 0) && (ctx.done == 0) ){
		err = (lasterr == ERROR_ACCESS_DENIED) ? ERR_ACCESS : ERR_NORESPONSE;
	}else if ( (sent != 0) && (sendresult == 0) && (ctx.done == 0) ){
		err = ERR_REJECTED;
	}else{
		/* bounded wait for a reply that has not arrived yet */
		while ( ctx.done == 0 ){
			ULONGLONG remain;

			now = GetTickCount64();
			if ( now >= deadline ){
				err = ERR_NORESPONSE;
				break;
			}
			remain = deadline - now;
			if ( remain > 50 ) remain = 50;
			MsgWaitForMultipleObjects(0, NULL, FALSE, (DWORD)remain,
					QS_SENDMESSAGE | QS_POSTMESSAGE);
			while ( PeekMessageW(&msg, hReply, 0, 0, PM_REMOVE) ){
				DispatchMessageW(&msg);
			}
		}
	}

	InterlockedExchange(&ctx.done, 1);		/* ignore any late reply */
	SetWindowLongPtrW(hReply, GWLP_USERDATA, 0);
	DestroyWindow(hReply);
	HeapFree(GetProcessHeap(), 0, query);

	EnterCriticalSection(&csModule);
	if ( err == ERR_NONE ){
		RetryUntil = 0;
	}else{
		if ( c->retry != 0 ) RetryUntil = GetTickCount64() + c->retry;
		hEverything = NULL;		/* look for the window again next time */
	}
	LeaveCriticalSection(&csModule);
	Notify(ppxa, err);

cleanup_regex:
	if ( regex != NULL ) HeapFree(GetProcessHeap(), 0, regex);
	HeapFree(GetProcessHeap(), 0, c);
}

/*----------------------------------------------------------------------------
	entry
----------------------------------------------------------------------------*/
EXTDLL int PPXAPI ModuleEntry(PPXAPPINFOW *ppxa, DWORD cmdID, PPXMODULEPARAM pxs)
{
	if ( cmdID == PPXMEVENT_SEARCH ){
		if ( pxs.search->keyword[0] != L'\0' ) DoSearch(pxs.search, ppxa);
		return PPXMRESULT_SKIP;	/* always SKIP so the next module runs too */
	}

	if ( cmdID == PPXMEVENT_CLEANUP ){
		/* ppxa is not available here */
		EnterCriticalSection(&csMigemo);
		UnloadMigemo();
		MigemoFailed = FALSE;
		MigemoWarned = FALSE;
		MigemoKeyDict[0] = MigemoKeyDll[0] = L'\0';
		LeaveCriticalSection(&csMigemo);

		EnterCriticalSection(&csModule);
		if ( ClassRegistered ){
			UnregisterClassW(REPLY_CLASSNAME, hDllInstance);
			ClassRegistered = FALSE;
		}
		hEverything = NULL;
		RetryUntil = 0;
		LastErr = ERR_NONE;
		LastSearchTick = 0;
		CfgLoaded = FALSE;
		LeaveCriticalSection(&csModule);
		return PPXMRESULT_SKIP;
	}

	if ( cmdID == PPXM_INFORMATION ){
		if ( pxs.info->infotype == 0 ){
			pxs.info->typeflags = PPMTYPEFLAGS(PPXMEVENT_SEARCH);
			wcscpy(pxs.info->copyright,
				L"PPXETS_M (PPx Everything Search Module, migemo edition) based on PPXETS Copyright (c)TORO");
			return TRUE;
		}
	}
	return FALSE;
}
