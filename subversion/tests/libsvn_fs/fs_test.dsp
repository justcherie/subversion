# Microsoft Developer Studio Project File - Name="tests_libsvn_fs_fs" - Package Owner=<4>
# Microsoft Developer Studio Generated Build File, Format Version 6.00
# ** DO NOT EDIT **

# TARGTYPE "Win32 (x86) Console Application" 0x0103

CFG=tests_libsvn_fs_fs - Win32 Debug
!MESSAGE This is not a valid makefile. To build this project using NMAKE,
!MESSAGE use the Export Makefile command and run
!MESSAGE 
!MESSAGE NMAKE /f "fs_test.mak".
!MESSAGE 
!MESSAGE You can specify a configuration when running NMAKE
!MESSAGE by defining the macro CFG on the command line. For example:
!MESSAGE 
!MESSAGE NMAKE /f "fs_test.mak" CFG="tests_libsvn_fs_fs - Win32 Debug"
!MESSAGE 
!MESSAGE Possible choices for configuration are:
!MESSAGE 
!MESSAGE "tests_libsvn_fs_fs - Win32 Release" (based on "Win32 (x86) Console Application")
!MESSAGE "tests_libsvn_fs_fs - Win32 Debug" (based on "Win32 (x86) Console Application")
!MESSAGE 

# Begin Project
# PROP AllowPerConfigDependencies 0
# PROP Scc_ProjName ""
# PROP Scc_LocalPath ""
CPP=cl.exe
RSC=rc.exe

!IF  "$(CFG)" == "tests_libsvn_fs_fs - Win32 Release"

# PROP BASE Use_MFC 0
# PROP BASE Use_Debug_Libraries 0
# PROP BASE Output_Dir "Release"
# PROP BASE Intermediate_Dir "Release"
# PROP BASE Target_Dir ""
# PROP Use_MFC 0
# PROP Use_Debug_Libraries 0
# PROP Output_Dir "Release"
# PROP Intermediate_Dir "Release\obj"
# PROP Ignore_Export_Lib 0
# PROP Target_Dir ""
# ADD BASE CPP /nologo /W3 /GX /O2 /D "WIN32" /D "NDEBUG" /D "_CONSOLE" /D "_MBCS" /YX /FD /c
# ADD CPP /nologo /MD /W3 /GX /O2 /I ".." /I "..\..\include" /I "..\..\..\apr\include" /I "..\..\..\expat-lite" /I "..\..\..\db3-win32\include" /I "..\..\.." /D "NDEBUG" /D "APR_DECLARE_STATIC" /D "WIN32" /D "_WINDOWS_CONSOLE" /D alloca=_alloca /FD /c
# SUBTRACT CPP /YX
# ADD BASE RSC /l 0x424 /d "NDEBUG"
# ADD RSC /l 0x424 /d "NDEBUG"
BSC32=bscmake.exe
# ADD BASE BSC32 /nologo
# ADD BSC32 /nologo
LINK32=link.exe
# ADD BASE LINK32 kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib /nologo /subsystem:console /machine:I386
# ADD LINK32 ..\..\libsvn_fs\Release\libsvn_fs.lib ..\..\tests\Release\libsvn_tests_main.lib ..\..\libsvn_subr\Release\libsvn_subr.lib ..\..\..\apr\LibR\apr.lib ..\..\..\expat-lite\Release\libexpat.lib ..\..\..\db3-win32\lib\libdb33.lib kernel32.lib advapi32.lib ws2_32.lib mswsock.lib ole32.lib /nologo /subsystem:console /machine:I386 /out:"Release/fs-test.exe"

!ELSEIF  "$(CFG)" == "tests_libsvn_fs_fs - Win32 Debug"

# PROP BASE Use_MFC 0
# PROP BASE Use_Debug_Libraries 1
# PROP BASE Output_Dir "Debug"
# PROP BASE Intermediate_Dir "Debug"
# PROP BASE Target_Dir ""
# PROP Use_MFC 0
# PROP Use_Debug_Libraries 1
# PROP Output_Dir "Debug"
# PROP Intermediate_Dir "Debug\obj"
# PROP Ignore_Export_Lib 0
# PROP Target_Dir ""
# ADD BASE CPP /nologo /W3 /Gm /GX /ZI /Od /D "WIN32" /D "_DEBUG" /D "_CONSOLE" /D "_MBCS" /YX /FD /GZ /c
# ADD CPP /nologo /MDd /W3 /GX /ZI /Od /I ".." /I "..\..\include" /I "..\..\..\apr\include" /I "..\..\..\expat-lite" /I "..\..\..\db3-win32\include" /I "..\..\.." /D "SVN_DEBUG" /D "_DEBUG" /D "APR_DECLARE_STATIC" /D "WIN32" /D "_WINDOWS_CONSOLE" /D alloca=_alloca /FD /GZ /c
# SUBTRACT CPP /YX
# ADD BASE RSC /l 0x424 /d "_DEBUG"
# ADD RSC /l 0x424 /d "_DEBUG"
BSC32=bscmake.exe
# ADD BASE BSC32 /nologo
# ADD BSC32 /nologo
LINK32=link.exe
# ADD BASE LINK32 kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib /nologo /subsystem:console /debug /machine:I386 /pdbtype:sept
# ADD LINK32 ..\..\libsvn_fs\Debug\libsvn_fs.lib ..\..\tests\Debug\libsvn_tests_main.lib ..\..\libsvn_subr\Debug\libsvn_subr.lib ..\..\..\apr\LibD\apr.lib ..\..\..\expat-lite\Debug\libexpat.lib ..\..\..\db3-win32\lib\libdb33d.lib kernel32.lib advapi32.lib ws2_32.lib mswsock.lib ole32.lib /nologo /subsystem:console /debug /machine:I386 /out:"Debug/fs-test.exe" /pdbtype:sept
# SUBTRACT LINK32 /incremental:no

!ENDIF 

# Begin Target

# Name "tests_libsvn_fs_fs - Win32 Release"
# Name "tests_libsvn_fs_fs - Win32 Debug"
# Begin Group "Source Files"

# PROP Default_Filter "cpp;c;cxx;rc;def;r;odl;idl;hpj;bat"
# Begin Source File

SOURCE=".\fs-test.c"
# End Source File
# End Group
# End Target
# End Project
