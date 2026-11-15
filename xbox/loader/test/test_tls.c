/* A module with real thread-local data (built without -femulated-tls): refused without a TLS slot, and with one, each
   thread's copy starts from its initial value. */
_Thread_local int per_thread = 5;
_Thread_local char big[4000] = {1};

__declspec(dllexport) int tls_value(void) { return ++per_thread + big[0] * 100; }
