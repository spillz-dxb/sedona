//
// Copyright (c) 2007 Tridium, Inc.
// Licensed under the Academic Free License version 3.0
//
// History:
//   3 Mar 07  Brian Frank  Creation
//

#include "sedona.h"
#ifdef USE_STANDARD_MAIN

// includes
#include <stdio.h>
#include <stdlib.h>
#include "errorcodes.h"

//needed to pass version info from gcc command line as a string value (only needed for QNX)
#define VER1(x) #x
#define VER(x) VER1(x)
//#define VER VER_(PLAT_BUILD_VERSION)


// sys::Sys forward
int64_t sys_Sys_ticks(SedonaVM* vm, Cell* params);
void sys_Sys_sleep(SedonaVM* vm, Cell* params);

int64_t yieldNs = 0;

// forwards
static int printUsage(const char* exe);
static int printVersion();
static int loadFile(const char* filename, uint8_t** addr, size_t* size);
static void checkStaged(const char* filename);
static void onAssertFailure(const char* location, uint16_t linenum);
static int  commonVmSetup(SedonaVM* vm, const char* scodeFile);
static int  runInPlatformMode();
static int  runInStandaloneMode(const char* filename, int vmArgc, char* vmArgv[]);

// auto-generated by sedonac in "nativetable.c"
extern NativeMethod* nativeTable[];

////////////////////////////////////////////////////////////////
// Main
////////////////////////////////////////////////////////////////

int main(int argc, char *argv[])
{
  //int result;
  int i;
  bool runAsPlatform = FALSE;
  char* filename = NULL;
  int optCount = 0;

  // parse arguments
  for (i=1; i<argc; ++i)
  {
    char* arg = argv[i];
    if (arg[0] == '-')
    {
      if (strcmp(arg, "--?") == 0)   return printUsage(argv[0]);
      if (strcmp(arg, "--ver") == 0) return printVersion();

      if (strcmp(arg, "--plat") == 0) runAsPlatform = TRUE;
      else if (strncmp(arg, "--home=", 7) == 0)
      {
        char* home = arg+7;
        if (strlen(arg) < 8) return printUsage(argv[0]);
        if (_chdir(home) != 0) return printUsage(argv[0]);
        optCount++;
      }
    }
    else
    {
      if (filename == NULL)
        filename = arg;
    }
  }

  printVersion();
  if (runAsPlatform)
  {
    return runInPlatformMode();
  }
  else
  {
    if (filename == NULL)
    {
      printUsage(argv[0]);
      return ERR_INPUT_FILE_NOT_FOUND;
    }
    return runInStandaloneMode(filename, argc - (2 + optCount), argv + (2+optCount));
  }
}

////////////////////////////////////////////////////////////////
// Platform VM
////////////////////////////////////////////////////////////////

static int runInPlatformMode()
{
  int result = 0;
  const char* scode = "kits.scode";
  const char* app   = "app.sab";
  SedonaVM vm;
  bool quit = FALSE;

  // Clear the VM struct (ensure NULL pointers at start)
  memset((void*)&vm , 0 , sizeof(SedonaVM));

  printf("Running SVM in Platform Mode\n");
  do
  {
    checkStaged(scode);
    checkStaged(app);

    if ((result = commonVmSetup(&vm, scode)) != 0)
      return result;
    vm.args     = (const char**)&app;
    vm.argsLen  = 1;

    result = vmRun(&vm);
    while ((result == ERR_HIBERNATE) || (result == ERR_YIELD))
    {
      //  Simulate hibernate and yield
      if (result == ERR_HIBERNATE)
        printf("-- Simulated hibernate --\n");       
      else
      {  
        sys_Sys_sleep(NULL, (Cell*)&yieldNs); 
        yieldNs = 0;
      }  
      
      
      result = vmResume(&vm);
    }

    if (result != 0)
    {
      if (result == ERR_RESTART)
        printf("Restarting VM\n\n");
      else
      {
        printf("Cannot run VM (%d)\n", result);
        quit = TRUE;
      }
    }
    else
    {
      printf("Quitting\n");
      quit = TRUE;
    }
  } while (!quit);

  return result;
}

////////////////////////////////////////////////////////////////
// Standalone VM
////////////////////////////////////////////////////////////////

static int runInStandaloneMode(const char* filename, int vmArgc, char* vmArgv[])
{
  int result;
  SedonaVM vm;
  int64_t t1, t2;

  // Clear the VM struct (ensure NULL pointers at start)
  memset((void*)&vm , 0 , sizeof(SedonaVM));

  if ((result = commonVmSetup(&vm, filename)) != 0)
    return result;

  // setup arguments
  vm.args = (const char**)(vmArgv);
  vm.argsLen = vmArgc;

  // run the VM
  t1 = sys_Sys_ticks(NULL, NULL);
  result = vmRun(&vm);

  while ((result == ERR_HIBERNATE) || (result == ERR_YIELD))
  {
    //  Simulate hibernate and yield    
    if (result == ERR_HIBERNATE)
    {  
      printf("-- Simulated hibernate --\n");             
    }  
    else
    { 
      printf("Simulate yield %lld ", yieldNs); 
      sys_Sys_sleep(NULL, (Cell*)&yieldNs);   
      yieldNs = 0;   
    }
    result = vmResume(&vm);
  }


  // done
  if (result != 0)
  {
    printf("Cannot run VM (%d)\n", result);
    return result;
  }
  t2 = sys_Sys_ticks(NULL, NULL);

  printf("\n");
  printf("VM Completed\n");
#ifdef _WIN32
  printf("Total Time       = %I64d ms\n", (t2-t1)/1000000i64);
#else
  printf("Total Time       = %lld ms\n", (t2-t1)/1000000ll);
#endif
  printf("Assert Successes = %d\n", vm.assertSuccesses);
  printf("Assert Failures  = %d\n", vm.assertFailures);
  printf("\n");
  if (vm.assertFailures == 0)
  {
    printf("--\n");
    printf("-- All tests passed\n");
    printf("--\n");
    return 0;
  }
  else
  {
    printf("--\n");
    printf("-- %d TESTS FAILED!!!\n", vm.assertFailures);
    printf("--\n");
    return vm.assertFailures;
  }

  return result;
}

