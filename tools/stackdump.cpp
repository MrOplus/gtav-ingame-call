// stackdump <pid> [tid]: prints the user-mode call stack of every thread (or one thread)
// of a running process. Used to diagnose GTA5.exe hanging on exit.
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

static void dumpThread(HANDLE proc, DWORD tid)
{
	HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION, FALSE, tid);
	if (!th)
	{
		printf("thread %lu: OpenThread failed (%lu)\n", tid, GetLastError());
		return;
	}
	SuspendThread(th);
	CONTEXT ctx{};
	ctx.ContextFlags = CONTEXT_FULL;
	if (!GetThreadContext(th, &ctx))
	{
		printf("thread %lu: GetThreadContext failed (%lu)\n", tid, GetLastError());
		ResumeThread(th);
		CloseHandle(th);
		return;
	}
	printf("thread %lu:\n", tid);
	STACKFRAME64 frame{};
	frame.AddrPC.Offset = ctx.Rip;
	frame.AddrPC.Mode = AddrModeFlat;
	frame.AddrFrame.Offset = ctx.Rbp;
	frame.AddrFrame.Mode = AddrModeFlat;
	frame.AddrStack.Offset = ctx.Rsp;
	frame.AddrStack.Mode = AddrModeFlat;
	char symBuf[sizeof(SYMBOL_INFO) + 512];
	auto* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
	for (int i = 0; i < 64; ++i)
	{
		if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, th, &frame, &ctx, nullptr, SymFunctionTableAccess64,
				SymGetModuleBase64, nullptr) || frame.AddrPC.Offset == 0)
			break;
		DWORD64 pc = frame.AddrPC.Offset;
		char modName[MAX_PATH] = "?";
		IMAGEHLP_MODULE64 mod{sizeof(mod)};
		if (SymGetModuleInfo64(proc, pc, &mod))
			strncpy_s(modName, mod.ModuleName, _TRUNCATE);
		sym->SizeOfStruct = sizeof(SYMBOL_INFO);
		sym->MaxNameLen = 511;
		DWORD64 disp = 0;
		if (SymFromAddr(proc, pc, &disp, sym))
			printf("  %2d %s!%s+0x%llx\n", i, modName, sym->Name, disp);
		else
			printf("  %2d %s+0x%llx\n", i, modName, pc - mod.BaseOfImage);
	}
	ResumeThread(th);
	CloseHandle(th);
}

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		printf("usage: stackdump <pid> [tid]\n");
		return 1;
	}
	DWORD pid = strtoul(argv[1], nullptr, 10);
	DWORD only = argc > 2 ? strtoul(argv[2], nullptr, 10) : 0;
	HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
	if (!proc)
	{
		printf("OpenProcess failed (%lu)\n", GetLastError());
		return 1;
	}
	SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
	char searchPath[1024];
	GetEnvironmentVariableA("_NT_SYMBOL_PATH", searchPath, sizeof(searchPath));
	if (!SymInitialize(proc, searchPath[0] ? searchPath : nullptr, TRUE))
		printf("SymInitialize failed (%lu) - module names only\n", GetLastError());

	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	THREADENTRY32 te{sizeof(te)};
	for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
	{
		if (te.th32OwnerProcessID == pid && (!only || te.th32ThreadID == only))
			dumpThread(proc, te.th32ThreadID);
	}
	CloseHandle(snap);
	SymCleanup(proc);
	CloseHandle(proc);
	return 0;
}
