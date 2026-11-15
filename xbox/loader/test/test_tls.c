/* A module with real thread-local data, which the loader must refuse (built without -femulated-tls). */
_Thread_local int per_thread = 5;

__declspec(dllexport) int tls_value(void) { return ++per_thread; }
