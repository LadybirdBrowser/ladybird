// Minimal weak definitions to satisfy FPC RTTI references when linking
// a raw Pascal object directly into a C/C++ shared library. The Pascal
// code does not use RTTI at runtime; these are only referenced symbolically.

#if defined(__APPLE__)
__attribute__((weak)) unsigned char rtti_sys_byte_indirect asm("_RTTI_$SYSTEM_$$_BYTE$indirect");
__attribute__((weak)) unsigned char rtti_sys_double_indirect asm("_RTTI_$SYSTEM_$$_DOUBLE$indirect");
__attribute__((weak)) unsigned char rtti_sys_longword_indirect asm("_RTTI_$SYSTEM_$$_LONGWORD$indirect");
__attribute__((weak)) unsigned char rtti_sys_pchar_indirect asm("_RTTI_$SYSTEM_$$_PCHAR$indirect");
__attribute__((weak)) unsigned char rtti_sys_pointer_indirect asm("_RTTI_$SYSTEM_$$_POINTER$indirect");
__attribute__((weak)) unsigned char rtti_sys_qword_indirect asm("_RTTI_$SYSTEM_$$_QWORD$indirect");
#else
__attribute__((weak)) unsigned char rtti_sys_byte_indirect asm("RTTI_$SYSTEM_$$_BYTE$indirect");
__attribute__((weak)) unsigned char rtti_sys_double_indirect asm("RTTI_$SYSTEM_$$_DOUBLE$indirect");
__attribute__((weak)) unsigned char rtti_sys_longword_indirect asm("RTTI_$SYSTEM_$$_LONGWORD$indirect");
__attribute__((weak)) unsigned char rtti_sys_pchar_indirect asm("RTTI_$SYSTEM_$$_PCHAR$indirect");
__attribute__((weak)) unsigned char rtti_sys_pointer_indirect asm("RTTI_$SYSTEM_$$_POINTER$indirect");
__attribute__((weak)) unsigned char rtti_sys_qword_indirect asm("RTTI_$SYSTEM_$$_QWORD$indirect");
#endif

// No runtime function stubs: we rely on the Pascal unit to avoid
// emitting helpers or to provide local implementations when needed.
