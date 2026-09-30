// Loads PhoneLink.asi against the stub ScriptHookV.dll and runs its script loop.
// stdin commands: "y" = press answer key, "n" = press hang-up key, "q" = quit.
#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <iostream>
#include <string>
#include <thread>

// Prints a symbolized stack for C++ exceptions and crashes thrown anywhere in the process.
static LONG CALLBACK onException(PEXCEPTION_POINTERS ep)
{
	DWORD code = ep->ExceptionRecord->ExceptionCode;
	if (code != 0xE06D7363 && code != 0xC0000005 && code != 0xC0000409)
		return EXCEPTION_CONTINUE_SEARCH;
	HANDLE proc = GetCurrentProcess();
	static bool symInit = SymInitialize(proc, nullptr, TRUE);
	printf("[harness] exception 0x%08lX on thread %lu\n", code, GetCurrentThreadId());
	void* frames[48];
	USHORT n = CaptureStackBackTrace(0, 48, frames, nullptr);
	char buf[sizeof(SYMBOL_INFO) + 256];
	auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
	for (USHORT i = 0; i < n; ++i)
	{
		sym->SizeOfStruct = sizeof(SYMBOL_INFO);
		sym->MaxNameLen = 255;
		DWORD64 disp = 0;
		IMAGEHLP_LINE64 line{sizeof(line)};
		DWORD ldisp = 0;
		if (SymFromAddr(proc, (DWORD64)frames[i], &disp, sym))
		{
			if (SymGetLineFromAddr64(proc, (DWORD64)frames[i], &ldisp, &line))
				printf("   %2u %s  (%s:%lu)\n", i, sym->Name, line.FileName, line.LineNumber);
			else
				printf("   %2u %s\n", i, sym->Name);
		}
	}
	fflush(stdout);
	return EXCEPTION_CONTINUE_SEARCH;
}

int main(int argc, char** argv)
{
	if (getenv("HARNESS_TRACE"))
		AddVectoredExceptionHandler(1, onException);
	const char* asi = argc > 1 ? argv[1] : "PhoneLink.asi";
	HMODULE shv = LoadLibraryA("ScriptHookV.dll");
	HMODULE plugin = LoadLibraryA(asi);
	if (!shv || !plugin)
	{
		printf("failed to load (shv=%p plugin=%p, err=%lu)\n", shv, plugin, GetLastError());
		return 1;
	}
	auto run = reinterpret_cast<void (*)()>(GetProcAddress(shv, "?harnessRunScript@@YAXXZ"));
	auto key = reinterpret_cast<void (*)(DWORD)>(GetProcAddress(shv, "?harnessKey@@YAXK@Z"));
	auto phone = reinterpret_cast<void (*)(int, int)>(GetProcAddress(shv, "?harnessPhone@@YAXHH@Z"));
	if (!run || !key || !phone)
	{
		printf("stub exports missing\n");
		return 1;
	}
	std::thread(run).detach();
	printf("harness running\n");
	fflush(stdout);

	std::string line;
	while (std::getline(std::cin, line))
	{
		if (line == "y")
			key(0x59);
		else if (line == "n")
			key(0x4E);
		else if (line == "q")
			break;
		else if (line == "phone down")
			phone(0, 0);
		else if (line == "phone up")
			phone(1, 0);
		else if (line == "phone contacts")
			phone(2, 0);
		else if (line.rfind("phone select ", 0) == 0)
			phone(3, atoi(line.c_str() + 13));
		else if (line == "phone cancel")
			phone(4, 0);
		else if (line.rfind("k ", 0) == 0)
			key(static_cast<DWORD>(strtoul(line.c_str() + 2, nullptr, 16))); // e.g. "k 76" = F7
		else if (line == "exit")
		{
			// Like the game quitting: other threads are killed, then DLLs get
			// DLL_PROCESS_DETACH and their static destructors run.
			printf("harness: ExitProcess\n");
			fflush(stdout);
			ExitProcess(0);
		}
	}
	TerminateProcess(GetCurrentProcess(), 0);
}
