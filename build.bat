call vcvars64

set clang_settings=-march=nehalem -O2 -DNDEBUG -Werror -Wall -Wno-unused-function -g -gcodeview -D_CRT_SECURE_NO_WARNINGS

set tb_source_files=src/tb/tb.c ^
	src/tb/tb_builder.c ^
	src/tb/x64/x64.c ^
	src/tb/opt/tb_opt.c ^
	src/tb/tb_coff.c ^
	src/tb/tb_elf64.c ^
	src/tb/tb_jit.c ^
	src/tb/tb_win32.c ^
	src/tb/tb_internal.c ^
	src/tb/stb_ds.c

IF NOT exist build (mkdir build)
clang %clang_settings% src/example_jit.c %tb_source_files% -o build/example.exe

