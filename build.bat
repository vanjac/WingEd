cd build
rc /fo resource.res ..\src\resource.rc
cl /nologo /MP /W4 /EHsc /Fe: winged.exe^
    /Zi /MTd /D CHROMA_DEBUG^
    /I ..\src /I ..\lib\glm /I ..\lib\immer /I ..\lib\winchroma^
    ..\src\*.cpp resource.res^
    /link /incremental:no /manifest:EMBED
cd ..
