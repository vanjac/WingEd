cd build
rc /fo resource.res ..\src\resource.rc
cl /nologo /GL /O2 /EHsc /MP /W4 /Fe: winged.exe /I ..\src /I ..\lib\glm /I ..\lib\immer /I ..\lib\winchroma ..\src\*.cpp resource.res /link /incremental:no /manifest:EMBED
cd ..