////////////////////////////////////////////////////////////////
// Common VM Setup
////////////////////////////////////////////////////////////////

/**
 *
 * This function initialize the following fields of the VM.
 * 1) stackMaxSize
 * 2) stackBaseAddr
 * 3) onAssertFailure callback
 * 4) nativeTable
 * 5) call function pointer
 *
 * The following fields will still need to be initialized before
 * the VM can be run.
 * 1) args
 * 2) argsLen
 */
static int commonVmSetup(SedonaVM* vm, const char* scodeFile)
{
  int result;

  if (vm->codeBaseAddr!=NULL) free((void*)vm->codeBaseAddr);   // Avoid memory leak

  // load scode
  result = loadFile(scodeFile, (uint8_t**)&(vm->codeBaseAddr), &(vm->codeSize));
  if (result != 0)
  {
    printf("Cannot load input file (%d): %s\n", result, scodeFile);
    return result;
  }

  if (vm->stackBaseAddr!=NULL) free((void*)vm->stackBaseAddr);  // Avoid memory leak

  // alloc stack (hardcoded for now)
  vm->stackMaxSize  = 16384;
  vm->stackBaseAddr = (uint8_t*)malloc(vm->stackMaxSize);
  if (vm->stackBaseAddr == NULL)
  {
    printf("Cannot malloc stack segments\n");
    return ERR_MALLOC_STACK;
  }

  // setup callbacks
  vm->onAssertFailure = onAssertFailure;

  // setup native method table
  vm->nativeTable = nativeTable;

  // setup call function pointer
  vm->call = vmCall;
  return 0;
}

////////////////////////////////////////////////////////////////
// Print Usage
////////////////////////////////////////////////////////////////

static int printUsage(const char* exe)
{
  printf("usage:\n");
  printf("  %s [options] <scode file> [<sab file>] [<Sedona main args>]\n", exe);
  printf("  %s [options] --plat\n", exe);
  printf("options:\n");
  printf("  --?       dump usage\n");
  printf("  --ver     dump version\n");
  printf("  --home=d  set current working directory\n");
  printf("  --plat    run in platform mode. 'kits.scode[.stage]' and 'app.sab[.stage]'\n");
  printf("            must be present in the working directory\n");
  return 0;
}

////////////////////////////////////////////////////////////////
// Print Version
////////////////////////////////////////////////////////////////

#ifndef PLAT_BUILD_VERSION
 #error Must set PLAT_BUILD_VERSION
#endif 

static int printVersion()
{
#ifdef IS_BIG_ENDIAN
  const char* endian = "big";
#else
  const char* endian = "little";
#endif
  printf("\n");
#ifdef __QNX__
  printf("Sedona VM %s\n", VER(PLAT_BUILD_VERSION));
#else
  printf("Sedona VM %s\n", PLAT_BUILD_VERSION);
#endif
 
  printf("buildDate: %s %s\n", __DATE__, __TIME__);
  printf("endian:    %s\n", endian);
  printf("blockSize: %d\n", SCODE_BLOCK_SIZE);
  printf("refSize:   %d\n", sizeof(void*));
  printf("\n");
  return 0;
}

////////////////////////////////////////////////////////////////
// Load File
////////////////////////////////////////////////////////////////

static int loadFile(const char* filename, uint8_t** paddr, size_t* psize)
{
  size_t result;
  FILE* file;
  size_t size;
  uint8_t* addr;

  // open file
  file = fopen(filename, "rb");
  if (file == NULL) return ERR_INPUT_FILE_NOT_FOUND;

  // seek to end to get file size
  result = fseek(file, 0, SEEK_END);
  if (result != 0) return ERR_CANNOT_READ_INPUT_FILE;
  size = ftell(file);
  rewind(file);

  // allocate memory for image
  addr = (uint8_t*)malloc(size);
  if (addr == NULL)
    return ERR_MALLOC_IMAGE;

  // read file into memory
  result = fread(addr, 1, size, file);
  if (result != size) return ERR_CANNOT_READ_INPUT_FILE;

  // success
  *paddr = addr;
  *psize = size;
  fclose(file);
  return 0;
}

//////////////////////////////////////////////////////////////////////////
// Staged Files
//////////////////////////////////////////////////////////////////////////

/**
 * Checks if <filename>.stage exists. If so it is renamed to <filename>.
 */
static void checkStaged(const char* filename)
{
  FILE* file;
  size_t len;
  char* stageName;

  len = strlen(filename);
  stageName = (char*)malloc(len + strlen(".stage") + 1);
  strcpy(stageName, filename);
  strcat(stageName, ".stage");
  if ((file = fopen(stageName, "rb")) != NULL)
  {
    fclose(file);
    printf("Moving %s to %s\n", stageName, filename);
    remove(filename);
    rename(stageName, filename);
  }
  free(stageName);
}

//////////////////////////////////////////////////////////////////////////
// VM Callbacks
//////////////////////////////////////////////////////////////////////////

static void onAssertFailure(const char* location, uint16_t linenum)
{
  printf("ASSERT FAILURE: %s [Line %d]\n", location, linenum);
}

#endif