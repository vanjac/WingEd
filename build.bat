if "%VSCMD_ARG_TGT_ARCH%"=="x64" (
    set CHROMA_LINKER=/subsystem:console,"5.02"
) else (
    set CHROMA_LINKER=/subsystem:console,"5.01"
)
cd build
rc /fo resource.res ..\src\resource.rc
cl /nologo /std:c++14 /MP /W4 /EHsc /experimental:external /external:W1 /Fe: winged.exe^
    /Zi /MTd /D CHROMA_DEBUG^
    /I ..\src /I ..\lib\winchroma /external:I ..\lib\glm /external:I ..\lib\immer^
    ..\src\*.cpp resource.res^
    /link /incremental:no /manifest:EMBED %CHROMA_LINKER%
cd ..
