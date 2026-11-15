/* longshot_tls.dll: a thread-local slot of Windows' own, for a module the app loads by hand whose thread-local data cannot
   be emulated (Mesa's Dozen, built with MSVC; xbox/loader/pe_loader.h, pel_tls_slot). Windows gives this DLL a TLS index
   when it loads and a block on every thread, copied from its template; the buffer is the only thread-local data here (the
   static C runtime has none), so the whole block is the slot. The C runtime's TLS directory (tlssup) is the one Windows
   takes: a directory of this file's own, with no entry point, is not given an index. Built with MSVC:
   cl /O2 /MT tls_slot.c /LD */
#include <windows.h>

#define SLOT_BYTES (64 * 1024)   /* (Dozen of Mesa 26.2: 37 KB) */

__declspec(thread) unsigned char longshot_tls_block[SLOT_BYTES];

extern ULONG _tls_index;
extern const IMAGE_TLS_DIRECTORY64 _tls_used;

/* The slot: the index Windows gave, the template it copies into each new thread's block, and its size */
__declspec(dllexport) void longshot_tls_slot(ULONG *index, void **template_data, SIZE_T *capacity) {
    longshot_tls_block[0] = longshot_tls_block[0];   /* (keeps the buffer) */
    *index = _tls_index;
    *template_data = (void *)(ULONG_PTR)_tls_used.StartAddressOfRawData;
    *capacity = (SIZE_T)(_tls_used.EndAddressOfRawData - _tls_used.StartAddressOfRawData);
}
