#pragma once
// FileExplorerOps.h
// XbDiag - File operations engine (copy/move/delete) and destination picker UI.

#include "DiagCommon.h"
#include "FileExplorer.h"

void FE_Ops_Snap(FileOpType op);            // snapshot marked items to clipboard
void FE_Ops_StartTo(const char* destDir);   // start op to specific dest dir
bool FE_Ops_AnyDestExists(const char* destDir); // true if any clipboard item already exists at dest
void FE_Ops_Start(FileOpType op);           // start op into current directory
void FE_Ops_Tick();                         // advance one tick of active op
void FE_Ops_MkDir(const char* name);        // create folder in current directory, reload, reposition cursor

void FE_Ops_PickLoadDriveList();            // load drive list into picker
void FE_Ops_PickLoadDir(const char* path);  // load directory into picker
void FE_Ops_DrawPicker();
void FE_Ops_EnterSelected();           // enter selected picker item
bool FE_Ops_DeleteRecursive(const char* path, bool isDir); // delete file/dir recursively                   // render destination picker overlay

// Directory loading — called by picker and op completion
void FE_Ops_LoadDirectory(const char* path);
void FE_Ops_LoadDriveList();