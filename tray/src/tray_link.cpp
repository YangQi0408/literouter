// The gateway links zlib statically while GIO uses the platform's shared zlib.
// This executable is not a zlib provider: keep its definitions private so GIO
// resolves its own ABI instead of interposing a different library build. These
// are exactly the zlib references GIO's runtime closure imports; no code or
// symbol visibility of any shared dependency is modified.
#if defined(__ELF__)
asm(".hidden deflate\n"
    ".hidden deflateEnd\n"
    ".hidden deflateInit_\n"
    ".hidden deflateInit2_\n"
    ".hidden deflateReset\n"
    ".hidden inflate\n"
    ".hidden inflateEnd\n"
    ".hidden inflateInit_\n"
    ".hidden inflateInit2_\n"
    ".hidden inflateReset\n");
#endif
