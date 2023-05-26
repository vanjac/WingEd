cd build
rc /fo resource.res ..\src\resource.rc
cl /nologo /std:c++14 /MP /W4 /EHsc /experimental:external /external:W1 /Fe: winged.exe^
    /Zi /MTd /D CHROMA_DEBUG^
    /I ..\src /I ..\lib\winchroma /external:I ..\lib\glm /external:I ..\lib\immer^
    ..\src\*.cpp resource.res^
    /link /incremental:no /manifest:EMBED
cd ..
