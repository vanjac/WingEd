CC  := i686-w64-mingw32-gcc-win32
CXX := i686-w64-mingw32-g++-win32
rc  := i686-w64-mingw32-windres

headers := $(wildcard src/*.h) lib/winchroma/winchroma.h lib/glad/glad.h lib/glad/glad_wgl.h
sources := $(wildcard src/*.cpp)
objects := $(sources:src/%.cpp=build/%.o)

CXXFLAGS := -std=c++14 -fno-rtti -pedantic -Wall -Wextra -Wdeprecated -Wconditionally-supported \
	-Wmultiple-inheritance -Wold-style-cast -Wsuggest-override -Wmissing-include-dirs \
	-Wconversion -Wshadow=local -Wnon-virtual-dtor \
	-Wno-unused-function -Wno-multichar -Wno-unknown-pragmas -Wno-format \
	-Wno-missing-field-initializers -Wno-cast-function-type \
	-DENTRY_APP_MAIN

debug: CXXFLAGS += -g -Og -DCHROMA_DEBUG
debug: build/winged.exe

release: CXXFLAGS += -mwindows -Os -s -flto=auto -DNDEBUG
release: CXXFLAGS += -fno-declone-ctor-dtor # https://gcc.gnu.org/bugzilla/show_bug.cgi?id=106103
release: clean build/winged.exe

build/winged.exe: $(objects) build/glad.o build/glad_wgl.o build/resource.coff
	@echo "Linking..."
	$(CXX) -o build/winged.exe $(CXXFLAGS) \
		$(objects) build/glad.o build/glad_wgl.o build/resource.coff \
		-luser32 -lgdi32 -lcomctl32 -lcomdlg32 \
		-lrpcrt4 -lopengl32 -lglu32 -lgdiplus -lshlwapi \
		-static

$(objects): build/%.o: src/%.cpp $(headers)
	@echo "Building $<..."
	@$(CXX) -c $< -o $@ $(CXXFLAGS) \
		-Isrc -Ilib/winchroma -isystem lib/glm -isystem lib/immer -isystem lib/glad

build/glad.o: lib/glad/glad.c lib/glad/glad.h
	$(CC) -c lib/glad/glad.c -o build/glad.o -Ilib/glad

build/glad_wgl.o: lib/glad/glad_wgl.c lib/glad/glad_wgl.h
	$(CC) -c lib/glad/glad_wgl.c -o build/glad_wgl.o -Ilib/glad

build/resource.coff: src/resource.rc
	$(rc) src/resource.rc -O coff build/resource.coff

clean:
	rm -r build
	mkdir build
	touch build/empty
