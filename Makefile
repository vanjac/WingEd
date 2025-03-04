cxx := x86_64-w64-mingw32-g++-win32
rc  := x86_64-w64-mingw32-windres

all: build/glad.o build/glad_wgl.o build/resource.coff
	$(cxx) -o build/winged.exe \
		-std=c++17 -pedantic -Wall -Wextra -Wmultiple-inheritance -Wold-style-cast \
		-Wno-multichar -Wno-unknown-pragmas -Wno-format \
		-Wno-missing-field-initializers -Wno-cast-function-type \
		-DCHROMA_DEBUG \
		-Isrc -Ilib/winchroma -isystem lib/glm -isystem lib/immer -isystem lib/glad \
		src/*.cpp build/glad.o build/glad_wgl.o build/resource.coff \
		-luser32 -lgdi32 -lcomctl32 -lcomdlg32 \
		-lrpcrt4 -lopengl32 -lglu32 -lgdiplus -lshlwapi \
		-static

build/glad.o: lib/glad/glad.c lib/glad/glad.h
	$(cxx) -c lib/glad/glad.c -o build/glad.o -Ilib/glad

build/glad_wgl.o: lib/glad/glad_wgl.c lib/glad/glad_wgl.h
	$(cxx) -c lib/glad/glad_wgl.c -o build/glad_wgl.o -Ilib/glad

build/resource.coff: src/resource.rc
	$(rc) src/resource.rc -O coff build/resource.coff

clean:
	rm -r build
	mkdir build
	touch build/empty
