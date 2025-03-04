all: build/resource.coff
	x86_64-w64-mingw32-g++-win32 -o build/winged.exe \
		-std=c++17 -pedantic -Wall -Wextra \
		-Wno-multichar -Wno-unknown-pragmas -Wno-format \
		-Wno-missing-field-initializers -Wno-cast-function-type \
		-DCHROMA_DEBUG \
		-Isrc -Ilib/winchroma -isystem lib/glm -isystem lib/immer -isystem lib/glad \
		src/*.cpp lib/glad/glad.c lib/glad/glad_wgl.c build/resource.coff \
		-luser32 -lgdi32 -lcomctl32 -lcomdlg32 \
		-lrpcrt4 -lopengl32 -lglu32 -lgdiplus -lshlwapi \
		-static

build/resource.coff: src/resource.rc
	x86_64-w64-mingw32-windres src/resource.rc -O coff build/resource.coff
