cd build
rc /fo resource.res ..\src\resource.rc
cl /nologo /std:c++14 /MP /W4 /EHsc /Fe: winged.exe^
    /Zi /MTd /D CHROMA_DEBUG^
    /I ..\src /I ..\lib\glm /I ..\lib\immer /I ..\lib\winchroma^
    ..\src\*.cpp resource.res^
    /link /incremental:no /manifest:EMBED
cd ..
