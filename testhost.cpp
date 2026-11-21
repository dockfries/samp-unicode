// 32-bit load harness: LoadLibrary the .asi, wait, then unload.
// Build: cl /MT /O2 testhost.cpp /Fe:testhost.exe
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: testhost <asi> [milliseconds]\n"); return 2; }
    printf("[host] loading %s\n", argv[1]);
    HMODULE h = LoadLibraryA(argv[1]);
    printf("[host] LoadLibrary -> %p (err=%lu)\n", (void*)h, GetLastError());
    if (!h) return 1;
    int ms = (argc >= 3) ? atoi(argv[2]) : 3000;
    printf("[host] sleeping %d ms...\n", ms);
    Sleep((DWORD)ms);
    printf("[host] still alive, unloading\n");
    FreeLibrary(h);
    printf("[host] done\n");
    return 0;
}
